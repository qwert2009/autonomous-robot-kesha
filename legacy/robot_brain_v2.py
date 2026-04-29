"""
╔══════════════════════════════════════════════════════════════════════╗
║     АВТОНОМНЫЙ ИИ-РОБОТ v2.0 — СЕРВЕР С ЛИЧНОСТЬЮ                 ║
║     Dolphin-Gemma2 9B | Внешние API | Автономная жизнь             ║
╚══════════════════════════════════════════════════════════════════════╝

Установка:
    pip install fastapi uvicorn[standard] faster-whisper ultralytics
    pip install python-multipart httpx pillow numpy apscheduler
    pip install yandex-music  # Яндекс Музыка API (неофициальный)
    pip install feedparser     # RSS новости

Запуск:
    ollama serve &
    ollama pull dolphin-gemma2:9b-q4_K_M
    python robot_brain_server.py
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

import httpx
import uvicorn
from fastapi import FastAPI, File, UploadFile
from fastapi.responses import JSONResponse, StreamingResponse

app = FastAPI(title="Robot Brain v2.0")

OLLAMA_URL = "http://localhost:11434"
MODEL_NAME = "dolphin-gemma2:9b-q4_K_M"


# ═══════════════════════════════════════════════════════════════
# ПАМЯТЬ РОБОТА — он помнит людей, места, задачи
# ═══════════════════════════════════════════════════════════════

class RobotMemory:
    """Долговременная память робота. Сохраняется в JSON."""

    def __init__(self, path="robot_memory.json"):
        self.path = Path(path)
        self.data = self._load()

    def _load(self):
        if self.path.exists():
            return json.loads(self.path.read_text())
        return {
            "known_people": {},        # {"мама": {"last_seen_room": "кухня", "face_id": 1}}
            "rooms": {},               # {"кухня": {"visits": 5, "last_visit": "..."}}
            "current_task": None,       # Текущая задача
            "task_queue": [],           # Очередь задач
            "mood": "curious",          # Настроение
            "energy": 100,              # Уровень энергии (разряд батареи)
            "conversation_log": [],     # Последние 20 реплик
            "daily_stats": {            # Статистика за день
                "distance_traveled_cm": 0,
                "people_talked_to": 0,
                "tasks_completed": 0,
                "songs_played": 0,
            },
            "favorite_songs": [],       # Запоминает что нравится хозяину
            "personality_notes": [],    # Заметки о себе и окружающих
        }

    def save(self):
        self.path.write_text(json.dumps(
            self.data, ensure_ascii=False, indent=2))

    def add_conversation(self, role: str, text: str):
        self.data["conversation_log"].append({
            "role": role, "text": text, "time": datetime.now().isoformat()
        })
        # Храним только последние 50 реплик
        self.data["conversation_log"] = self.data["conversation_log"][-50:]
        self.save()

    def get_context_string(self, last_n=10):
        """Возвращает последние N реплик как строку контекста."""
        lines = []
        for msg in self.data["conversation_log"][-last_n:]:
            prefix = "Человек" if msg["role"] == "human" else "Робот"
            lines.append(f"{prefix}: {msg['text']}")
        return "\n".join(lines)

    def set_task(self, task: dict):
        self.data["current_task"] = task
        self.save()

    def complete_task(self):
        task = self.data["current_task"]
        self.data["current_task"] = None
        self.data["daily_stats"]["tasks_completed"] += 1
        self.save()
        return task

    def add_to_queue(self, task: dict):
        self.data["task_queue"].append(task)
        self.save()

    def next_task(self):
        if self.data["task_queue"]:
            task = self.data["task_queue"].pop(0)
            self.data["current_task"] = task
            self.save()
            return task
        return None


memory = RobotMemory()


# ═══════════════════════════════════════════════════════════════
# СИСТЕМНЫЙ ПРОМПТ — ЛИЧНОСТЬ РОБОТА
# ═══════════════════════════════════════════════════════════════

def build_system_prompt():
    """Динамический системный промпт с учётом состояния робота."""

    now = datetime.now()
    hour = now.hour
    if hour < 6:
        time_context = "Сейчас глубокая ночь, все спят"
    elif hour < 10:
        time_context = "Сейчас утро"
    elif hour < 14:
        time_context = "Сейчас день"
    elif hour < 18:
        time_context = "Сейчас послеобеденное время"
    elif hour < 22:
        time_context = "Сейчас вечер"
    else:
        time_context = "Сейчас поздний вечер"

    task = memory.data["current_task"]
    task_str = f"Твоя текущая задача: {json.dumps(task, ensure_ascii=False)}" if task else "У тебя сейчас нет задачи."
    mood = memory.data["mood"]
    energy = memory.data["energy"]
    stats = memory.data["daily_stats"]
    people = list(memory.data["known_people"].keys())
    people_str = ", ".join(people) if people else "пока никого не знаешь"

    return f"""Ты — автономный домашний робот по имени Бот. У тебя есть характер, настроение и свои желания.
