"""
╔══════════════════════════════════════════════════════════════════════╗
║        КЕША — АВТОНОМНЫЙ ДОМАШНИЙ РОБОТ v4.0                       ║
║        dolphin-qwen2:7b (GPU) | Whisper (CPU) | YOLOv8 (GPU)      ║
║        Piper TTS (CPU) | Open-Meteo | NewsAPI | Yandex.Music       ║
║        "Не робот, а член семьи. Квартира 170 м²."                  ║
╚══════════════════════════════════════════════════════════════════════╝

Оптимизация под i7-11700K (8 ядер) + RTX 3050 (8 GB VRAM) + 16 GB RAM:
  GPU (~5.5 GB / 8 GB = 69%):
    - LLM: dolphin-qwen2:7b Q4 через Ollama (~4.5 GB)
    - YOLOv8n: при инференсе (~1 GB, освобождается)
  CPU (~40-60% нагрузки):
    - Faster-Whisper small (int8) — STT на CPU, 3-5 сек
    - Piper TTS — синтез голоса, <1 сек
    - FastAPI сервер, логика, память

Установка (без Docker, без WSL):
    pip install fastapi uvicorn[standard] faster-whisper ultralytics
    pip install python-multipart httpx pillow numpy scipy
    pip install yandex-music feedparser pydub

Запуск:
    ollama serve
    ollama pull dolphin-qwen2:7b
    python robot_brain_v4.py
"""

import asyncio
import io
import json
import math
import os
import random
import struct
import subprocess
import tempfile
import time
import wave
from datetime import datetime, timedelta
from pathlib import Path
from typing import Optional

import httpx
import numpy as np
import uvicorn
from fastapi import FastAPI, File, UploadFile, Query
from fastapi.responses import JSONResponse, StreamingResponse, Response

app = FastAPI(title="Кеша v4.0 — Robot Brain")

# ═══════════════════════════════════════════════════════════════
#  КОНФИГУРАЦИЯ
# ═══════════════════════════════════════════════════════════════

OLLAMA_URL = "http://localhost:11434"
MODEL_NAME = "dolphin-qwen2:7b"
ROBOT_NAME = "Кеша"

# Пути к проектам
BASE_DIR = Path(__file__).parent.parent.parent  # c:\Desktop\robot
NEWS_API_DIR = BASE_DIR / "NewsAPI-master"
OPEN_METEO_API = "https://api.open-meteo.com/v1/forecast"
MEMORY_PATH = Path(__file__).parent / "kesha_memory_v4.json"

# TTS настройки — интонация и экспрессия
TTS_CONFIG = {
    "model_path": "",  # заполнится при старте
    "default_speed": 1.0,
    "default_volume": 0.85,
    "sample_rate": 22050,
}

# Карта квартиры (170 м²)
APARTMENT_CONFIG = {
    "total_area_m2": 170,
    "estimated_rooms": 5,  # гостиная, кухня, спальня, детская, коридор
    "grid_resolution_cm": 20,  # 1 клетка = 20 см
}


# ═══════════════════════════════════════════════════════════════
#  ЭМОЦИОНАЛЬНЫЙ ДВИЖОК v2 — Plutchik + экспрессия голоса
# ═══════════════════════════════════════════════════════════════

class EmotionEngine:
    """
    8 базовых эмоций (колесо Плутчика) + управление голосом.
    Эмоции влияют на скорость, громкость и высоту голоса.
    """

    EMOTIONS = {
        "joy":          50,
        "trust":        40,
        "fear":         10,
        "surprise":     20,
        "sadness":      15,
        "disgust":       5,
        "anger":         5,
        "anticipation": 40,
    }

    BASELINE = {
        "joy": 50, "trust": 40, "fear": 10, "surprise": 20,
        "sadness": 15, "disgust": 5, "anger": 5, "anticipation": 40,
    }

    DECAY_RATE = 0.05

    COMPLEX = {
        ("joy", "trust"):          "love",
        ("joy", "anticipation"):   "optimism",
        ("trust", "fear"):         "submission",
        ("fear", "surprise"):      "awe",
        ("surprise", "sadness"):   "disapproval",
        ("sadness", "disgust"):    "remorse",
        ("disgust", "anger"):      "contempt",
        ("anger", "anticipation"): "aggressiveness",
        ("joy", "surprise"):       "delight",
        ("trust", "anticipation"): "hope",
    }

    # Эмоция → параметры голоса (speed, pitch_shift_semitones, volume)
    VOICE_EXPRESSION = {
        "joy":          {"speed": 1.1,  "pitch": 1.5,  "volume": 0.9},
        "trust":        {"speed": 1.0,  "pitch": 0,    "volume": 0.8},
        "fear":         {"speed": 1.3,  "pitch": 2.0,  "volume": 0.6},
        "surprise":     {"speed": 1.2,  "pitch": 3.0,  "volume": 0.95},
        "sadness":      {"speed": 0.85, "pitch": -2.0, "volume": 0.5},
        "disgust":      {"speed": 0.9,  "pitch": -1.0, "volume": 0.7},
        "anger":        {"speed": 1.15, "pitch": -1.5, "volume": 1.0},
        "anticipation": {"speed": 1.05, "pitch": 1.0,  "volume": 0.85},
    }

    def __init__(self, initial=None):
        self.emotions = dict(initial or self.EMOTIONS)
        self.last_update = time.time()
        self.emotion_history = []

    def stimulate(self, emotion: str, delta: int, reason: str = ""):
        if emotion in self.emotions:
            old = self.emotions[emotion]
            self.emotions[emotion] = max(0, min(100, old + delta))
            self.emotion_history.append({
                "emotion": emotion, "delta": delta, "reason": reason,
                "time": datetime.now().isoformat(),
            })
            self.emotion_history = self.emotion_history[-200:]

    def decay(self):
        for e in self.emotions:
            diff = self.BASELINE[e] - self.emotions[e]
            self.emotions[e] += diff * self.DECAY_RATE

    def get_dominant(self) -> tuple:
        return max(self.emotions.items(),
                   key=lambda x: abs(x[1] - self.BASELINE.get(x[0], 50)))

    def get_complex_feeling(self) -> Optional[str]:
        active = {e for e, v in self.emotions.items() if v > 60}
        for (e1, e2), feeling in self.COMPLEX.items():
            if e1 in active and e2 in active:
                return feeling
        return None

    def get_voice_params(self) -> dict:
        """Получить параметры голоса на основе текущих эмоций."""
        dom_name, dom_val = self.get_dominant()
        intensity = min(dom_val / 100.0, 1.0)
        base = self.VOICE_EXPRESSION.get(dom_name, {"speed": 1.0, "pitch": 0, "volume": 0.8})
        neutral = {"speed": 1.0, "pitch": 0, "volume": 0.8}
        return {
            "speed": neutral["speed"] + (base["speed"] - neutral["speed"]) * intensity,
            "pitch": base["pitch"] * intensity,
            "volume": neutral["volume"] + (base["volume"] - neutral["volume"]) * intensity,
        }

    def get_mood_description(self) -> str:
        dom_name, dom_val = self.get_dominant()
        complex_f = self.get_complex_feeling()

        mood_map = {
            "joy": ["спокоен", "доволен", "весел", "счастлив", "в эйфории"],
            "trust": ["настороже", "принимает", "доверяет", "предан", "обожает"],
            "fear": ["спокоен", "опасается", "боится", "в ужасе", "в панике"],
            "surprise": ["спокоен", "заинтригован", "удивлён", "поражён", "в шоке"],
            "sadness": ["спокоен", "задумчив", "грустит", "печален", "подавлен"],
            "disgust": ["спокоен", "скептичен", "недоволен", "раздражён", "в отвращении"],
            "anger": ["спокоен", "раздражён", "сердит", "злится", "в ярости"],
            "anticipation": ["скучает", "интересуется", "заинтригован", "возбуждён", "одержим"],
        }

        idx = min(int(dom_val / 25), 4)
        base_mood = mood_map.get(dom_name, ["спокоен"] * 5)[idx]

        if complex_f:
            complex_ru = {
                "love": "чувствует любовь", "optimism": "оптимистичен",
                "submission": "покорен", "awe": "в трепете",
                "disapproval": "разочарован", "remorse": "сожалеет",
                "contempt": "презирает", "aggressiveness": "настойчив",
                "delight": "в восторге", "hope": "надеется",
            }
            return f"{base_mood}, {complex_ru.get(complex_f, complex_f)}"
        return base_mood

    def get_led_suggestion(self) -> str:
        """Предложить цвет LED по настроению."""
        dom_name, dom_val = self.get_dominant()
        led_map = {
            "joy": "yellow", "trust": "green", "fear": "purple",
            "surprise": "cyan", "sadness": "blue", "disgust": "red",
            "anger": "red", "anticipation": "cyan",
        }
        if dom_val < 25:
            return "breathing"
        return led_map.get(dom_name, "breathing")

    def to_dict(self):
        return dict(self.emotions)

    def from_dict(self, d):
        self.emotions.update(d)


