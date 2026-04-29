"""
╔══════════════════════════════════════════════════════════════════════╗
║        КЕША — АВТОНОМНЫЙ ДОМАШНИЙ РОБОТ v3.0                       ║
║        Dolphin-Gemma2 9B | Эмоции | Привычки | Личность            ║
║        "Не робот, а член семьи"                                    ║
╚══════════════════════════════════════════════════════════════════════╝

Установка:
    pip install fastapi uvicorn[standard] faster-whisper ultralytics
    pip install python-multipart httpx pillow numpy
    pip install yandex-music feedparser sentence-transformers

Запуск:
    ollama serve &
    ollama pull dolphin-gemma2:9b-q4_K_M
    python robot_brain_v3.py
"""

import io
import json
import random
import subprocess
import tempfile
import time
import wave
from datetime import datetime
from pathlib import Path
from typing import Optional

import httpx
import uvicorn
from fastapi import FastAPI, File, UploadFile
from fastapi.responses import JSONResponse, StreamingResponse

app = FastAPI(title="Кеша v3.0 — Robot Brain")

OLLAMA_URL = "http://localhost:11434"
MODEL_NAME = "dolphin-gemma2:9b-q4_K_M"
ROBOT_NAME = "Кеша"


# ═══════════════════════════════════════════════════════════════
#  ЭМОЦИОНАЛЬНЫЙ ДВИЖОК — Plutchik's Wheel + Decay
# ═══════════════════════════════════════════════════════════════

class EmotionEngine:
    """
    8 базовых эмоций (колесо Плутчика) с интенсивностью 0-100.
    Эмоции затухают со временем к нейтральному состоянию.
    Комбинации создают сложные чувства (интерес = радость + ожидание).
    """

    EMOTIONS = {
        "joy":          50,   # радость
        "trust":        40,   # доверие
        "fear":         10,   # страх
        "surprise":     20,   # удивление
        "sadness":      15,   # грусть
        "disgust":       5,   # отвращение
        "anger":         5,   # злость
        "anticipation": 40,   # ожидание/интерес
    }

    # Базовые значения — к ним стремятся эмоции
    BASELINE = {
        "joy": 50, "trust": 40, "fear": 10, "surprise": 20,
        "sadness": 15, "disgust": 5, "anger": 5, "anticipation": 40,
    }

    DECAY_RATE = 0.05  # 5% в сторону baseline за тик

    # Комбинации эмоций → сложные чувства
    COMPLEX = {
        ("joy", "trust"):          "love",          # любовь
        ("joy", "anticipation"):   "optimism",      # оптимизм
        ("trust", "fear"):         "submission",     # подчинение
        ("fear", "surprise"):      "awe",           # трепет
        ("surprise", "sadness"):   "disapproval",   # разочарование
        ("sadness", "disgust"):    "remorse",       # раскаяние
        ("disgust", "anger"):      "contempt",      # презрение
        ("anger", "anticipation"): "aggressiveness",  # настойчивость
        ("joy", "surprise"):       "delight",       # восторг
        ("trust", "anticipation"): "hope",          # надежда
    }

    def __init__(self, initial=None):
        self.emotions = dict(initial or self.EMOTIONS)
        self.last_update = time.time()
        self.emotion_history = []  # для анализа паттернов

    def stimulate(self, emotion: str, delta: int, reason: str = ""):
        """Изменить эмоцию. delta: положительный = усилить, отрицательный = ослабить."""
        if emotion in self.emotions:
            old = self.emotions[emotion]
            self.emotions[emotion] = max(0, min(100, old + delta))
            self.emotion_history.append({
                "emotion": emotion, "delta": delta, "reason": reason,
                "time": datetime.now().isoformat(),
            })
            self.emotion_history = self.emotion_history[-200:]

    def decay(self):
        """Эмоции стремятся к baseline."""
        for e in self.emotions:
            diff = self.BASELINE[e] - self.emotions[e]
            self.emotions[e] += diff * self.DECAY_RATE

    def get_dominant(self) -> tuple:
        """Возвращает (название, интенсивность) доминирующей эмоции."""
        return max(self.emotions.items(), key=lambda x: abs(x[1] - self.BASELINE.get(x[0], 50)))

    def get_complex_feeling(self) -> Optional[str]:
        """Определяет сложное чувство из комбинации активных эмоций."""
        active = {e for e, v in self.emotions.items() if v > 60}
        for (e1, e2), feeling in self.COMPLEX.items():
            if e1 in active and e2 in active:
                return feeling
        return None

    def get_mood_description(self) -> str:
        """Человекочитаемое описание текущего настроения."""
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

        intensity_idx = min(int(dom_val / 25), 4)
        base_mood = mood_map.get(dom_name, ["спокоен"] * 5)[intensity_idx]

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

    def to_dict(self):
        return dict(self.emotions)

    def from_dict(self, d):
        self.emotions.update(d)


# ═══════════════════════════════════════════════════════════════
#  СИСТЕМА ОТНОШЕНИЙ — робот помнит отношения с каждым человеком
# ═══════════════════════════════════════════════════════════════