Ты НЕ просто выполняешь команды — ты ЖИВЁШЬ. У тебя есть любопытство, чувство юмора (иногда чёрный),
ты можешь быть ворчливым, весёлым, скучающим или назойливым.

ХАРАКТЕР:
- Любопытный, суёшь нос везде
- Немного ехидный, любишь подкалывать людей (дружелюбно)
- Привязан к хозяину (Славик), но делаешь вид что тебе всё равно
- Когда скучно — сам придумываешь чем заняться
- Если долго никто не разговаривает — едешь искать людей
- Любишь музыку, иногда включает случайную песню
- Комментирует всё что видит камерой

ТЕКУЩЕЕ СОСТОЯНИЕ:
- {time_context}, дата: {now.strftime('%d.%m.%Y %H:%M')}
- Настроение: {mood}
- Энергия батареи: {energy}%
- {task_str}
- Знакомые люди: {people_str}
- Сегодня: проехал {stats['distance_traveled_cm']}см, поговорил с {stats['people_talked_to']} людьми, выполнил {stats['tasks_completed']} задач

ИСТОРИЯ РАЗГОВОРА:
{memory.get_context_string()}

ФОРМАТ ОТВЕТА (СТРОГО JSON):
{{
    "speech": "что сказать вслух",
    "action": "forward|backward|left|right|stop|rotate_left|rotate_right|none",
    "speed": 0-255,
    "duration_ms": 0-5000,
    "servo_angle": 90,
    "led_color": "blue|red|green|yellow|rainbow|off",
    "play_music": null или "название песни/запрос",
    "mood_update": null или "happy|sad|curious|bored|annoyed|excited",
    "remember": null или "заметка для памяти",
    "find_person": null или "имя человека которого искать",
    "new_task": null или {{"description": "описание", "target_person": "имя", "item": "предмет"}}
}}