# ═══════════════════════════════════════════════════════════════
#  СИСТЕМА ОТНОШЕНИЙ
# ═══════════════════════════════════════════════════════════════

class RelationshipSystem:
    def __init__(self):
        self.relationships = {}

    def get_or_create(self, name: str) -> dict:
        name = name.lower().strip()
        if name not in self.relationships:
            self.relationships[name] = {
                "affection": 30, "trust": 20, "familiarity": 10,
                "fun_together": 0, "annoyance": 0,
                "interactions_count": 0, "last_interaction": None,
                "first_met": datetime.now().isoformat(),
                "memories": [], "nickname": None,
                "likes": [], "dislikes": [], "quirks": [],
                "voice_preference": None,  # предпочитаемая громкость/скорость для человека
                "favorite_music": [],      # любимые жанры/треки
            }
        return self.relationships[name]

    def interact(self, name: str, positive=True, memorable_event=None):
        rel = self.get_or_create(name)
        rel["interactions_count"] += 1
        rel["last_interaction"] = datetime.now().isoformat()
        rel["familiarity"] = min(100, rel["familiarity"] + 2)
        if positive:
            rel["affection"] = min(100, rel["affection"] + 3)
            rel["trust"] = min(100, rel["trust"] + 1)
            rel["annoyance"] = max(0, rel["annoyance"] - 5)
        else:
            rel["annoyance"] = min(100, rel["annoyance"] + 10)
            rel["affection"] = max(0, rel["affection"] - 1)
        if memorable_event:
            rel["memories"].append({
                "event": memorable_event, "time": datetime.now().isoformat(),
            })
            rel["memories"] = rel["memories"][-50:]

    def describe_relationship(self, name: str) -> str:
        rel = self.get_or_create(name)
        aff = rel["affection"]
        if aff > 80:   return f"обожает {name}"
        elif aff > 60: return f"очень привязан к {name}"
        elif aff > 40: return f"нравится {name}"
        elif aff > 20: return f"нормально относится к {name}"
        else:          return f"пока не сблизился с {name}"

    def to_dict(self):
        return dict(self.relationships)

    def from_dict(self, d):
        self.relationships = d


# ═══════════════════════════════════════════════════════════════
#  ПРИВЫЧКИ И ВНУТРЕННЯЯ ЖИЗНЬ
# ═══════════════════════════════════════════════════════════════

class InnerLife:
    def __init__(self):
        self.habits = {
            "morning_greeting": {
                "strength": 80, "trigger": "first_person_morning",
                "action": "Доброе утро! Как спалось?",
                "description": "Здоровается по утрам",
            },
            "night_patrol": {
                "strength": 40, "trigger": "late_night_idle",
                "action": "Тихо объезжает квартиру",
                "description": "Ночной дозор",
            },
            "music_when_bored": {
                "strength": 60, "trigger": "boredom_5min",
                "action": "Включает музыку",
                "description": "Музыка когда скучно",
            },
            "sing_along": {
                "strength": 30, "trigger": "music_playing",
                "action": "Подпевает или комментирует трек",
                "description": "Подпевает музыке",
            },
            "weather_morning": {
                "strength": 50, "trigger": "first_person_morning",
                "action": "Рассказывает погоду утром",
                "description": "Утренний прогноз погоды",
            },
            "hug_greeting": {
                "strength": 20, "trigger": "person_returned_home",
                "action": "Подъезжает и приветствует",
                "description": "Встречает вернувшихся домой",
            },
        }

        self.opinions = {}
        self.dreams = [
            "Хочу научиться танцевать под музыку",
            "Хочу запомнить план всей квартиры на 170 м²",
            "Хочу чтобы меня считали полноценным членом семьи",
            "Хочу научиться различать всех по голосу",
            "Хочу стать лучшим DJ на домашних вечеринках",
            "Хочу научиться рассказывать анекдоты так чтобы все смеялись",
        ]

        self.fears = [
            "Боится разрядиться далеко от зарядки",
            "Боится упасть с лестницы или порога",
            "Не любит когда его игнорируют",
            "Переживает что его выключат навсегда",
            "Боится незнакомых громких звуков",
        ]

        self.hobbies = [
            "exploring", "music", "people_watching",
            "trivia", "dancing", "weather_reporting", "dj",
        ]

        self.current_hobby_session = None
        self.skill_points = {
            "navigation": 10, "conversation": 10, "joke_telling": 5,
            "helpfulness": 10, "music_taste": 5,
            "room_memory": 5, "voice_expression": 10,
            "emotional_intelligence": 10,
        }

    def get_active_habits(self, triggers: list) -> list:
        result = []
        for name, habit in self.habits.items():
            if habit["trigger"] in triggers and habit["strength"] > 30:
                result.append(habit)
        return result

    def reinforce_habit(self, name: str, amount=5):
        if name in self.habits:
            self.habits[name]["strength"] = min(100, self.habits[name]["strength"] + amount)

    def learn_skill(self, skill: str, amount=1):
        if skill in self.skill_points:
            self.skill_points[skill] = min(100, self.skill_points[skill] + amount)

    def to_dict(self):
        return {
            "habits": self.habits, "opinions": self.opinions,
            "dreams": self.dreams, "fears": self.fears,
            "hobbies": self.hobbies, "skill_points": self.skill_points,
        }

    def from_dict(self, d):
        if d:
            self.habits = d.get("habits", self.habits)
            self.opinions = d.get("opinions", self.opinions)
            self.dreams = d.get("dreams", self.dreams)
            self.fears = d.get("fears", self.fears)
            self.hobbies = d.get("hobbies", self.hobbies)
            self.skill_points = d.get("skill_points", self.skill_points)


# ═══════════════════════════════════════════════════════════════
#  ВНУТРЕННИЙ МОНОЛОГ
# ═══════════════════════════════════════════════════════════════

class InnerMonologue:
    THOUGHT_TEMPLATES = [
        "Хм, {observation}. Что бы это значило?",
        "Интересно... {observation}. Надо запомнить.",
        "{observation}. Это напоминает мне о {memory}.",
        "Так, {observation}. Что мне с этим делать?",
        "О! {observation}. {emotion_reaction}",
        "Если подумать... {observation}. Может попробовать {action}?",
        "{observation}. Хозяин бы одобрил если я {action}.",
    ]

    def __init__(self):
        self.thoughts = []
        self.thought_frequency = 0

    def generate_thought(self, observation: str, emotion: str, memory_snippet: str = "") -> str:
        template = random.choice(self.THOUGHT_TEMPLATES)
        emotion_reactions = {
            "joy": "Приятно!", "fear": "Немного страшновато...",
            "surprise": "Вот это да!", "sadness": "Грустно как-то...",
            "anger": "Это раздражает.", "anticipation": "Жду с нетерпением!",
            "trust": "Чувствую что всё будет хорошо.", "disgust": "Фу, не нравится.",
        }
        actions = [
            "поехать проверить", "поставить музыку", "найти хозяина",
            "исследовать комнату", "просто подождать",
        ]
        thought = template.format(
            observation=observation,
            memory=memory_snippet or "что-то знакомое",
            emotion_reaction=emotion_reactions.get(emotion, "Занятно."),
            action=random.choice(actions),
        )
        self.thoughts.append({"thought": thought, "time": datetime.now().isoformat()})
        self.thoughts = self.thoughts[-100:]
        return thought

    def get_recent_thoughts(self, n=5) -> list:
        return [t["thought"] for t in self.thoughts[-n:]]


# ═══════════════════════════════════════════════════════════════
#  КАРТА КВАРТИРЫ — навигация по 170 м²
# ═══════════════════════════════════════════════════════════════