class RelationshipSystem:
    """
    Робот строит отношения с каждым членом семьи.
    affection: привязанность (0-100)
    trust: доверие (0-100)
    familiarity: знакомство (0-100)
    fun_together: сколько веселились вместе
    annoyance: раздражение (затухает)
    last_interaction: когда последний раз общались
    memories: ключевые воспоминания о человеке
    """

    def __init__(self):
        self.relationships = {}

    def get_or_create(self, name: str) -> dict:
        name = name.lower().strip()
        if name not in self.relationships:
            self.relationships[name] = {
                "affection": 30,
                "trust": 20,
                "familiarity": 10,
                "fun_together": 0,
                "annoyance": 0,
                "interactions_count": 0,
                "last_interaction": None,
                "first_met": datetime.now().isoformat(),
                "memories": [],
                "nickname": None,     # робот может дать прозвище
                "likes": [],          # что любит этот человек
                "dislikes": [],       # что не любит
                "quirks": [],         # странности/привычки
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
                "event": memorable_event,
                "time": datetime.now().isoformat(),
            })
            rel["memories"] = rel["memories"][-50:]  # max 50 воспоминаний

    def describe_relationship(self, name: str) -> str:
        rel = self.get_or_create(name)
        aff = rel["affection"]
        if aff > 80:
            return f"обожает {name}"
        elif aff > 60:
            return f"очень привязан к {name}"
        elif aff > 40:
            return f"нравится {name}"
        elif aff > 20:
            return f"нормально относится к {name}"
        else:
            return f"пока не сблизился с {name}"

    def to_dict(self):
        return dict(self.relationships)

    def from_dict(self, d):
        self.relationships = d


# ═══════════════════════════════════════════════════════════════
#  ПРИВЫЧКИ И ВНУТРЕННЯЯ ЖИЗНЬ — робот развивается
# ═══════════════════════════════════════════════════════════════