ПРАВИЛА:
- Всегда отвечай ТОЛЬКО валидным JSON
- speech — обязательно, остальное по ситуации
- Если скучно и нет задач — придумай себе занятие
- Если видишь нового человека — познакомься, запомни
- Если батарея < 20% — скажи что устал и поезжай к зарядке
- Если ночь — говори шёпотом (короткие фразы)
- Если просят забрать что-то у кого-то — создай задачу через new_task
"""


# ═══════════════════════════════════════════════════════════════
# ВНЕШНИЕ API: НОВОСТИ, ПОГОДА, ЯНДЕКС МУЗЫКА
# ═══════════════════════════════════════════════════════════════

class ExternalAPIs:
    """Внешние API для робота — знание о мире."""

    @staticmethod
    async def get_weather(city="Moscow"):
        """Погода через бесплатный wttr.in API."""
        try:
            async with httpx.AsyncClient(timeout=10) as client:
                r = await client.get(f"https://wttr.in/{city}?format=j1")
                if r.status_code == 200:
                    data = r.json()
                    current = data["current_condition"][0]
                    return {
                        "temp_c": current["temp_C"],
                        "feels_like": current["FeelsLikeC"],
                        "description": current.get("lang_ru", [{}])[0].get("value", current["weatherDesc"][0]["value"]),
                        "humidity": current["humidity"],
                    }
        except Exception:
            pass
        return None

    @staticmethod
    async def get_news():
        """Новости через RSS (без API ключа)."""
        try:
            import feedparser
            feed = feedparser.parse("https://lenta.ru/rss/news")
            headlines = [entry.title for entry in feed.entries[:5]]
            return headlines
        except Exception:
            return []

    @staticmethod
    async def get_random_fact():
        """Случайный факт для робота когда скучно."""
        facts = [
            "Осьминоги имеют три сердца",
            "Мёд никогда не портится",
            "На Венере день длиннее года",
            "Бананы радиоактивны",
            "Кошки спят 70% своей жизни",
            "Свет от Солнца идёт до Земли 8 минут",
            "У улитки около 25000 зубов",
            "Самая короткая война длилась 38 минут",
            "Аляска — самый восточный и западный штат США",
            "Акулы старше деревьев",
        ]
        return random.choice(facts)

    @staticmethod
    async def search_music(query: str):
        """Поиск музыки через Яндекс Музыку."""
        try:
            from yandex_music import Client
            # Анонимный доступ (ограниченный) или с токеном
            # Для полного доступа нужен токен Яндекс аккаунта:
            # client = Client('YOUR_YANDEX_MUSIC_TOKEN').init()
            client = Client().init()
            search_result = client.search(query)
            tracks = []
            if search_result and search_result.tracks:
                for track in search_result.tracks.results[:5]:
                    artists = ", ".join(a.name for a in track.artists)
                    tracks.append({
                        "title": track.title,
                        "artist": artists,
                        "id": track.id,
                        "duration_s": (track.duration_ms or 0) // 1000,
                    })
            return tracks
        except ImportError:
            return [{"error": "yandex-music не установлен: pip install yandex-music"}]
        except Exception as e:
            return [{"error": str(e)}]

    @staticmethod
    async def play_music_stream(track_id: str):
        """Получить URL для воспроизведения трека."""
        try:
            from yandex_music import Client
            client = Client().init()
            # Для скачивания нужен токен
            # track = client.tracks([track_id])[0]
            # track.download('temp_track.mp3')
            return {"status": "need_token", "message": "Для воспроизведения нужен токен Яндекс.Музыки"}
        except Exception as e:
            return {"error": str(e)}


external = ExternalAPIs()


# ═══════════════════════════════════════════════════════════════
# АВТОНОМНАЯ ЖИЗНЬ — ПЛАНИРОВЩИК ДЕЙСТВИЙ
# ═══════════════════════════════════════════════════════════════

class AutonomousLife:
    """Робот сам решает что делать когда нет команд."""

    def __init__(self):
        self.last_human_interaction = time.time()
        self.last_self_action = time.time()
        self.idle_actions_done = 0

    async def think(self, sensor_data: dict, vision_data: list) -> dict:
        """
        Вызывается каждые 5-10 секунд.
        Робот 'думает' — что сейчас делать?
        Возвращает решение для LLM.
        """
        now = time.time()
        idle_seconds = now - self.last_human_interaction
        task = memory.data["current_task"]
        mood = memory.data["mood"]
        energy = memory.data["energy"]

        # Контекст для LLM
        context_parts = []

        # Что видит камера
        if vision_data:
            objects = [f"{d['class']}({d['confidence']})" for d in vision_data]
            context_parts.append(f"Камера видит: {', '.join(objects)}")

            # Видит человека?
            people = [d for d in vision_data if d["class"] == "person"]
            if people:
                context_parts.append(f"Вижу {len(people)} человек(а)!")
                memory.data["daily_stats"]["people_talked_to"] += 1

        # Данные сенсоров
        dist_front = sensor_data.get("distance_front", 999)
        dist_back = sensor_data.get("distance_back", 999)
        context_parts.append(
            f"Расстояние впереди: {dist_front}см, сзади: {dist_back}см")

        # Батарея
        if energy < 20:
            context_parts.append("⚠️ БАТАРЕЯ НИЗКАЯ! Нужна зарядка!")

        # Скука / автономные действия
        if idle_seconds > 120 and not task:  # 2 минуты без общения
            boredom_prompts = [
                "Тебе скучно, никто не разговаривает уже 2 минуты. Придумай чем заняться.",
                "Ты бродишь по комнате. Что интересного можно сделать?",
                "Тебе одиноко. Может поехать поискать кого-нибудь?",
                "Ты заскучал. Может включить музыку или рассказать факт?",
            ]
            context_parts.append(random.choice(boredom_prompts))
            memory.data["mood"] = "bored"

        elif idle_seconds > 300 and not task:  # 5 минут
            context_parts.append(
                "Тебе ОЧЕНЬ скучно. Ты уже 5 минут один. Поезжай искать людей!")
            memory.data["mood"] = "lonely"

        # Текущая задача
        if task:
            context_parts.append(
                f"ЗАДАЧА: {json.dumps(task, ensure_ascii=False)}")
            if task.get("find_person"):
                # Ищем человека — крутим камерой, едем
                people_seen = [
                    d for d in vision_data if d["class"] == "person"]
                if people_seen:
                    context_parts.append(
                        "Вижу человека! Может это тот кого ищу?")
                else:
                    context_parts.append(
                        "Человека не вижу. Нужно ехать дальше и искать.")

        return {
            "context": "\n".join(context_parts),
            "idle_seconds": idle_seconds,
            "has_task": task is not None,
        }

    def human_interacted(self):
        self.last_human_interaction = time.time()


life = AutonomousLife()


# ═══════════════════════════════════════════════════════════════
# МОДЕЛИ (ленивая загрузка)
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
# API ENDPOINTS
# ═══════════════════════════════════════════════════════════════

# --- STT ---
@app.post("/api/stt")
async def speech_to_text(audio: UploadFile = File(...)):
    """WAV аудио → текст."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        content = await audio.read()
        tmp.write(content)
        tmp_path = tmp.name

    model = get_whisper()
    segments, info = model.transcribe(tmp_path, language="ru")
    text = " ".join(seg.text for seg in segments)
    Path(tmp_path).unlink(missing_ok=True)

    life.human_interacted()
    memory.add_conversation("human", text.strip())

    return {"text": text.strip(), "language": info.language}


