"""
Локальный API-сервер для ИИ-робота.
Запускать на ПК с RTX 3050.

Установка:
    pip install fastapi uvicorn faster-whisper piper-tts ultralytics python-multipart httpx pillow numpy

Запуск:
    python robot_api_server.py

Ollama должен быть запущен отдельно:
    ollama serve
    ollama pull dolphin-gemma2:9b-q4_K_M
"""

import io
import tempfile
import wave
from pathlib import Path

import httpx
import uvicorn
from fastapi import FastAPI, File, UploadFile
from fastapi.responses import JSONResponse, StreamingResponse

app = FastAPI(title="Robot AI API")

# === Глобальные модели (ленивая загрузка) ===
_whisper_model = None
_tts_voice = None
_yolo_model = None

OLLAMA_URL = "http://localhost:11434"
SYSTEM_PROMPT = """Ты — автономный робот-помощник. Отвечай коротко, по-русски.
Ты можешь видеть через камеру и слышать через микрофон.
Если тебя просят куда-то поехать или что-то сделать, отвечай JSON с действием:
{"speech": "текст ответа", "action": "forward|backward|left|right|stop", "speed": 0-255}
Если просто разговор — только {"speech": "текст ответа"}"""


def get_whisper():
    global _whisper_model
    if _whisper_model is None:
        from faster_whisper import WhisperModel
        _whisper_model = WhisperModel(
            "small", device="cuda", compute_type="float16")
    return _whisper_model


def get_tts():
    global _tts_voice
    if _tts_voice is None:
        # Piper TTS вызывается через CLI
        # Убедитесь что модель скачана в ~/piper-models/
        pass
    return True


def get_yolo():
    global _yolo_model
    if _yolo_model is None:
        from ultralytics import YOLO
        _yolo_model = YOLO("yolov8n.pt")  # ~6MB, скачается автоматически
    return _yolo_model


# === STT: Распознавание речи ===
@app.post("/api/stt")
async def speech_to_text(audio: UploadFile = File(...)):
    """Принимает WAV аудио, возвращает текст."""
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        content = await audio.read()
        tmp.write(content)
        tmp_path = tmp.name

    model = get_whisper()
    segments, info = model.transcribe(tmp_path, language="ru")
    text = " ".join(seg.text for seg in segments)

    Path(tmp_path).unlink(missing_ok=True)
    return {"text": text.strip(), "language": info.language}


# === TTS: Синтез речи ===
@app.post("/api/tts")
async def text_to_speech(data: dict):
    """Принимает {"text": "..."}, возвращает WAV аудио."""
    import subprocess

    text = data.get("text", "")
    if not text:
        return JSONResponse({"error": "empty text"}, status_code=400)

    # Piper TTS через CLI
    # Путь к модели — поменяйте на свой
    model_path = Path.home() / "piper-models" / "ru_RU-ruslan-medium.onnx"

    if not model_path.exists():
        return JSONResponse(
            {"error": f"Модель не найдена: {model_path}. Скачайте с https://github.com/rhasspy/piper/releases"},
            status_code=500,
        )

    proc = subprocess.run(
        ["piper", "--model", str(model_path), "--output_raw"],
        input=text.encode("utf-8"),
        capture_output=True,
        timeout=30,
    )

    if proc.returncode != 0:
        return JSONResponse({"error": proc.stderr.decode()}, status_code=500)

    # Конвертируем raw PCM в WAV (16kHz, 16bit, mono)
    raw_audio = proc.stdout
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(22050)
        wf.writeframes(raw_audio)
    buf.seek(0)

    return StreamingResponse(buf, media_type="audio/wav")


# === LLM: Чат с роботом ===
@app.post("/api/chat")
async def chat(data: dict):
    """Принимает {"message": "...", "context": "..."}, возвращает ответ LLM."""
    message = data.get("message", "")
    context = data.get("context", "")  # описание того что видит камера

    full_message = message
    if context:
        full_message = f"[Камера видит: {context}]\n\nПользователь: {message}"

    async with httpx.AsyncClient(timeout=60) as client:
        resp = await client.post(
            f"{OLLAMA_URL}/api/generate",
            json={
                "model": "dolphin-gemma2:9b-q4_K_M",
                "prompt": full_message,
                "system": SYSTEM_PROMPT,
                "stream": False,
                "options": {"temperature": 0.7, "num_predict": 256},
            },
        )

    if resp.status_code != 200:
        return JSONResponse({"error": "Ollama недоступен"}, status_code=502)

    result = resp.json()
    return {"response": result.get("response", "")}


# === Computer Vision: Распознавание объектов ===
@app.post("/api/vision")
async def detect_objects(image: UploadFile = File(...)):
    """Принимает JPEG изображение, возвращает обнаруженные объекты."""
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


# === Навигация: Команда движения (для логирования) ===
@app.post("/api/navigate")
async def navigate(data: dict):
    """Обработка навигационных решений. Принимает данные сенсоров, возвращает команду."""
    distance_front = data.get("distance_front", 999)  # HC-SR04 в см
    ir_left = data.get("ir_left", False)
    ir_right = data.get("ir_right", False)
    gyro = data.get("gyro", {})

    # Простая логика избегания препятствий
    if distance_front < 20:
        return {"action": "backward", "speed": 150, "duration_ms": 500}
    elif distance_front < 40:
        if not ir_left:
            return {"action": "left", "speed": 150, "duration_ms": 300}
        elif not ir_right:
            return {"action": "right", "speed": 150, "duration_ms": 300}
        else:
            return {"action": "backward", "speed": 150, "duration_ms": 500}
    else:
        return {"action": "forward", "speed": 200, "duration_ms": 0}


# === Healthcheck ===
@app.get("/api/health")
async def health():
    """Проверка что сервер работает."""
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
        "gpu": "RTX 3050",
    }


if __name__ == "__main__":
    print("=" * 50)
    print("  Robot AI API Server")
    print("  http://0.0.0.0:8000")
    print("  Docs: http://0.0.0.0:8000/docs")
    print("=" * 50)
    uvicorn.run(app, host="0.0.0.0", port=8000)