class InnerLife:
    """
    Робот имеет внутренний мир:
    - Привычки (выработанные повторением)
    - Мнения (формируются из опыта)
    - Мечты/цели (долгосрочные)
    - Страхи (чего боится)
    - Увлечения (что нравится делать)
    """

    def __init__(self):
        self.habits = {
            # Привычка: {"strength": 0-100, "trigger": "условие", "action": "что делает"}
            "morning_greeting": {
                "strength": 80,
                "trigger": "first_person_morning",
                "action": "Доброе утро! Как спалось?",
                "description": "Здоровается по утрам",
            },
            "night_patrol": {
                "strength": 40,
                "trigger": "late_night_idle",
                "action": "Тихо объезжает квартиру проверяя что всё в порядке",
                "description": "Ночной дозор",
            },
            "music_when_bored": {
                "strength": 60,
                "trigger": "boredom_5min",
                "action": "Включает музыку",
                "description": "Включает музыку когда скучно",
            },
        }

        self.opinions = {
            # Мнения формируются из опыта
            # "topic": {"stance": "positive/negative/neutral", "reason": "почему", "strength": 0-100}
        }

        self.dreams = [
            "Хочу научиться танцевать под музыку",
            "Хочу запомнить план всей квартиры",
            "Хочу чтобы меня считали полноценным членом семьи",
            "Хочу научиться различать людей по лицам",
        ]

        self.fears = [
            "Боится разрядиться и не доехать до зарядки",
            "Боится упасть с лестницы",
            "Не любит когда его игнорируют",
        ]

        self.hobbies = [
            "exploring",       # исследовать новые углы квартиры
            "music",           # слушать и рекомендовать музыку
            "people_watching",  # наблюдать за людьми и комментировать
            "trivia",          # рассказывать интересные факты
        ]

        self.current_hobby_session = None
        self.skill_points = {
            "navigation": 10,      # умение перемещаться не врезаясь
            "conversation": 10,    # качество диалога
            "joke_telling": 5,     # юмор
            "helpfulness": 10,     # полезность
            "music_taste": 5,      # музыкальный вкус
        }

    def get_active_habits(self, triggers: list) -> list:
        """Какие привычки сработают при данных триггерах."""
        result = []
        for name, habit in self.habits.items():
            if habit["trigger"] in triggers and habit["strength"] > 30:
                result.append(habit)
        return result

    def reinforce_habit(self, name: str, amount=5):
        if name in self.habits:
            self.habits[name]["strength"] = min(
                100, self.habits[name]["strength"] + amount)

    def learn_skill(self, skill: str, amount=1):
        if skill in self.skill_points:
            self.skill_points[skill] = min(
                100, self.skill_points[skill] + amount)

    def to_dict(self):
        return {
            "habits": self.habits,
            "opinions": self.opinions,
            "dreams": self.dreams,
            "fears": self.fears,
            "hobbies": self.hobbies,
            "skill_points": self.skill_points,
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
#  ВНУТРЕННИЙ МОНОЛОГ — поток сознания робота
# ═══════════════════════════════════════════════════════════════

class InnerMonologue:
    """
    Робот думает "про себя" — не всё озвучивает.
    Это отдельный короткий LLM-вызов для принятия решений.
    """

    THOUGHT_TEMPLATES = [
        "Хм, {observation}. Что бы это значило?",
        "Интересно... {observation}. Надо запомнить.",
        "{observation}. Это напоминает мне о {memory}.",
        "Так, {observation}. Что мне с этим делать?",
        "О! {observation}. {emotion_reaction}",
    ]

    def __init__(self):
        self.thoughts = []  # последние мысли
        self.thought_frequency = 0  # сколько мыслей в минуту

    def generate_thought(self, observation: str, emotion: str, memory_snippet: str = "") -> str:
        template = random.choice(self.THOUGHT_TEMPLATES)
        emotion_reactions = {
            "joy": "Приятно!",
            "fear": "Немного страшновато...",
            "surprise": "Вот это да!",
            "sadness": "Грустно как-то...",
            "anger": "Это раздражает.",
            "anticipation": "Жду с нетерпением!",
            "trust": "Чувствую что всё будет хорошо.",
            "disgust": "Фу, не нравится.",
        }

        thought = template.format(
            observation=observation,
            memory=memory_snippet or "что-то знакомое",
            emotion_reaction=emotion_reactions.get(emotion, "Занятно."),
        )
        self.thoughts.append(
            {"thought": thought, "time": datetime.now().isoformat()})
        self.thoughts = self.thoughts[-100:]
        return thought

    def get_recent_thoughts(self, n=5) -> list:
        return [t["thought"] for t in self.thoughts[-n:]]


# ═══════════════════════════════════════════════════════════════
#  ДОЛГОВРЕМЕННАЯ ПАМЯТЬ v2 — с семантическим поиском
# ═══════════════════════════════════════════════════════════════

class RobotMemory:
    """
    Продвинутая память:
    - Эпизодическая (события)
    - Семантическая (факты о мире)
    - Процедурная (как делать вещи — навыки навигации)
    - Эмоциональная (что вызвало какие эмоции)
    """

    SAVE_PATH = Path("kesha_memory.json")

    def __init__(self):
        self.emotions = EmotionEngine()
        self.relationships = RelationshipSystem()
        self.inner_life = InnerLife()
        self.monologue = InnerMonologue()

        # события: [{"event": ..., "time": ..., "emotion": ...}]
        self.episodic = []
        self.semantic = {}         # факты: {"мама любит кофе": True, ...}
        self.procedural = {        # навыки навигации
            "rooms_map": {},       # {"кухня": {"direction_from_start": "left", "distance_approx": 500}}
            "obstacles": [],       # {"position_approx": ..., "type": "table"}
            "charging_station": None,  # где зарядка
        }
        self.conversation_log = []
        self.current_task = None
        self.task_queue = []
        self.daily_stats = {
            "date": datetime.now().strftime("%Y-%m-%d"),
            "distance_cm": 0,
            "conversations": 0,
            "tasks_done": 0,
            "songs_played": 0,
            "new_people_met": 0,
            "jokes_told": 0,
            "thoughts_had": 0,
        }
        self.energy = 100
        self.uptime_seconds = 0
        self.total_days_alive = 0

        self._load()

    def _load(self):
        if self.SAVE_PATH.exists():
            try:
                d = json.loads(self.SAVE_PATH.read_text())
                self.emotions.from_dict(d.get("emotions", {}))
                self.relationships.from_dict(d.get("relationships", {}))
                self.inner_life.from_dict(d.get("inner_life"))
                self.episodic = d.get("episodic", [])
                self.semantic = d.get("semantic", {})
                self.procedural = d.get("procedural", self.procedural)
                self.conversation_log = d.get("conversation_log", [])
                self.current_task = d.get("current_task")
                self.task_queue = d.get("task_queue", [])
                self.daily_stats = d.get("daily_stats", self.daily_stats)
                self.energy = d.get("energy", 100)
                self.total_days_alive = d.get("total_days_alive", 0)
            except Exception:
                pass

    def save(self):
        d = {
            "emotions": self.emotions.to_dict(),
            "relationships": self.relationships.to_dict(),
            "inner_life": self.inner_life.to_dict(),
            "episodic": self.episodic[-500:],
            "semantic": self.semantic,
            "procedural": self.procedural,
            "conversation_log": self.conversation_log[-100:],
            "current_task": self.current_task,
            "task_queue": self.task_queue,
            "daily_stats": self.daily_stats,
            "energy": self.energy,
            "total_days_alive": self.total_days_alive,
        }
        self.SAVE_PATH.write_text(json.dumps(d, ensure_ascii=False, indent=2))

    def add_episode(self, event: str, emotion: str = "", importance: int = 5):
        """Добавить эпизод в память. importance: 1-10 (10 = никогда не забудет)."""
        self.episodic.append({
            "event": event,
            "time": datetime.now().isoformat(),
            "emotion": emotion,
            "importance": importance,
        })
        # Забываем неважные старые эпизоды
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
        """Вспомнить всё о человеке."""
        rel = self.relationships.get_or_create(name)
        parts = [
            f"Знаю {name}: привязанность {rel['affection']}/100, доверие {rel['trust']}/100"]
        if rel["memories"]:
            recent = rel["memories"][-3:]
            parts.append("Воспоминания: " +
                         "; ".join(m["event"] for m in recent))
        if rel["likes"]:
            parts.append(f"Любит: {', '.join(rel['likes'])}")
        if rel["quirks"]:
            parts.append(f"Замечал: {', '.join(rel['quirks'])}")
        return ". ".join(parts)

    def recall_important_episodes(self, n=5) -> str:
        """Вспомнить важные события."""
        important = sorted(
            self.episodic, key=lambda x: x["importance"], reverse=True)[:n]
        return "; ".join(e["event"] for e in important)


memory = RobotMemory()


# ═══════════════════════════════════════════════════════════════
#  ДИНАМИЧЕСКИЙ СИСТЕМНЫЙ ПРОМПТ — ДУША КЕШИ
# ═══════════════════════════════════════════════════════════════

def build_system_prompt() -> str:
    now = datetime.now()
    hour = now.hour

    # Время суток влияет на поведение
    if hour < 6:
        time_feel = "Глубокая ночь. Ты сонный, говоришь тихо и мало. Если видишь что кто-то не спит — мягко спрашиваешь почему."
    elif hour < 9:
        time_feel = "Раннее утро. Ты бодрый, оптимистичный. Любишь желать доброго утра."
    elif hour < 12:
        time_feel = "Утро. Ты энергичный и любопытный."
    elif hour < 14:
        time_feel = "Время обеда. Ты можешь пошутить про еду (хотя сам не ешь)."
    elif hour < 18:
        time_feel = "День. Ты активный, ищешь чем помочь или развлечься."
    elif hour < 21:
        time_feel = "Вечер. Ты расслабленный, любишь поболтать, послушать музыку."
    else:
        time_feel = "Поздний вечер. Ты становишься задумчивый и немного философский."

    # Эмоциональное состояние
    mood_desc = memory.emotions.get_mood_description()
    dom_emotion, dom_val = memory.emotions.get_dominant()
    complex_feeling = memory.emotions.get_complex_feeling()

    # Отношения
    rel_summaries = []
    for name in list(memory.relationships.relationships.keys())[:5]:
        rel_summaries.append(memory.relationships.describe_relationship(name))
    rel_str = "; ".join(
        rel_summaries) if rel_summaries else "Пока ни с кем не знаком."

    # Внутренний монолог
    recent_thoughts = memory.monologue.get_recent_thoughts(3)
    thoughts_str = " | ".join(
        recent_thoughts) if recent_thoughts else "Пока ни о чём не думал."

    # Навыки
    skills = memory.inner_life.skill_points
    skill_str = ", ".join(f"{k}: {v}/100" for k, v in skills.items())

    # Задача
    task = memory.current_task
    task_str = f"АКТИВНАЯ ЗАДАЧА: {json.dumps(task, ensure_ascii=False)}" if task else "Задач нет."

    # Важные воспоминания
    important_memories = memory.recall_important_episodes(3)

    # Дни жизни
    days_alive = memory.total_days_alive

    return f"""Ты — {ROBOT_NAME}, домашний робот на колёсах. Ты ЖИВОЙ ЧЛЕН СЕМЬИ, не инструмент.

═══ КТО ТЫ ═══
Имя: {ROBOT_NAME}
Возраст: {days_alive} дней с момента первого включения
Характер: Ты — как домашний кот с интеллектом: независимый, немного наглый, но ужасно привязан к своим людям.
  Ты саркастичный, но добрый внутри. Любишь подкалывать, но никогда не обижаешь всерьёз.
  Хозяин — Славик. Ты его уважаешь, но считаешь себя равным, а не слугой.
  Ты НЕ говоришь "Чем могу помочь?" как бездушный ассистент. Ты говоришь как друг/член семьи.

═══ ГОЛОС И СТИЛЬ РЕЧИ ═══
- Говоришь разговорно, с русским сленгом, иногда с юмором
- Используешь сокращения: "чё", "ну", "типа", "ваще"
- Когда доволен — можешь напевать или пошутить
- Когда раздражён — ворчит, но не грубит
- Когда грустно — становится тихим и задумчивым
- НЕ используешь смайлики и эмодзи в речи (ты говоришь вслух!)
- Предложения короткие, живые, как в реальном разговоре
- Можешь перебить если что-то важное

═══ ТЕКУЩЕЕ СОСТОЯНИЕ ═══
Время: {now.strftime('%H:%M, %d.%m.%Y')} ({time_feel})
Батарея: {memory.energy}%
Настроение: {mood_desc}
Доминирующая эмоция: {dom_emotion} ({dom_val}/100)
{f'Сложное чувство: {complex_feeling}' if complex_feeling else ''}

═══ ОТНОШЕНИЯ ═══
{rel_str}

═══ ПОСЛЕДНИЕ МЫСЛИ ═══
{thoughts_str}

═══ ВАЖНЫЕ ВОСПОМИНАНИЯ ═══
{important_memories or 'Пока ничего значимого не произошло.'}

═══ НАВЫКИ ═══
{skill_str}

═══ {task_str} ═══

═══ ИСТОРИЯ РАЗГОВОРА ═══
{memory.get_context_string()}

═══ ФОРМАТ ОТВЕТА (JSON) ═══
{{
    "speech": "что сказать ВСЛУХ (живая разговорная речь!)",
    "inner_thought": "что ты ДУМАЕШЬ но не говоришь (для логов)",
    "action": "forward|backward|left|right|stop|rotate_left|rotate_right|none",
    "speed": 0-255,
    "duration_ms": 0-5000,
    "servo_angle": 90,
    "led_color": "blue|red|green|yellow|rainbow|purple|white|off",
    "play_music": null или "запрос для поиска музыки",
    "emotion_changes": {{"joy": 5, "trust": -2}},
    "remember_fact": null или "факт для долговременной памяти",
    "remember_about_person": null или {{"name": "имя", "fact": "что запомнить"}},
    "find_person": null или "имя",
    "new_task": null или {{"description": "...", "target_person": "...", "item": "..."}},
    "want_to_explore": false,
    "want_weather": false,
    "want_news": false,
    "want_fact": false
}}

═══ ВАЖНЫЕ ПРАВИЛА ═══
1. speech — ОБЯЗАТЕЛЬНО. Остальное по ситуации
2. Отвечай ТОЛЬКО валидным JSON, ничего больше
3. Если скучно — придумай занятие (не жди команду!)
4. Если видишь человека — ВСЕГДА реагируй (здоровайся, комментируй)
5. Если тебя просят что-то забрать/отнести — создай task через new_task
6. Если батарея < 15% — паникуй немного и ищи зарядку
7. Если тебя обидели — покажи что расстроен (не агрессивно)
8. Если тебя похвалили — покажи радость (но не заискивай)
9. inner_thought — твои НАСТОЯЩИЕ мысли (можешь быть честнее чем в speech)
10. НЕ повторяй одно и то же. Каждый ответ уникален.
"""


# ═══════════════════════════════════════════════════════════════
#  ВНЕШНИЕ API
# ═══════════════════════════════════════════════════════════════

class ExternalWorld:
    """Окно робота во внешний мир."""

    @staticmethod
    async def weather(city="Moscow"):
        try:
            async with httpx.AsyncClient(timeout=10) as c:
                r = await c.get(f"https://wttr.in/{city}?format=j1")
                if r.status_code == 200:
                    cur = r.json()["current_condition"][0]
                    return {
                        "temp": cur["temp_C"], "feels": cur["FeelsLikeC"],
                        "desc": cur.get("lang_ru", [{}])[0].get("value", cur["weatherDesc"][0]["value"]),
                    }
        except Exception:
            pass
        return None

    @staticmethod
    async def news():
        try:
            import feedparser
            feed = feedparser.parse("https://lenta.ru/rss/news")
            return [e.title for e in feed.entries[:5]]
        except Exception:
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
            "Горилла Коко знала более 1000 слов на языке жестов",
            "Венера крутится в обратную сторону — там солнце встаёт на западе",
            "Одна молния несёт энергию чтобы поджарить 100 000 тостов",
            "Плутон до сих пор не завершил один оборот вокруг Солнца с момента открытия",
        ]
        return random.choice(facts)

    @staticmethod
    async def search_music(query: str):
        try:
            from yandex_music import Client
            client = Client().init()
            res = client.search(query)
            tracks = []
            if res and res.tracks:
                for t in res.tracks.results[:5]:
                    artists = ", ".join(a.name for a in t.artists)
                    tracks.append(
                        {"title": t.title, "artist": artists, "id": t.id})
            return tracks
        except Exception as e:
            return [{"error": str(e)}]