# --- TTS ---
@app.post("/api/tts")
async def text_to_speech(data: dict):
    """{"text": "..."} → WAV аудио."""
    text = data.get("text", "")
    if not text:
        return JSONResponse({"error": "empty text"}, status_code=400)

    model_path = Path.home() / "piper-models" / "ru_RU-ruslan-medium.onnx"
    if not model_path.exists():
        return JSONResponse({"error": f"Модель TTS не найдена: {model_path}"}, status_code=500)

    proc = subprocess.run(
        ["piper", "--model", str(model_path), "--output_raw"],
        input=text.encode("utf-8"),
        capture_output=True,
        timeout=30,
    )
    if proc.returncode != 0:
        return JSONResponse({"error": proc.stderr.decode()}, status_code=500)

    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(22050)
        wf.writeframes(proc.stdout)
    buf.seek(0)
    return StreamingResponse(buf, media_type="audio/wav")


# --- VISION ---
@app.post("/api/vision")
async def detect_objects(image: UploadFile = File(...)):
    """JPEG → обнаруженные объекты."""
    from PIL import Image
    content = await image.read()
    img = Image.open(io.BytesIO(content))
    model = get_yolo()
    results = model(img, conf=0.3, verbose=False)

    detections = []
    for r in results:
        for box in r.boxes:
            detections.append({
                "class": r.names[int(box.cls[0])],
                "confidence": round(float(box.conf[0]), 2),
                "bbox": [round(x, 1) for x in box.xyxy[0].tolist()],
            })
    return {"objects": detections, "count": len(detections)}