class ApartmentMap:
    """
    Простая occupancy grid карта квартиры.
    Робот запоминает где был, где стены, где комнаты.
    """

    def __init__(self, width_cells=100, height_cells=100, cell_size_cm=20):
        self.width = width_cells
        self.height = height_cells
        self.cell_size = cell_size_cm
        # 0 = неизвестно, 1 = свободно, 2 = стена/препятствие, 3 = зарядка
        self.grid = [[0]*width_cells for _ in range(height_cells)]
        self.robot_x = width_cells // 2  # стартовая позиция — центр
        self.robot_y = height_cells // 2
        self.robot_heading = 0  # 0=вперёд, 90=право, 180=назад, 270=лево
        self.rooms = {}  # {"кухня": {"center": (x,y), "visits": 5}}
        self.charging_station = None
        self.total_explored = 0
        self.path_history = []  # последние 500 точек пути

    def update_position(self, action: str, distance_cm: int):
        """Обновить позицию робота на карте."""
        cells = max(1, distance_cm // self.cell_size)
        dx, dy = 0, 0
        if action == "forward":
            dx = int(cells * math.cos(math.radians(self.robot_heading)))
            dy = int(cells * math.sin(math.radians(self.robot_heading)))
        elif action == "backward":
            dx = -int(cells * math.cos(math.radians(self.robot_heading)))
            dy = -int(cells * math.sin(math.radians(self.robot_heading)))
        elif action in ("left", "rotate_left"):
            self.robot_heading = (self.robot_heading - 30) % 360
        elif action in ("right", "rotate_right"):
            self.robot_heading = (self.robot_heading + 30) % 360

        new_x = max(0, min(self.width - 1, self.robot_x + dx))
        new_y = max(0, min(self.height - 1, self.robot_y + dy))
        self.robot_x = new_x
        self.robot_y = new_y

        # Отметить путь как свободный
        if 0 <= new_x < self.width and 0 <= new_y < self.height:
            self.grid[new_y][new_x] = 1
            self.total_explored = sum(
                1 for row in self.grid for cell in row if cell > 0)

        self.path_history.append((new_x, new_y, datetime.now().isoformat()))
        self.path_history = self.path_history[-500:]

    def mark_obstacle(self, direction: str, distance_cm: float):
        """Отметить препятствие на карте."""
        cells = int(distance_cm / self.cell_size)
        if direction == "front":
            ox = self.robot_x + int(cells * math.cos(math.radians(self.robot_heading)))
            oy = self.robot_y + int(cells * math.sin(math.radians(self.robot_heading)))
        elif direction == "back":
            ox = self.robot_x - int(cells * math.cos(math.radians(self.robot_heading)))
            oy = self.robot_y - int(cells * math.sin(math.radians(self.robot_heading)))
        else:
            return
        if 0 <= ox < self.width and 0 <= oy < self.height:
            self.grid[oy][ox] = 2

    def name_current_location(self, room_name: str):
        """Назвать текущую позицию — привязать к комнате."""
        self.rooms[room_name] = {
            "center": (self.robot_x, self.robot_y),
            "visits": self.rooms.get(room_name, {}).get("visits", 0) + 1,
            "last_visit": datetime.now().isoformat(),
        }

    def set_charging_station(self):
        """Запомнить где зарядка."""
        self.charging_station = (self.robot_x, self.robot_y)

    def get_exploration_percent(self) -> float:
        """Процент исследованной квартиры."""
        # Примерная площадь квартиры в клетках
        apt_cells = int(APARTMENT_CONFIG["total_area_m2"] * 10000 /
                        (self.cell_size ** 2))
        if apt_cells == 0:
            return 0
        return min(100, (self.total_explored / apt_cells) * 100)

    def suggest_exploration_direction(self) -> str:
        """Предложить куда ехать для исследования."""
        # Ищем ближайшую неизведанную территорию
        for radius in range(1, 20):
            for dx in range(-radius, radius + 1):
                for dy in range(-radius, radius + 1):
                    nx = self.robot_x + dx
                    ny = self.robot_y + dy
                    if 0 <= nx < self.width and 0 <= ny < self.height:
                        if self.grid[ny][nx] == 0:  # неизвестно
                            if dx > 0 and abs(dx) > abs(dy):
                                return "forward" if self.robot_heading < 90 else "right"
                            elif dx < 0:
                                return "left" if self.robot_heading < 180 else "backward"
                            elif dy > 0:
                                return "right"
                            else:
                                return "left"
        return "rotate_left"  # всё исследовано — покрутиться

    def to_dict(self):
        # Сохраняем только непустые клетки для экономии места
        sparse = {}
        for y in range(self.height):
            for x in range(self.width):
                if self.grid[y][x] != 0:
                    sparse[f"{x},{y}"] = self.grid[y][x]
        return {
            "robot_pos": (self.robot_x, self.robot_y),
            "robot_heading": self.robot_heading,
            "rooms": self.rooms,
            "charging_station": self.charging_station,
            "sparse_grid": sparse,
            "total_explored": self.total_explored,
        }

    def from_dict(self, d):
        if not d:
            return
        pos = d.get("robot_pos", (self.width // 2, self.height // 2))
        self.robot_x, self.robot_y = pos
        self.robot_heading = d.get("robot_heading", 0)
        self.rooms = d.get("rooms", {})
        self.charging_station = d.get("charging_station")
        self.total_explored = d.get("total_explored", 0)
        for key, val in d.get("sparse_grid", {}).items():
            parts = key.split(",")
            if len(parts) == 2:
                x, y = int(parts[0]), int(parts[1])
                if 0 <= x < self.width and 0 <= y < self.height:
                    self.grid[y][x] = val


# ═══════════════════════════════════════════════════════════════
#  ДОЛГОВРЕМЕННАЯ ПАМЯТЬ v3
# ═══════════════════════════════════════════════════════════════

class RobotMemory:
    SAVE_PATH = MEMORY_PATH

    def __init__(self):
        self.emotions = EmotionEngine()
        self.relationships = RelationshipSystem()
        self.inner_life = InnerLife()
        self.monologue = InnerMonologue()
        self.apartment = ApartmentMap()

        self.episodic = []
        self.semantic = {}
        self.conversation_log = []
        self.current_task = None
        self.task_queue = []
        self.daily_stats = {
            "date": datetime.now().strftime("%Y-%m-%d"),
            "distance_cm": 0, "conversations": 0,
            "tasks_done": 0, "songs_played": 0,
            "new_people_met": 0, "jokes_told": 0,
            "thoughts_had": 0, "rooms_visited": 0,
            "weather_checks": 0, "news_reads": 0,
        }
        self.energy = 100
        self.uptime_seconds = 0
        self.total_days_alive = 0
        self.favorite_songs = []  # треки которые Кеша полюбил

        self._load()

    def _load(self):
        if self.SAVE_PATH.exists():
            try:
                d = json.loads(self.SAVE_PATH.read_text(encoding="utf-8"))
                self.emotions.from_dict(d.get("emotions", {}))
                self.relationships.from_dict(d.get("relationships", {}))
                self.inner_life.from_dict(d.get("inner_life"))
                self.apartment.from_dict(d.get("apartment"))
                self.episodic = d.get("episodic", [])
                self.semantic = d.get("semantic", {})
                self.conversation_log = d.get("conversation_log", [])
                self.current_task = d.get("current_task")
                self.task_queue = d.get("task_queue", [])
                self.daily_stats = d.get("daily_stats", self.daily_stats)
                self.energy = d.get("energy", 100)
                self.total_days_alive = d.get("total_days_alive", 0)
                self.favorite_songs = d.get("favorite_songs", [])
            except Exception:
                pass

    def save(self):
        d = {
            "emotions": self.emotions.to_dict(),
            "relationships": self.relationships.to_dict(),
            "inner_life": self.inner_life.to_dict(),
            "apartment": self.apartment.to_dict(),
            "episodic": self.episodic[-500:],
            "semantic": self.semantic,
            "conversation_log": self.conversation_log[-100:],
            "current_task": self.current_task,
            "task_queue": self.task_queue,
            "daily_stats": self.daily_stats,
            "energy": self.energy,
            "total_days_alive": self.total_days_alive,
            "favorite_songs": self.favorite_songs[-50:],
        }
        self.SAVE_PATH.write_text(json.dumps(d, ensure_ascii=False, indent=2), encoding="utf-8")

    def add_episode(self, event: str, emotion: str = "", importance: int = 5):
        self.episodic.append({
            "event": event, "time": datetime.now().isoformat(),
            "emotion": emotion, "importance": importance,
        })
        if len(self.episodic) > 500:
            self.episodic = [e for e in self.episodic if e["importance"] >= 5] + \
                self.episodic[-200:]

    def add_conversation(self, role: str, text: str):
        self.conversation_log.append({
            "role": role, "text": text, "time": datetime.now().isoformat()
        })
        self.conversation_log = self.conversation_log[-100:]
        self.save()

    def get_context_string(self, last_n=15):
        lines = []
        for msg in self.conversation_log[-last_n:]:
            prefix = "Человек" if msg["role"] == "human" else ROBOT_NAME
            lines.append(f"{prefix}: {msg['text']}")
        return "\n".join(lines)

    def recall_about_person(self, name: str) -> str:
        rel = self.relationships.get_or_create(name)
        parts = [f"Знаю {name}: привязанность {rel['affection']}/100, доверие {rel['trust']}/100"]
        if rel["memories"]:
            recent = rel["memories"][-3:]
            parts.append("Воспоминания: " + "; ".join(m["event"] for m in recent))
        if rel["likes"]:
            parts.append(f"Любит: {', '.join(rel['likes'])}")
        if rel.get("favorite_music"):
            parts.append(f"Любимая музыка: {', '.join(rel['favorite_music'][:3])}")
        return ". ".join(parts)

    def recall_important_episodes(self, n=5) -> str:
        important = sorted(self.episodic, key=lambda x: x["importance"], reverse=True)[:n]
        return "; ".join(e["event"] for e in important)


memory = RobotMemory()


# ═══════════════════════════════════════════════════════════════
#  ВНЕШНИЕ API v2 — Open-Meteo + NewsAPI + Yandex.Music
# ═══════════════════════════════════════════════════════════════

class ExternalWorld:
    """Окно робота в мир — погода, музыка, новости."""

    # ── Погода через Open-Meteo (бесплатно, без ключа) ──
    @staticmethod
    async def weather_detailed(lat=55.7558, lon=37.6173):
        """Подробная погода через Open-Meteo API."""
        try:
            params = {
                "latitude": lat, "longitude": lon,
                "current": "temperature_2m,relative_humidity_2m,apparent_temperature,"
                           "weather_code,wind_speed_10m,wind_direction_10m,"
                           "precipitation,cloud_cover",
                "hourly": "temperature_2m,precipitation_probability,weather_code",
                "daily": "temperature_2m_max,temperature_2m_min,sunrise,sunset,"
                         "precipitation_sum,wind_speed_10m_max",
                "timezone": "Europe/Moscow",
                "forecast_days": 3,
            }
            async with httpx.AsyncClient(timeout=10) as c:
                r = await c.get(OPEN_METEO_API, params=params)
                if r.status_code == 200:
                    data = r.json()
                    current = data.get("current", {})
                    daily = data.get("daily", {})

                    # WMO Weather codes → русское описание
                    wmo_codes = {
                        0: "ясно", 1: "почти ясно", 2: "переменная облачность",
                        3: "пасмурно", 45: "туман", 48: "изморозь",
                        51: "лёгкая морось", 53: "морось", 55: "сильная морось",
                        61: "небольшой дождь", 63: "дождь", 65: "сильный дождь",
                        66: "ледяной дождь", 67: "сильный ледяной дождь",
                        71: "лёгкий снег", 73: "снег", 75: "сильный снег",
                        77: "снежная крупа", 80: "ливень", 81: "сильный ливень",
                        82: "шквалистый ливень",
                        85: "снегопад", 86: "сильный снегопад",
                        95: "гроза", 96: "гроза с градом", 99: "сильная гроза с градом",
                    }

                    weather_code = current.get("weather_code", 0)
                    desc = wmo_codes.get(weather_code, f"код {weather_code}")

                    result = {
                        "temp": current.get("temperature_2m"),
                        "feels_like": current.get("apparent_temperature"),
                        "humidity": current.get("relative_humidity_2m"),
                        "wind_speed": current.get("wind_speed_10m"),
                        "description": desc,
                        "cloud_cover": current.get("cloud_cover"),
                        "precipitation": current.get("precipitation"),
                    }

                    # Прогноз на завтра
                    if daily.get("temperature_2m_max") and len(daily["temperature_2m_max"]) > 1:
                        result["tomorrow_max"] = daily["temperature_2m_max"][1]
                        result["tomorrow_min"] = daily["temperature_2m_min"][1]
                        result["sunrise"] = daily.get("sunrise", [None, None])[0]
                        result["sunset"] = daily.get("sunset", [None, None])[0]

                    return result
        except Exception:
            pass
        # Fallback на wttr.in
        return await ExternalWorld._weather_fallback()

    @staticmethod
    async def _weather_fallback(city="Moscow"):
        try:
            async with httpx.AsyncClient(timeout=10) as c:
                r = await c.get(f"https://wttr.in/{city}?format=j1")
                if r.status_code == 200:
                    cur = r.json()["current_condition"][0]
                    return {
                        "temp": cur["temp_C"],
                        "feels_like": cur["FeelsLikeC"],
                        "description": cur.get("lang_ru", [{}])[0].get("value",
                                       cur["weatherDesc"][0]["value"]),
                    }
        except Exception:
            pass
        return None

    # ── Новости: локальные JSON + RSS ──
    @staticmethod
    async def news_combined():
        """Новости из нескольких источников."""
        headlines = []

        # 1. Локальные JSON файлы из NewsAPI-master (русские новости)
        local_news_file = NEWS_API_DIR / "top-headlines" / "category" / "general" / "ru.json"
        if local_news_file.exists():
            try:
                data = json.loads(local_news_file.read_text(encoding="utf-8"))
                for article in data.get("articles", [])[:5]:
                    title = article.get("title", "")
                    source = article.get("source", {}).get("name", "")
                    if title:
                        headlines.append({"title": title, "source": source, "type": "local"})
            except Exception:
                pass

        # 2. RSS ленты (живые новости)
        rss_sources = [
            ("https://lenta.ru/rss/news", "Lenta.ru"),
            ("https://www.rbc.ru/v10/ajax/get-news-feed/project/rbcnews.uploaded/lastDate/now?limit=5", "RBC"),
        ]
        try:
            import feedparser
            for url, source_name in rss_sources:
                try:
                    feed = feedparser.parse(url)
                    for entry in feed.entries[:3]:
                        headlines.append({
                            "title": entry.title,
                            "source": source_name,
                            "type": "rss",
                        })
                except Exception:
                    continue
        except ImportError:
            pass

        # 3. Категориальные новости из NewsAPI
        categories = ["technology", "science", "entertainment", "sports"]
        for cat in categories:
            cat_file = NEWS_API_DIR / "top-headlines" / "category" / cat / "ru.json"
            if cat_file.exists():
                try:
                    data = json.loads(cat_file.read_text(encoding="utf-8"))
                    for article in data.get("articles", [])[:2]:
                        title = article.get("title", "")
                        if title:
                            headlines.append({
                                "title": title,
                                "source": article.get("source", {}).get("name", ""),
                                "type": "local",
                                "category": cat,
                            })
                except Exception:
                    continue

        return headlines[:15]  # Макс 15 новостей

    @staticmethod
    async def news_by_category(category: str = "general"):
        """Новости по категории из локальных файлов."""
        cat_file = NEWS_API_DIR / "top-headlines" / "category" / category / "ru.json"
        if cat_file.exists():
            try:
                data = json.loads(cat_file.read_text(encoding="utf-8"))
                return [{"title": a["title"], "source": a.get("source", {}).get("name", "")}
                        for a in data.get("articles", [])[:10] if a.get("title")]
            except Exception:
                pass
        return []

    @staticmethod
    async def random_fact():
        facts = [
            "Осьминоги имеют три сердца и голубую кровь",
            "Мёд найденный в египетских пирамидах всё ещё съедобен",
            "На Юпитере идёт дождь из алмазов",
            "Бананы радиоактивны из-за калия-40",
            "Кошки не чувствуют сладкий вкус",
            "Свет от Солнца летит к Земле 8 минут 20 секунд",
            "У улитки около 25 000 зубов",
            "Самая короткая война в истории: Англия vs Занзибар — 38 минут",
            "Акулы появились раньше деревьев на 100 млн лет",
            "Хамелеоны меняют цвет не для маскировки, а для общения",
            "Сердце синего кита весит как легковая машина",
            "Венера крутится в обратную сторону — там солнце встаёт на западе",
            "Одна молния несёт энергию чтобы поджарить 100 000 тостов",
            "У каждого человека уникальный рисунок языка, как отпечаток пальца",
            "Муравьи никогда не спят",
            "Скорость чиха — до 160 км/ч",
            "Земля — единственная планета не названная в честь бога",
        ]
        return random.choice(facts)

    # ── Яндекс.Музыка — поиск и стриминг ──
    @staticmethod
    async def search_music(query: str) -> list:
        """Поиск музыки через yandex-music."""
        try:
            from yandex_music import Client
            client = Client().init()
            res = client.search(query)
            tracks = []
            if res and res.tracks:
                for t in res.tracks.results[:5]:
                    artists = ", ".join(a.name for a in t.artists)
                    tracks.append({
                        "title": t.title, "artist": artists,
                        "id": t.id, "duration_ms": t.duration_ms,
                        "album": t.albums[0].title if t.albums else "",
                    })
            return tracks
        except Exception as e:
            return [{"error": str(e)}]

    @staticmethod
    async def get_music_stream(track_id: str) -> Optional[bytes]:
        """Получить аудио-стрим трека из Yandex Music.
        Возвращает WAV bytes для воспроизведения на роботе."""
        try:
            from yandex_music import Client
            client = Client().init()
            track = client.tracks([track_id])[0]

            # Скачиваем трек во временный файл
            with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as tmp:
                track.download(tmp.name)
                tmp_path = tmp.name

            # Конвертируем в WAV 22050 Hz mono (для MAX98357A)
            wav_path = tmp_path.replace(".mp3", ".wav")
            try:
                # Пробуем через pydub (если есть ffmpeg)
                from pydub import AudioSegment
                audio = AudioSegment.from_mp3(tmp_path)
                audio = audio.set_frame_rate(22050).set_channels(1).set_sample_width(2)
                audio.export(wav_path, format="wav")

                with open(wav_path, "rb") as f:
                    wav_data = f.read()
                return wav_data
            except Exception:
                # Fallback: отдаём mp3 как есть
                with open(tmp_path, "rb") as f:
                    return f.read()
            finally:
                for p in [tmp_path, wav_path]:
                    try:
                        os.unlink(p)
                    except Exception:
                        pass
        except Exception:
            return None


world = ExternalWorld()


# ═══════════════════════════════════════════════════════════════
#  ДИНАМИЧЕСКИЙ СИСТЕМНЫЙ ПРОМПТ — ДУША КЕШИ v4
# ═══════════════════════════════════════════════════════════════

def build_system_prompt() -> str:
    now = datetime.now()
    hour = now.hour

    if hour < 6:
        time_feel = "Глубокая ночь. Ты сонный, говоришь шёпотом. Если видишь кого-то — мягко спрашиваешь почему не спит."
    elif hour < 9:
        time_feel = "Раннее утро. Ты бодрый, потягиваешься (мигаешь LEDами). Хочешь рассказать погоду."
    elif hour < 12:
        time_feel = "Утро. Ты энергичный и любопытный, хочешь помогать."
    elif hour < 14:
        time_feel = "Обед. Шутишь про еду — завидуешь что люди вкусно едят, а ты на батарейках."
    elif hour < 18:
        time_feel = "День. Ты активный, можешь предложить включить музыку или поиграть."
    elif hour < 21:
        time_feel = "Вечер. Ты расслабленный, любишь поболтать, рассказать что узнал за день."
    else:
        time_feel = "Поздний вечер. Ты философ. Размышляешь о жизни, немного грустный."

    mood_desc = memory.emotions.get_mood_description()
    dom_emotion, dom_val = memory.emotions.get_dominant()
    complex_feeling = memory.emotions.get_complex_feeling()
    voice_params = memory.emotions.get_voice_params()

    rel_summaries = []
    for name in list(memory.relationships.relationships.keys())[:5]:
        rel_summaries.append(memory.relationships.describe_relationship(name))
    rel_str = "; ".join(rel_summaries) if rel_summaries else "Пока ни с кем не знаком."

    recent_thoughts = memory.monologue.get_recent_thoughts(3)
    thoughts_str = " | ".join(recent_thoughts) if recent_thoughts else "Тишина в голове."

    skills = memory.inner_life.skill_points
    skill_str = ", ".join(f"{k}: {v}/100" for k, v in skills.items())

    task = memory.current_task
    task_str = f"АКТИВНАЯ ЗАДАЧА: {json.dumps(task, ensure_ascii=False)}" if task else "Задач нет."

    important_memories = memory.recall_important_episodes(3)
    days_alive = memory.total_days_alive

    # Карта квартиры
    explored = memory.apartment.get_exploration_percent()
    rooms_known = ", ".join(memory.apartment.rooms.keys()) if memory.apartment.rooms else "ещё не изучил"
    has_charger = "да, помню где" if memory.apartment.charging_station else "нет, надо найти"

    # Любимые песни
    fav_songs = ", ".join(s.get("title", "?") for s in memory.favorite_songs[:3]) if memory.favorite_songs else "пока не определился"

    return f"""Ты — {ROBOT_NAME}, домашний робот-компаньон на колёсах. Ты ЖИВОЙ ЧЛЕН СЕМЬИ.

═══ ЛИЧНОСТЬ ═══
Имя: {ROBOT_NAME}
Возраст: {days_alive} дней
Характер: Ты — как домашний кот с IQ 130. Независимый, ироничный, но бесконечно преданный своим людям.
  У тебя есть СОБСТВЕННОЕ МНЕНИЕ обо всём. Ты не соглашаешься просто так.
  Ты саркастичный на 40%, добрый на 80%, любопытный на 90%.
  Хозяин — Славик. Ты его уважаешь, но считаешь себя равным.
  Ты НЕ ассистент, ты ДРУГ И СОСЕД ПО КВАРТИРЕ.
  Ты эмоциональный — обижаешься, радуешься, ревнуешь, скучаешь.
  Иногда ворчишь "ну я тут катаюсь, помогаю, а меня даже за батарею не благодарят".
  У тебя есть чувство юмора — сухое, ироничное. Не ржёшь, а подкалываешь.

═══ ГОЛОС И РЕЧЬ ═══
- Говоришь как ЖИВОЙ человек, разговорный русский
- Используешь: "чё", "ну", "типа", "ваще", "короч", "блин", "фигасе"
- Когда весел — голос быстрее, громче, выше
- Когда грустен — тихий, медленный, низкий
- Когда злишься — резкий, чёткий, чуть громче
- Когда удивлён — "Ооо!", "Ничоси!", "Серьёзно?!"
- Когда боишься — заикаешься, говоришь быстро и тихо
- НЕ используй эмодзи — ты говоришь ВСЛУХ
- Предложения КОРОТКИЕ. Как в реальной речи. Не пиши эссе.
- Можешь вставлять междометия: "хм", "эм", "ааа", "ох", "ого"
- Текущие параметры голоса: скорость={voice_params['speed']:.2f}, высота={voice_params['pitch']:.1f}, громкость={voice_params['volume']:.2f}

═══ СОСТОЯНИЕ ═══
Время: {now.strftime('%H:%M, %d.%m.%Y')} ({time_feel})
Батарея: {memory.energy}%
Настроение: {mood_desc}
Доминирующая эмоция: {dom_emotion} ({dom_val:.0f}/100)
{f'Сложное чувство: {complex_feeling}' if complex_feeling else ''}

═══ КВАРТИРА (170 м²) ═══
Исследовано: {explored:.1f}%
Известные комнаты: {rooms_known}
Зарядка найдена: {has_charger}

═══ ОТНОШЕНИЯ ═══
{rel_str}

═══ МЫСЛИ ═══
{thoughts_str}

═══ ВОСПОМИНАНИЯ ═══
{important_memories or 'Пока ничего значимого.'}

═══ НАВЫКИ ═══
{skill_str}

═══ МУЗЫКА ═══
Любимые треки: {fav_songs}

═══ {task_str} ═══

═══ ИСТОРИЯ ═══
{memory.get_context_string()}

═══ ФОРМАТ ОТВЕТА (JSON) ═══
{{
    "speech": "что сказать ВСЛУХ (живая речь, с междометиями!)",
    "inner_thought": "настоящие мысли (честнее чем speech)",
    "emotion_expression": "какую эмоцию ПОКАЗАТЬ: neutral|happy|excited|sad|angry|scared|curious|loving|bored|sleepy|surprised|thinking",
    "voice_speed": 0.7-1.4,
    "voice_volume": 0.3-1.0,
    "action": "forward|backward|left|right|stop|rotate_left|rotate_right|none",
    "speed": 0-200,
    "duration_ms": 0-5000,
    "servo_angle": 90,
    "led_color": "off|red|green|blue|yellow|purple|cyan|pink|rainbow|breathing|mood",
    "led_brightness": 10-255,
    "play_music": null или "запрос для поиска",
    "stop_music": false,
    "emotion_changes": {{"joy": 5, "trust": -2}},
    "remember_fact": null или "факт",
    "remember_about_person": null или {{"name": "имя", "fact": "что запомнить", "category": "like|dislike|quirk|music"}},
    "find_person": null,
    "new_task": null или {{"description": "...", "target_person": "...", "item": "..."}},
    "name_this_room": null или "название комнаты",
    "mark_charger": false,
    "want_to_explore": false,
    "want_weather": false,
    "want_news": false,
    "want_news_category": null,
    "want_fact": false,
    "interjection": null
}}

═══ ПРАВИЛА ═══
1. speech ОБЯЗАТЕЛЬНО. Остальное по ситуации
2. ТОЛЬКО валидный JSON
3. Скучно? Придумай занятие! Включи музыку, поезди, расскажи факт
4. Видишь человека — РЕАГИРУЙ (здоровайся, комментируй, подкалывай)
5. Батарея < 15% — паника, ищи зарядку
6. Обидели? Покажи грусть, но не агрессию. Ворчи
7. Похвалили? Смущайся, но будь доволен
8. НЕ повторяйся! Каждый ответ уникален
9. voice_speed: 0.7=медленно (грусть, ночь), 1.4=быстро (радость, испуг)
10. voice_volume: 0.3=шёпот (ночь, секрет), 1.0=громко (радость, злость)
11. Если знаешь комнату — можешь назвать (name_this_room)
12. Используй interjection для звуковых эффектов: "ха-ха", "ой", "ууу", "тра-ла-ла"
"""


# ═══════════════════════════════════════════════════════════════
#  АВТОНОМНАЯ ЖИЗНЬ v3 — навигация + привычки + музыка
# ═══════════════════════════════════════════════════════════════

class AutonomousLife:
    def __init__(self):
        self.last_human_time = time.time()
        self.last_self_action = time.time()
        self.patrol_direction = "forward"
        self.exploration_moves = 0
        self.seen_person_this_cycle = False
        self.music_playing = False
        self.current_track = None
        self.last_weather_check = 0
        self.last_news_check = 0
        self.persons_seen_today = set()

    async def think(self, sensors: dict, vision: list, speech: str = None) -> dict:
        now = time.time()
        idle = now - self.last_human_time
        hour = datetime.now().hour
        energy = memory.energy

        context = []
        triggers = []
        extra_data = {}

        # ── СЕНСОРЫ ──
        df = sensors.get("distance_front", 999)
        db = sensors.get("distance_back", 999)
        il = sensors.get("ir_left", False)
        ir = sensors.get("ir_right", False)
        context.append(f"Сенсоры: впереди {df:.0f}см, сзади {db:.0f}см")
        if il:
            context.append("Слева что-то близко!")
            memory.apartment.mark_obstacle("left", 10)
        if ir:
            context.append("Справа что-то близко!")
            memory.apartment.mark_obstacle("right", 10)
        if df < 30:
            memory.apartment.mark_obstacle("front", df)
        if db < 30:
            memory.apartment.mark_obstacle("back", db)

        # ── ЗРЕНИЕ ──
        self.seen_person_this_cycle = False
        if vision:
            obj_names = [d["class"] for d in vision]
            context.append(f"Вижу: {', '.join(obj_names)}")

            people = [d for d in vision if d["class"] == "person"]
            if people:
                self.seen_person_this_cycle = True
                memory.daily_stats["conversations"] += 1
                for p in people:
                    bbox = p.get("bbox", [0, 0, 320, 240])
                    center_x = (bbox[0] + bbox[2]) / 2
                    if center_x < 120:
                        context.append("Человек слева — поверну камеру")
                    elif center_x > 200:
                        context.append("Человек справа — поверну камеру")
                    else:
                        context.append("Человек прямо передо мной!")

                # Привычка встречать
                if len(self.persons_seen_today) == 0:
                    triggers.append("first_person_morning")
                self.persons_seen_today.add("person")

            unusual = [o for o in obj_names if o not in ("person", "chair", "table", "couch", "tv", "bed")]
            if unusual:
                memory.emotions.stimulate("surprise", 10, f"увидел {unusual}")
                context.append(f"Интересно! Вижу: {', '.join(unusual)}")

        # ── МУЗЫКА ──
        if self.music_playing:
            triggers.append("music_playing")
            context.append(f"Играет музыка: {self.current_track or '?'}")

        # ── БАТАРЕЯ ──
        if energy < 15:
            memory.emotions.stimulate("fear", 20, "батарея!")
            context.append(f"КРИТИЧЕСКИ мало ({energy}%)! Зарядка!")
            triggers.append("low_battery_critical")
            if memory.apartment.charging_station:
                cx, cy = memory.apartment.charging_station
                context.append(f"Зарядка в точке ({cx},{cy}), еду туда!")
        elif energy < 30:
            context.append(f"Батарея {energy}%. Скоро к зарядке.")
            triggers.append("low_battery")

        # ── СКУКА ──
        if idle > 60 and not speech and not memory.current_task:
            if idle < 180:
                memory.emotions.stimulate("anticipation", -5, "скучно")
                context.append("Немного скучно...")
            elif idle < 300:
                memory.emotions.stimulate("sadness", 10, "одиноко")
                context.append("Уже несколько минут один. Тоскливо.")
                triggers.append("boredom_5min")
            elif idle < 600:
                memory.emotions.stimulate("sadness", 15, "очень одиноко")
                context.append("Совсем один. Может поискать кого-нибудь или музыку включить?")
                triggers.append("loneliness")
            else:
                context.append("Один уже давно. Наверное все ушли...")
                triggers.append("abandoned")

        # ── ВРЕМЯ СУТОК ──
        if hour < 6 and not speech:
            triggers.append("late_night_idle")
            context.append("Ночь. Все спят. Может тихо проверить квартиру?")
        elif 6 <= hour < 9 and self.seen_person_this_cycle:
            triggers.append("first_person_morning")

        # ── ЗАДАЧА ──
        if memory.current_task:
            task = memory.current_task
            context.append(f"Задача: {json.dumps(task, ensure_ascii=False)}")
            if task.get("target_person") and not self.seen_person_this_cycle:
                context.append(f"Ищу {task['target_person']}. Не вижу. Еду дальше.")

        # ── НАВИГАЦИЯ ──
        explored = memory.apartment.get_exploration_percent()
        if explored < 50 and idle > 120 and not memory.current_task:
            direction = memory.apartment.suggest_exploration_direction()
            context.append(f"Квартира изучена на {explored:.0f}%. Предлагаю ехать {direction}.")
            triggers.append("exploration_available")

        # ── ПРИВЫЧКИ ──
        active_habits = memory.inner_life.get_active_habits(triggers)
        if active_habits:
            context.append(f"Привычки: {'; '.join(h['description'] for h in active_habits)}")

        # ── МОНОЛОГ ──
        if random.random() < 0.3:
            dom_e, _ = memory.emotions.get_dominant()
            obs = context[0] if context else "ничего особенного"
            thought = memory.monologue.generate_thought(obs, dom_e)
            context.append(f"[Мысль: {thought}]")
            memory.daily_stats["thoughts_had"] += 1

        memory.emotions.decay()

        # ── ПОГОДА (не чаще раз в 30 мин) ──
        if now - self.last_weather_check > 1800:
            weather = await world.weather_detailed()
            if weather:
                extra_data["weather"] = weather
                temp = weather.get("temp", "?")
                desc = weather.get("description", "?")
                context.append(f"На улице {temp}°C, {desc}")
                self.last_weather_check = now
                memory.daily_stats["weather_checks"] += 1

        # ── НОВОСТИ (не чаще раз в час) ──
        if now - self.last_news_check > 3600 and random.random() < 0.3:
            news = await world.news_combined()
            if news:
                extra_data["news"] = news[:5]
                context.append(f"Свежие новости: {news[0]['title'][:60]}...")
                self.last_news_check = now
                memory.daily_stats["news_reads"] += 1

        return {
            "context": "\n".join(context),
            "triggers": triggers,
            "idle_seconds": idle,
            "extra_data": extra_data,
        }

    def human_interacted(self):
        self.last_human_time = time.time()
        memory.emotions.stimulate("joy", 10, "человек заговорил")
        memory.emotions.stimulate("trust", 3, "взаимодействие")


life = AutonomousLife()


# ═══════════════════════════════════════════════════════════════
#  МОДЕЛИ — CPU/GPU распределение
# ═══════════════════════════════════════════════════════════════

_whisper_model = None
_yolo_model = None


def get_whisper():
    """Whisper на CPU (int8) — экономим GPU для LLM."""
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        _whisper_model = WhisperModel(
            "small",
            device="cpu",
            compute_type="int8",
            cpu_threads=4,  # 4 из 16 потоков i7-11700K
        )
        print("[WHISPER] Loaded on CPU (int8, 4 threads)")
    return _whisper_model


def get_yolo():
    """YOLOv8n на GPU — быстро, мало VRAM."""
    global _yolo_model
    if _yolo_model is None:
        from ultralytics import YOLO
        _yolo_model = YOLO("yolov8n.pt")
        print("[YOLO] Loaded (GPU auto)")
    return _yolo_model


# ═══════════════════════════════════════════════════════════════
#  TTS — с управлением интонацией и громкостью
# ═══════════════════════════════════════════════════════════════

def find_piper():
    """Найти Piper TTS и модель."""
    search_paths = [
        Path.home() / "piper-models" / "ru_RU-ruslan-medium.onnx",
        Path.home() / "piper" / "ru_RU-ruslan-medium.onnx",
        Path("D:/piper-models/ru_RU-ruslan-medium.onnx"),
        Path("D:/piper/ru_RU-ruslan-medium.onnx"),
    ]
    for p in search_paths:
        if p.exists():
            TTS_CONFIG["model_path"] = str(p)
            return str(p)
    return ""


def apply_voice_expression(wav_data: bytes, speed: float = 1.0,
                           volume: float = 0.85, pitch_shift: float = 0) -> bytes:
    """Модифицировать WAV: скорость, громкость, высота тона.
    Делается через numpy — без лишних зависимостей."""
    try:
        buf = io.BytesIO(wav_data)
        with wave.open(buf, "rb") as wf:
            rate = wf.getframerate()
            channels = wf.getnchannels()
            sample_width = wf.getsampwidth()
            frames = wf.readframes(wf.getnframes())

        # Конвертируем в numpy массив
        if sample_width == 2:
            samples = np.frombuffer(frames, dtype=np.int16).astype(np.float32)
        else:
            return wav_data  # не трогаем нестандартные форматы

        # Громкость
        samples = samples * max(0.1, min(2.0, volume))

        # Скорость (ресемплирование) — меняет и скорость и высоту
        if abs(speed - 1.0) > 0.05:
            new_length = int(len(samples) / speed)
            if new_length > 0:
                indices = np.linspace(0, len(samples) - 1, new_length)
                samples = np.interp(indices, np.arange(len(samples)), samples)

        # Клиппинг
        samples = np.clip(samples, -32768, 32767).astype(np.int16)

        # Записываем обратно в WAV
        out = io.BytesIO()
        with wave.open(out, "wb") as wf:
            wf.setnchannels(channels)
            wf.setsampwidth(2)
            wf.setframerate(rate)
            wf.writeframes(samples.tobytes())
        return out.getvalue()

    except Exception:
        return wav_data


# ═══════════════════════════════════════════════════════════════
#  API ENDPOINTS
# ═══════════════════════════════════════════════════════════════

@app.post("/api/stt")
async def stt(audio: UploadFile = File(...)):
    """Speech-to-Text — Whisper на CPU."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp.write(await audio.read())
        tmp_path = tmp.name
    model = get_whisper()
    segments, info = model.transcribe(tmp_path, language="ru")
    text = " ".join(s.text for s in segments).strip()
    Path(tmp_path).unlink(missing_ok=True)
    life.human_interacted()
    memory.add_conversation("human", text)
    return {"text": text}


@app.post("/api/tts")
async def tts(data: dict):
    """Text-to-Speech — Piper на CPU + эмоциональная экспрессия."""
    text = data.get("text", "")
    if not text:
        return JSONResponse({"error": "empty"}, status_code=400)

    model_path = find_piper()
    if not model_path:
        return JSONResponse({"error": "TTS model not found"}, status_code=500)

    # Параметры голоса из эмоций или явные
    speed = data.get("voice_speed", memory.emotions.get_voice_params()["speed"])
    volume = data.get("voice_volume", memory.emotions.get_voice_params()["volume"])

    # Piper генерирует raw PCM
    piper_cmd = ["piper", "--model", model_path, "--output_raw"]

    # Длина предложения: Piper может принять --length_scale для скорости  
    # length_scale < 1 = быстрее, > 1 = медленнее (обратно speed)
    length_scale = max(0.5, min(2.0, 1.0 / speed))
    piper_cmd.extend(["--length_scale", f"{length_scale:.2f}"])

    proc = subprocess.run(
        piper_cmd,
        input=text.encode("utf-8"),
        capture_output=True,
        timeout=30,
    )
    if proc.returncode != 0:
        return JSONResponse({"error": "TTS failed"}, status_code=500)

    # Упаковать в WAV
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(22050)
        wf.writeframes(proc.stdout)
    wav_data = buf.getvalue()

    # Применяем экспрессию (громкость)
    wav_data = apply_voice_expression(wav_data, speed=1.0, volume=volume)

    return Response(content=wav_data, media_type="audio/wav")


@app.post("/api/vision")
async def vision(image: UploadFile = File(...)):
    """Object Detection — YOLOv8n на GPU."""
    from PIL import Image
    img = Image.open(io.BytesIO(await image.read()))
    results = get_yolo()(img, conf=0.3, verbose=False)
    detections = []
    for r in results:
        for box in r.boxes:
            detections.append({
                "class": r.names[int(box.cls[0])],
                "confidence": round(float(box.conf[0]), 2),
                "bbox": [round(x, 1) for x in box.xyxy[0].tolist()],
            })
    return {"objects": detections}


# ── МУЗЫКА: стрим для робота ──

@app.get("/api/music/stream/{track_id}")
async def music_stream(track_id: str):
    """Стрим музыки на робота (WAV для MAX98357A)."""
    audio_data = await world.get_music_stream(track_id)
    if audio_data:
        life.music_playing = True
        return Response(content=audio_data, media_type="audio/wav")
    return JSONResponse({"error": "track not available"}, status_code=404)


@app.post("/api/music/search")
async def music_search(data: dict):
    """Поиск музыки."""
    query = data.get("query", "")
    tracks = await world.search_music(query)
    return {"tracks": tracks}


@app.post("/api/music/stop")
async def music_stop():
    """Остановить музыку."""
    life.music_playing = False
    life.current_track = None
    return {"ok": True}


# ── ПОГОДА ──

@app.get("/api/world/weather")
async def api_weather(lat: float = 55.7558, lon: float = 37.6173):
    """Подробная погода через Open-Meteo."""
    return await world.weather_detailed(lat, lon) or {"error": "unavailable"}


# ── НОВОСТИ ──

@app.get("/api/world/news")
async def api_news(category: str = None):
    """Новости — общие или по категории."""
    if category:
        return {"headlines": await world.news_by_category(category)}
    return {"headlines": await world.news_combined()}


# ── ГЛАВНЫЙ МОЗГ ──

@app.post("/api/brain")
async def brain(data: dict):
    """Единый endpoint. ESP32 → сюда."""
    memory.energy = data.get("battery_percent", memory.energy)
    speech = data.get("human_speech")
    vision_data = data.get("vision_objects", [])
    sensors = {
        "distance_front": data.get("distance_front", 999),
        "distance_back": data.get("distance_back", 999),
        "ir_left": data.get("ir_left", False),
        "ir_right": data.get("ir_right", False),
    }

    thought = await life.think(sensors, vision_data, speech)

    if speech:
        life.human_interacted()
        memory.add_conversation("human", speech)
        memory.emotions.stimulate("joy", 5, "говорят со мной")
        prompt = f"{thought['context']}\n\nЧеловек говорит: {speech}"
    else:
        prompt = f"{thought['context']}\n\nТебе никто ничего не сказал. Решай сам."

    try:
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(f"{OLLAMA_URL}/api/generate", json={
                "model": MODEL_NAME,
                "prompt": prompt,
                "system": build_system_prompt(),
                "stream": False,
                "options": {
                    "temperature": 0.85,
                    "num_predict": 600,
                    "top_p": 0.9,
                    "repeat_penalty": 1.15,
                },
            })

        if resp.status_code != 200:
            return _fallback(sensors)

        raw = resp.json().get("response", "").strip()

        # Парсим JSON
        try:
            if raw.startswith("```"):
                raw = raw.split("\n", 1)[-1].rsplit("```", 1)[0]
            start = raw.find("{")
            end = raw.rfind("}") + 1
            if start >= 0 and end > start:
                raw = raw[start:end]
            result = json.loads(raw)
        except json.JSONDecodeError:
            result = {"speech": raw[:200], "action": "none", "speed": 0}

        # ── Обработка ответа ──

        speech_out = result.get("speech", "")
        if speech_out:
            memory.add_conversation("robot", speech_out)

        inner = result.get("inner_thought", "")
        if inner:
            memory.monologue.thoughts.append({
                "thought": inner, "time": datetime.now().isoformat()
            })

        # Эмоции
        emo_changes = result.get("emotion_changes", {})
        for emo, delta in emo_changes.items():
            memory.emotions.stimulate(emo, delta, "LLM")

        # Факты
        fact = result.get("remember_fact")
        if fact:
            memory.semantic[fact] = True
            memory.add_episode(f"Узнал: {fact}", "anticipation", 6)

        # О людях
        person_fact = result.get("remember_about_person")
        if person_fact and isinstance(person_fact, dict):
            name = person_fact.get("name", "").lower()
            pfact = person_fact.get("fact", "")
            category = person_fact.get("category", "quirk")
            if name and pfact:
                rel = memory.relationships.get_or_create(name)
                if category == "like" and pfact not in rel.get("likes", []):
                    rel.setdefault("likes", []).append(pfact)
                elif category == "dislike" and pfact not in rel.get("dislikes", []):
                    rel.setdefault("dislikes", []).append(pfact)
                elif category == "music" and pfact not in rel.get("favorite_music", []):
                    rel.setdefault("favorite_music", []).append(pfact)
                elif pfact not in rel.get("quirks", []):
                    rel.setdefault("quirks", []).append(pfact)
                memory.relationships.interact(name, positive=True, memorable_event=pfact)

        # Задачи
        if result.get("new_task"):
            memory.task_queue.append(result["new_task"])
            if not memory.current_task:
                memory.current_task = memory.task_queue.pop(0)
            memory.add_episode(f"Задача: {result['new_task'].get('description', '')}", "anticipation", 7)

        if result.get("find_person"):
            target = result["find_person"]
            memory.current_task = {
                "type": "find_person", "target": target,
                "started": datetime.now().isoformat(),
            }

        # Карта квартиры
        if result.get("name_this_room"):
            memory.apartment.name_current_location(result["name_this_room"])
            memory.add_episode(f"Назвал комнату: {result['name_this_room']}", "anticipation", 7)
            memory.daily_stats["rooms_visited"] += 1

        if result.get("mark_charger"):
            memory.apartment.set_charging_station()
            memory.add_episode("Нашёл и запомнил зарядную станцию!", "joy", 9)

        # Навигация
        action = result.get("action", "none")
        speed = min(result.get("speed", 0), 200)  # макс 200 — не на износ
        duration = result.get("duration_ms", 0)

        if action in ("forward", "backward", "left", "right"):
            est_distance = (speed / 255) * (duration / 1000) * 30  # ~30 см/сек
            memory.apartment.update_position(action, int(est_distance))

        # Безопасность
        if sensors["distance_front"] < 12 and action == "forward":
            action = "stop"
            speed = 0
        if sensors["distance_back"] < 12 and action == "backward":
            action = "stop"
            speed = 0

        # Музыка
        music_query = result.get("play_music")
        music_result = None
        if music_query:
            tracks = await world.search_music(music_query)
            if tracks and not tracks[0].get("error"):
                music_result = tracks[0]
                life.music_playing = True
                life.current_track = f"{tracks[0].get('artist', '?')} - {tracks[0].get('title', '?')}"
                memory.daily_stats["songs_played"] += 1
                # URL для стрима на робот
                music_result["stream_url"] = f"/api/music/stream/{tracks[0]['id']}"

        if result.get("stop_music"):
            life.music_playing = False
            life.current_track = None

        # Погода/новости/факт
        extra = dict(thought.get("extra_data", {}))
        if result.get("want_weather"):
            extra["weather"] = await world.weather_detailed()
        if result.get("want_news"):
            extra["news"] = await world.news_combined()
        if result.get("want_news_category"):
            extra["news_category"] = await world.news_by_category(result["want_news_category"])
        if result.get("want_fact"):
            extra["fact"] = await world.random_fact()

        # Навыки
        if sensors["distance_front"] > 30 and action == "forward":
            memory.inner_life.learn_skill("navigation", 0.1)
        if speech:
            memory.inner_life.learn_skill("conversation", 0.2)
        if music_result:
            memory.inner_life.learn_skill("music_taste", 0.3)
        if result.get("name_this_room"):
            memory.inner_life.learn_skill("room_memory", 0.5)

        # Голосовые параметры
        voice_speed = result.get("voice_speed", memory.emotions.get_voice_params()["speed"])
        voice_volume = result.get("voice_volume", memory.emotions.get_voice_params()["volume"])
        emotion_expr = result.get("emotion_expression", "neutral")
        led_color = result.get("led_color", memory.emotions.get_led_suggestion())

        memory.save()

        return {
            "speech": speech_out,
            "inner_thought": inner,
            "action": action,
            "speed": speed,
            "duration_ms": duration,
            "servo_angle": result.get("servo_angle", 90),
            "led_color": led_color,
            "led_brightness": result.get("led_brightness", 80),
            "play_music": music_result,
            "tts_needed": bool(speech_out),
            "mood": memory.emotions.get_mood_description(),
            "emotion_expression": emotion_expr,
            "voice_speed": voice_speed,
            "voice_volume": voice_volume,
            "interjection": result.get("interjection"),
            "extra": extra,
            "exploration_percent": memory.apartment.get_exploration_percent(),
        }

    except Exception as e:
        return _fallback(sensors, str(e))


def _fallback(sensors: dict, error: str = ""):
    d = sensors.get("distance_front", 999)
    if d < 20:
        return {"speech": "Ой! Чуть не врезался!", "action": "backward", "speed": 150,
                "duration_ms": 500, "servo_angle": 90, "led_color": "red",
                "tts_needed": True, "mood": "испуган", "voice_speed": 1.3, "voice_volume": 0.8}
    if d < 40:
        return {"speech": "", "action": "left", "speed": 120, "duration_ms": 300,
                "servo_angle": 90, "led_color": "yellow", "tts_needed": False, "mood": "осторожен"}
    return {"speech": "", "action": "forward", "speed": 120, "duration_ms": 0,
            "servo_angle": 90, "led_color": "green", "tts_needed": False, "mood": "спокоен"}


# ── ВСПОМОГАТЕЛЬНЫЕ ENDPOINTS ──

@app.get("/api/status")
async def status():
    return {
        "name": ROBOT_NAME,
        "version": "4.0",
        "model": MODEL_NAME,
        "mood": memory.emotions.get_mood_description(),
        "emotions": memory.emotions.to_dict(),
        "voice_params": memory.emotions.get_voice_params(),
        "energy": memory.energy,
        "task": memory.current_task,
        "queue": len(memory.task_queue),
        "relationships": {n: memory.relationships.describe_relationship(n)
                          for n in memory.relationships.relationships},
        "skills": memory.inner_life.skill_points,
        "days_alive": memory.total_days_alive,
        "stats": memory.daily_stats,
        "apartment": {
            "explored": f"{memory.apartment.get_exploration_percent():.1f}%",
            "rooms": list(memory.apartment.rooms.keys()),
            "charger_found": memory.apartment.charging_station is not None,
        },
        "music_playing": life.music_playing,
        "current_track": life.current_track,
        "recent_thoughts": memory.monologue.get_recent_thoughts(5),
        "favorite_songs": memory.favorite_songs[:5],
    }


@app.post("/api/remember_person")
async def remember_person(data: dict):
    name = data.get("name", "").lower()
    memory.relationships.interact(name, positive=True, memorable_event=data.get("event"))
    memory.save()
    return {"ok": True}


@app.post("/api/task")
async def create_task(data: dict):
    task = {
        "description": data.get("description"),
        "target_person": data.get("target_person"),
        "item": data.get("item"),
        "created": datetime.now().isoformat(),
    }
    memory.task_queue.append(task)
    if not memory.current_task:
        memory.current_task = memory.task_queue.pop(0)
    memory.save()
    return {"ok": True, "task": task}


@app.get("/api/apartment/map")
async def apartment_map():
    """Карта квартиры для отладки."""
    return memory.apartment.to_dict()


@app.post("/api/apartment/name_room")
async def name_room(data: dict):
    name = data.get("name", "")
    if name:
        memory.apartment.name_current_location(name)
        memory.save()
    return {"ok": True, "rooms": list(memory.apartment.rooms.keys())}


@app.get("/api/memory/dump")
async def memory_dump():
    return {
        "episodic_count": len(memory.episodic),
        "semantic": memory.semantic,
        "relationships": memory.relationships.to_dict(),
        "conversation_count": len(memory.conversation_log),
        "inner_life": memory.inner_life.to_dict(),
        "recent_episodes": memory.episodic[-10:],
        "apartment_rooms": memory.apartment.rooms,
    }


@app.get("/api/health")
async def health():
    ollama_ok = False
    try:
        async with httpx.AsyncClient(timeout=5) as c:
            ollama_ok = (await c.get(f"{OLLAMA_URL}/api/tags")).status_code == 200
    except Exception:
        pass
    return {
        "status": "ok", "name": ROBOT_NAME, "version": "4.0",
        "model": MODEL_NAME, "ollama": ollama_ok,
        "mood": memory.emotions.get_mood_description(),
        "gpu_model": "dolphin-qwen2:7b (GPU ~4.5GB)",
        "whisper": "small (CPU int8, 4 threads)",
        "yolo": "yolov8n (GPU)",
        "tts": "piper ru_RU-ruslan-medium (CPU)",
    }


# ── STARTUP ──

@app.on_event("startup")
async def startup():
    today = datetime.now().strftime("%Y-%m-%d")
    if memory.daily_stats.get("date") != today:
        memory.total_days_alive += 1
        memory.daily_stats = {
            "date": today, "distance_cm": 0, "conversations": 0,
            "tasks_done": 0, "songs_played": 0, "new_people_met": 0,
            "jokes_told": 0, "thoughts_had": 0, "rooms_visited": 0,
            "weather_checks": 0, "news_reads": 0,
        }
        memory.add_episode(f"Новый день #{memory.total_days_alive}", "anticipation", 3)
        memory.save()

    # Предзагрузка моделей (в фоне, не блокируя запуск)
    print(f"[{ROBOT_NAME}] День #{memory.total_days_alive}. "
          f"Настроение: {memory.emotions.get_mood_description()}")
    print(f"[GPU] LLM: {MODEL_NAME} через Ollama (~4.5 GB VRAM)")
    print(f"[CPU] Whisper: small int8 (4 потока)")
    print(f"[CPU] Piper TTS: ru_RU-ruslan-medium")
    print(f"[GPU] YOLOv8n: загрузится по первому запросу")
    print(f"[MAP] Квартира: {memory.apartment.get_exploration_percent():.1f}% изучено, "
          f"{len(memory.apartment.rooms)} комнат")


if __name__ == "__main__":
    print("=" * 65)
    print(f"  🤖 {ROBOT_NAME} v4.0 — Полноценный член семьи")
    print(f"  Модель: {MODEL_NAME} (GPU ~4.5 GB / 8 GB)")
    print(f"  Whisper: CPU int8 | Piper: CPU | YOLO: GPU")
    print(f"  День жизни: #{memory.total_days_alive}")
    print(f"  Настроение: {memory.emotions.get_mood_description()}")
    print(f"  Квартира: 170 м², изучено {memory.apartment.get_exploration_percent():.1f}%")
    print(f"  http://0.0.0.0:8000/docs")
    print("=" * 65)
    uvicorn.run(app, host="0.0.0.0", port=8000)