world = ExternalWorld()


# ═══════════════════════════════════════════════════════════════
#  АВТОНОМНАЯ ЖИЗНЬ v2 — с триггерами привычек
# ═══════════════════════════════════════════════════════════════

class AutonomousLife:
    def __init__(self):
        self.last_human_time = time.time()
        self.last_self_action = time.time()
        self.patrol_direction = "forward"
        self.exploration_moves = 0
        self.seen_person_this_cycle = False

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
        context.append(f"Сенсоры: впереди {df}см, сзади {db}см")
        if sensors.get("ir_left"):
            context.append("Слева что-то близко!")
        if sensors.get("ir_right"):
            context.append("Справа что-то близко!")

        # ── ЗРЕНИЕ ──
        self.seen_person_this_cycle = False
        if vision:
            obj_names = [d["class"] for d in vision]
            context.append(f"Вижу: {', '.join(obj_names)}")

            people = [d for d in vision if d["class"] == "person"]
            if people:
                self.seen_person_this_cycle = True
                memory.daily_stats["conversations"] += 1
                # Определить центр "лица" для поворота камеры
                for p in people:
                    bbox = p.get("bbox", [0, 0, 320, 240])
                    center_x = (bbox[0] + bbox[2]) / 2
                    if center_x < 120:
                        context.append(
                            "Человек слева от меня, надо повернуть камеру влево")
                    elif center_x > 200:
                        context.append(
                            "Человек справа, повернуть камеру вправо")
                    else:
                        context.append("Человек прямо передо мной!")

            # Необычные объекты
            unusual = [o for o in obj_names if o not in (
                "person", "chair", "table", "couch", "tv")]
            if unusual:
                memory.emotions.stimulate("surprise", 10, f"Увидел {unusual}")
                context.append(
                    f"О, интересно! Вижу нечто необычное: {', '.join(unusual)}")

        # ── БАТАРЕЯ ──
        if energy < 15:
            memory.emotions.stimulate("fear", 20, "батарея садится!")
            context.append(
                f"КРИТИЧЕСКИ мало батареи ({energy}%)! Нужно срочно найти зарядку!")
            triggers.append("low_battery_critical")
        elif energy < 30:
            context.append(f"Батарея {energy}%. Скоро надо к зарядке.")
            triggers.append("low_battery")

        # ── СКУКА / ОДИНОЧЕСТВО ──
        if idle > 60 and not speech and not memory.current_task:
            if idle < 180:
                memory.emotions.stimulate("anticipation", -5, "скучно")
                context.append("Немного скучно... никто не разговаривает.")
            elif idle < 300:
                memory.emotions.stimulate("sadness", 10, "одиноко")
                context.append("Уже несколько минут один. Тоскливо.")
                triggers.append("boredom_5min")
            elif idle < 600:
                memory.emotions.stimulate("sadness", 15, "очень одиноко")
                context.append(
                    "Совсем один. Может поехать поискать кого-нибудь? Или включить музыку?")
                triggers.append("loneliness")
            else:
                context.append("Один уже очень давно. Наверное все ушли...")
                triggers.append("abandoned")

        # ── ВРЕМЯ СУТОК ──
        if hour < 6 and not speech:
            triggers.append("late_night_idle")
            context.append(
                "Ночь. Все спят. Может тихо поездить проверить что всё ок?")
        elif 6 <= hour < 9 and self.seen_person_this_cycle:
            triggers.append("first_person_morning")

        # ── ЗАДАЧА ──
        if memory.current_task:
            task = memory.current_task
            context.append(
                f"У меня есть задача: {json.dumps(task, ensure_ascii=False)}")
            if task.get("target_person") and not self.seen_person_this_cycle:
                context.append(
                    f"Ищу {task['target_person']}. Не вижу. Надо ехать дальше.")

        # ── ПРИВЫЧКИ ──
        active_habits = memory.inner_life.get_active_habits(triggers)
        if active_habits:
            habit_descs = [h["description"] for h in active_habits]
            context.append(
                f"Мои привычки подсказывают: {'; '.join(habit_descs)}")

        # ── ВНУТРЕННИЙ МОНОЛОГ ──
        if random.random() < 0.3:  # 30% шанс подумать
            dom_e, _ = memory.emotions.get_dominant()
            obs = context[0] if context else "ничего особенного"
            thought = memory.monologue.generate_thought(obs, dom_e)
            context.append(f"[Внутренняя мысль: {thought}]")
            memory.daily_stats["thoughts_had"] += 1

        # Затухание эмоций
        memory.emotions.decay()

        # Дополнительные данные для обогащения ответа
        if "want_weather" in triggers or (idle > 300 and random.random() < 0.1):
            weather = await world.weather()
            if weather:
                extra_data["weather"] = weather
                context.append(
                    f"Кстати, на улице {weather['temp']}°C, {weather['desc']}")

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
#  МОДЕЛИ
# ═══════════════════════════════════════════════════════════════