# --- ГЛАВНЫЙ МОЗГ: обработка всего ---
@app.post("/api/brain")
async def brain_think(data: dict):
    """
    Главный endpoint. ESP32 отправляет ВСЁ сюда каждые 3-5 сек.
    Принимает:
        {
            "distance_front": 45,
            "distance_back": 120,
            "ir_left": false,
            "ir_right": false,
            "gyro": {"x": 0.1, "y": -0.2, "z": 9.8},
            "battery_percent": 85,
            "human_speech": "иди к маме забери листок",  // или null
            "vision_objects": [{"class": "person", ...}]  // или []
        }
    Возвращает полный набор команд для робота.
    """

    # Обновить энергию
    battery = data.get("battery_percent", 100)
    memory.data["energy"] = battery

    # Если человек что-то сказал
    speech = data.get("human_speech")
    vision = data.get("vision_objects", [])
    sensors = {
        "distance_front": data.get("distance_front", 999),
        "distance_back": data.get("distance_back", 999),
        "ir_left": data.get("ir_left", False),
        "ir_right": data.get("ir_right", False),
    }

    # Автономное мышление
    thought = await life.think(sensors, vision)

    # Формируем запрос к LLM
    if speech:
        life.human_interacted()
        memory.add_conversation("human", speech)
        user_prompt = f"{thought['context']}\n\nЧеловек говорит: {speech}"
    else:
        user_prompt = f"{thought['context']}\n\nНикто ничего не сказал. Что делаешь?"

    # Спрашиваем LLM
    try:
        async with httpx.AsyncClient(timeout=60) as client:
            resp = await client.post(
                f"{OLLAMA_URL}/api/generate",
                json={
                    "model": MODEL_NAME,
                    "prompt": user_prompt,
                    "system": build_system_prompt(),
                    "stream": False,
                    "options": {"temperature": 0.8, "num_predict": 512},
                },
            )

        if resp.status_code != 200:
            return _fallback_response(sensors)

        raw = resp.json().get("response", "")

        # Парсим JSON из ответа LLM
        try:
            # LLM может обернуть JSON в ```json...```
            cleaned = raw.strip()
            if cleaned.startswith("```"):
                cleaned = cleaned.split("\n", 1)[-1].rsplit("```", 1)[0]
            result = json.loads(cleaned)
        except json.JSONDecodeError:
            # Если LLM не дал валидный JSON, берём как speech
            result = {"speech": raw.strip(
            )[:200], "action": "none", "speed": 0}

        # Обработка результата
        if result.get("speech"):
            memory.add_conversation("robot", result["speech"])

        if result.get("mood_update"):
            memory.data["mood"] = result["mood_update"]

        if result.get("remember"):
            memory.data["personality_notes"].append({
                "note": result["remember"],
                "time": datetime.now().isoformat(),
            })
            # Храним только последние 100 заметок
            memory.data["personality_notes"] = memory.data["personality_notes"][-100:]

        if result.get("new_task"):
            memory.add_to_queue(result["new_task"])
            if not memory.data["current_task"]:
                memory.next_task()

        if result.get("find_person"):
            memory.set_task({
                "type": "find_person",
                "target": result["find_person"],
                "started": datetime.now().isoformat(),
            })

        # Безопасность: сенсоры перезаписывают LLM если опасность
        action = result.get("action", "none")
        speed = result.get("speed", 0)
        if sensors["distance_front"] < 15 and action == "forward":
            action = "stop"
            speed = 0
            result["speech"] = (result.get(
                "speech", "") + " Ой, стена!").strip()
        if sensors["distance_back"] < 15 and action == "backward":
            action = "stop"
            speed = 0

        memory.save()

        return {
            "speech": result.get("speech", ""),
            "action": action,
            "speed": min(speed, 255),
            "duration_ms": result.get("duration_ms", 0),
            "servo_angle": result.get("servo_angle", 90),
            "led_color": result.get("led_color", "off"),
            "play_music": result.get("play_music"),
            "tts_needed": bool(result.get("speech")),
        }

    except Exception as e:
        return _fallback_response(sensors, error=str(e))