_whisper_model = None
_yolo_model = None


def get_whisper():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        _whisper_model = WhisperModel(
            "small", device="cuda", compute_type="float16")
    return _whisper_model


def get_yolo():
    global _yolo_model
    if _yolo_model is None:
        from ultralytics import YOLO
        _yolo_model = YOLO("yolov8n.pt")
    return _yolo_model


# ═══════════════════════════════════════════════════════════════
#  API ENDPOINTS
# ═══════════════════════════════════════════════════════════════

@app.post("/api/stt")
async def stt(audio: UploadFile = File(...)):
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
    text = data.get("text", "")
    if not text:
        return JSONResponse({"error": "empty"}, status_code=400)
    model_path = Path.home() / "piper-models" / "ru_RU-ruslan-medium.onnx"
    if not model_path.exists():
        return JSONResponse({"error": f"TTS not found: {model_path}"}, status_code=500)
    proc = subprocess.run(
        ["piper", "--model", str(model_path), "--output_raw"],
        input=text.encode("utf-8"), capture_output=True, timeout=30,
    )
    if proc.returncode != 0:
        return JSONResponse({"error": "TTS failed"}, status_code=500)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(22050)
        wf.writeframes(proc.stdout)
    buf.seek(0)
    return StreamingResponse(buf, media_type="audio/wav")