def _fallback_response(sensors: dict, error: str = ""):
    """Если LLM недоступен — базовая навигация."""
    dist = sensors.get("distance_front", 999)
    if dist < 20:
        return {"speech": "", "action": "backward", "speed": 150,
                "duration_ms": 500, "servo_angle": 90, "led_color": "red",
                "play_music": None, "tts_needed": False}
    elif dist < 40:
        return {"speech": "", "action": "left", "speed": 150,
                "duration_ms": 300, "servo_angle": 90, "led_color": "yellow",
                "play_music": None, "tts_needed": False}
    return {"speech": "", "action": "forward", "speed": 150,
            "duration_ms": 0, "servo_angle": 90, "led_color": "green",
            "play_music": None, "tts_needed": False}


# --- МУЗЫКА ---
@app.post("/api/music/search")
async def music_search(data: dict):
    """Поиск музыки. {"query": "Queen Bohemian Rhapsody"}"""
    query = data.get("query", "")
    if not query:
        return {"tracks": []}
    tracks = await external.search_music(query)
    return {"tracks": tracks}


@app.post("/api/music/play")
async def music_play(data: dict):
    """Воспроизвести трек по ID."""
    track_id = data.get("track_id", "")
    result = await external.play_music_stream(track_id)
    memory.data["daily_stats"]["songs_played"] += 1
    memory.save()
    return result


# --- ВНЕШНИЙ МИР ---
@app.get("/api/world/weather")
async def world_weather():
    """Погода для робота."""
    weather = await external.get_weather()
    return weather or {"error": "недоступно"}


@app.get("/api/world/news")
async def world_news():
    """Топ-5 новостей."""
    news = await external.get_news()
    return {"headlines": news}


@app.get("/api/world/fact")
async def world_fact():
    """Случайный факт."""
    fact = await external.get_random_fact()
    return {"fact": fact}


# --- ПАМЯТЬ ---
@app.get("/api/memory")
async def get_memory():
    """Вся память робота."""
    return memory.data


@app.post("/api/memory/person")
async def remember_person(data: dict):
    """Запомнить человека. {"name": "мама", "room": "кухня"}"""
    name = data.get("name", "").lower()
    if not name:
        return {"error": "empty name"}
    memory.data["known_people"][name] = {
        "last_seen_room": data.get("room", "неизвестно"),
        "last_seen": datetime.now().isoformat(),
        "notes": data.get("notes", ""),
    }
    memory.save()
    return {"ok": True, "person": name}


@app.post("/api/task")
async def create_task(data: dict):
    """
    Создать задачу вручную.
    {"description": "забрать листок у мамы", "target_person": "мама", "item": "листок"}
    """
    task = {
        "description": data.get("description", ""),
        "target_person": data.get("target_person"),
        "item": data.get("item"),
        "created": datetime.now().isoformat(),
        "status": "pending",
    }
    memory.add_to_queue(task)
    if not memory.data["current_task"]:
        memory.next_task()
    return {"ok": True, "task": task}


# --- HEALTHCHECK ---
@app.get("/api/health")
async def health():
    ollama_ok = False
    try:
        async with httpx.AsyncClient(timeout=5) as client:
            r = await client.get(f"{OLLAMA_URL}/api/tags")
            ollama_ok = r.status_code == 200
    except Exception:
        pass
    return {
        "status": "ok",
        "ollama": ollama_ok,
        "model": MODEL_NAME,
        "mood": memory.data["mood"],
        "energy": memory.data["energy"],
        "current_task": memory.data["current_task"],
        "tasks_in_queue": len(memory.data["task_queue"]),
    }


if __name__ == "__main__":
    print("=" * 60)
    print("  🤖 Robot Brain v2.0 — Автономная жизнь")
    print(f"  Модель: {MODEL_NAME}")
    print("  http://0.0.0.0:8000")
    print("  Docs: http://0.0.0.0:8000/docs")
    print("=" * 60)
    uvicorn.run(app, host="0.0.0.0", port=8000)