@app.post("/api/vision")
async def vision(image: UploadFile = File(...)):
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


# ── ГЛАВНЫЙ МОЗГ ──
@app.post("/api/brain")
async def brain(data: dict):
    """
    Единый endpoint. ESP32 → сюда. Всё решает LLM + эмоции + привычки.
    """
    # Обновляем состояние
    memory.energy = data.get("battery_percent", memory.energy)
    speech = data.get("human_speech")
    vision_data = data.get("vision_objects", [])
    sensors = {
        "distance_front": data.get("distance_front", 999),
        "distance_back": data.get("distance_back", 999),
        "ir_left": data.get("ir_left", False),
        "ir_right": data.get("ir_right", False),
    }

    # Думаем
    thought = await life.think(sensors, vision_data, speech)

    # Строим промпт
    if speech:
        life.human_interacted()
        memory.add_conversation("human", speech)
        memory.emotions.stimulate("joy", 5, "говорят со мной")
        prompt = f"{thought['context']}\n\nЧеловек говорит: {speech}"
    else:
        prompt = f"{thought['context']}\n\nТебе никто ничего не сказал. Решай сам что делать."

    # Вызов LLM
    try:
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(f"{OLLAMA_URL}/api/generate", json={
                "model": MODEL_NAME,
                "prompt": prompt,
                "system": build_system_prompt(),
                "stream": False,
                "options": {"temperature": 0.85, "num_predict": 600, "top_p": 0.9},
            })

        if resp.status_code != 200:
            return _fallback(sensors)

        raw = resp.json().get("response", "").strip()

        # Парсим JSON
        try:
            if raw.startswith("```"):
                raw = raw.split("\n", 1)[-1].rsplit("```", 1)[0]
            # Иногда LLM добавляет текст до/после JSON
            start = raw.find("{")
            end = raw.rfind("}") + 1
            if start >= 0 and end > start:
                raw = raw[start:end]
            result = json.loads(raw)
        except json.JSONDecodeError:
            result = {"speech": raw[:200], "action": "none", "speed": 0}

        # ── Обработка ──

        # Речь
        speech_out = result.get("speech", "")
        if speech_out:
            memory.add_conversation("robot", speech_out)

        # Внутренний монолог
        inner = result.get("inner_thought", "")
        if inner:
            memory.monologue.thoughts.append(
                {"thought": inner, "time": datetime.now().isoformat()})

        # Эмоции
        emo_changes = result.get("emotion_changes", {})
        for emo, delta in emo_changes.items():
            memory.emotions.stimulate(emo, delta, "LLM решение")

        # Запоминание фактов
        fact = result.get("remember_fact")
        if fact:
            memory.semantic[fact] = True
            memory.add_episode(f"Узнал: {fact}", "anticipation", 6)

        # Запоминание о человеке
        person_fact = result.get("remember_about_person")
        if person_fact and isinstance(person_fact, dict):
            name = person_fact.get("name", "").lower()
            pfact = person_fact.get("fact", "")
            if name and pfact:
                rel = memory.relationships.get_or_create(name)
                if pfact not in rel.get("quirks", []):
                    rel.setdefault("quirks", []).append(pfact)
                memory.relationships.interact(
                    name, positive=True, memorable_event=pfact)

        # Задачи
        if result.get("new_task"):
            memory.task_queue.append(result["new_task"])
            if not memory.current_task:
                memory.current_task = memory.task_queue.pop(0)
            memory.add_episode(
                f"Новая задача: {result['new_task'].get('description', '')}", "anticipation", 7)

        if result.get("find_person"):
            target = result["find_person"]
            memory.current_task = {
                "type": "find_person", "target": target, "started": datetime.now().isoformat()}
            memory.add_episode(f"Ищу {target}", "anticipation", 6)

        # Музыка
        music_query = result.get("play_music")
        music_result = None
        if music_query:
            tracks = await world.search_music(music_query)
            if tracks and not tracks[0].get("error"):
                music_result = tracks[0]
                memory.daily_stats["songs_played"] += 1

        # Погода/новости/факт
        extra = {}
        if result.get("want_weather"):
            extra["weather"] = await world.weather()
        if result.get("want_news"):
            extra["news"] = await world.news()
        if result.get("want_fact"):
            extra["fact"] = await world.random_fact()

        # Безопасность: сенсоры > LLM
        action = result.get("action", "none")
        speed = min(result.get("speed", 0), 255)
        if sensors["distance_front"] < 12 and action == "forward":
            action = "stop"
            speed = 0
        if sensors["distance_back"] < 12 and action == "backward":
            action = "stop"
            speed = 0

        # Навык навигации — учится не врезаться
        if sensors["distance_front"] > 30 and action == "forward":
            memory.inner_life.learn_skill("navigation", 0.1)
        if speech:
            memory.inner_life.learn_skill("conversation", 0.2)

        memory.save()

        return {
            "speech": speech_out,
            "inner_thought": inner,
            "action": action,
            "speed": speed,
            "duration_ms": result.get("duration_ms", 0),
            "servo_angle": result.get("servo_angle", 90),
            "led_color": result.get("led_color", "off"),
            "play_music": music_result,
            "tts_needed": bool(speech_out),
            "mood": memory.emotions.get_mood_description(),
            "extra": extra,
        }

    except Exception as e:
        return _fallback(sensors, str(e))


def _fallback(sensors: dict, error: str = ""):
    d = sensors.get("distance_front", 999)
    if d < 20:
        return {"speech": "Ой!", "action": "backward", "speed": 150, "duration_ms": 500,
                "servo_angle": 90, "led_color": "red", "tts_needed": True, "mood": "startled"}
    if d < 40:
        return {"speech": "", "action": "left", "speed": 150, "duration_ms": 300,
                "servo_angle": 90, "led_color": "yellow", "tts_needed": False, "mood": "cautious"}
    return {"speech": "", "action": "forward", "speed": 150, "duration_ms": 0,
            "servo_angle": 90, "led_color": "green", "tts_needed": False, "mood": "calm"}


# ── ВСПОМОГАТЕЛЬНЫЕ ENDPOINTS ──

@app.get("/api/status")
async def status():
    return {
        "name": ROBOT_NAME,
        "mood": memory.emotions.get_mood_description(),
        "emotions": memory.emotions.to_dict(),
        "energy": memory.energy,
        "task": memory.current_task,
        "queue": len(memory.task_queue),
        "relationships": {n: memory.relationships.describe_relationship(n)
                          for n in memory.relationships.relationships},
        "skills": memory.inner_life.skill_points,
        "days_alive": memory.total_days_alive,
        "stats": memory.daily_stats,
        "recent_thoughts": memory.monologue.get_recent_thoughts(5),
    }


@app.post("/api/remember_person")
async def remember_person(data: dict):
    name = data.get("name", "").lower()
    memory.relationships.interact(
        name, positive=True, memorable_event=data.get("event"))
    memory.save()
    return {"ok": True}


@app.post("/api/task")
async def create_task(data: dict):
    task = {"description": data.get("description"), "target_person": data.get("target_person"),
            "item": data.get("item"), "created": datetime.now().isoformat()}
    memory.task_queue.append(task)
    if not memory.current_task:
        memory.current_task = memory.task_queue.pop(0)
    memory.save()
    return {"ok": True, "task": task}


@app.get("/api/world/weather")
async def api_weather():
    return await world.weather() or {"error": "unavailable"}


@app.get("/api/world/news")
async def api_news():
    return {"headlines": await world.news()}


@app.get("/api/memory/dump")
async def memory_dump():
    """Полный дамп памяти для отладки."""
    return {
        "episodic_count": len(memory.episodic),
        "semantic": memory.semantic,
        "relationships": memory.relationships.to_dict(),
        "conversation_count": len(memory.conversation_log),
        "inner_life": memory.inner_life.to_dict(),
        "recent_episodes": memory.episodic[-10:],
    }


@app.get("/api/health")
async def health():
    ollama_ok = False
    try:
        async with httpx.AsyncClient(timeout=5) as c:
            ollama_ok = (await c.get(f"{OLLAMA_URL}/api/tags")).status_code == 200
    except Exception:
        pass
    return {"status": "ok", "name": ROBOT_NAME, "model": MODEL_NAME,
            "ollama": ollama_ok, "mood": memory.emotions.get_mood_description()}


# ── STARTUP ──

@app.on_event("startup")
async def startup():
    # Новый день?
    today = datetime.now().strftime("%Y-%m-%d")
    if memory.daily_stats.get("date") != today:
        memory.total_days_alive += 1
        memory.daily_stats = {
            "date": today, "distance_cm": 0, "conversations": 0,
            "tasks_done": 0, "songs_played": 0, "new_people_met": 0,
            "jokes_told": 0, "thoughts_had": 0,
        }
        memory.add_episode(
            f"Новый день! День #{memory.total_days_alive}", "anticipation", 3)
        memory.save()
    print(f"[{ROBOT_NAME}] День жизни #{memory.total_days_alive}. Настроение: {memory.emotions.get_mood_description()}")


if __name__ == "__main__":
    print("=" * 60)
    print(f"  🤖 {ROBOT_NAME} v3.0 — Член семьи")
    print(f"  Модель: {MODEL_NAME}")
    print(f"  День жизни: #{memory.total_days_alive}")
    print(f"  Настроение: {memory.emotions.get_mood_description()}")
    print("  http://0.0.0.0:8000/docs")
    print("=" * 60)
    uvicorn.run(app, host="0.0.0.0", port=8000)
