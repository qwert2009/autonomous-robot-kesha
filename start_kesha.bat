@echo off
chcp 65001 >nul
title КЕША v5.1 — Запуск

echo ╔═══════════════════════════════════════════════════════════════╗
echo ║  КЕША v5.1 — Человеческий Разум                              ║
echo ║  4WD Mecanum │ Dual ESP32 │ FreeRTOS │ Автозарядка           ║
echo ║  dolphin-llama3:8b │ Fireworks AI │ PID │ Психика 2.0        ║
echo ╚═══════════════════════════════════════════════════════════════╝
echo.

:: ═══ Пути — всё на D:\Kesha ═══
set PIPER_BINARY=D:\Kesha\piper\bin\piper\piper.exe
set PIPER_VOICE_MODEL=D:\Kesha\piper\models\ru_RU-ruslan-medium.onnx
set OLLAMA_MODELS=D:\Kesha\ollama\models
set FIREWORKS_API_KEY=fw_Ra7NFFhW5fTLScfgfMchDx

:: FFmpeg в PATH (если не в системном PATH)
set PATH=D:\Kesha\ffmpeg\bin;%PATH%

:: ═══ 1. Ollama ═══
echo [1/2] Запускаю Ollama (LLM на GPU)...
tasklist /fi "imagename eq ollama.exe" | find "ollama.exe" >nul
if %ERRORLEVEL% neq 0 (
    start "" ollama serve
    echo   Ожидаю запуск Ollama...
    timeout /t 5 /nobreak >nul
) else (
    echo   Ollama уже запущена.
)

:: Проверка модели
echo   Проверяю модель dolphin-llama3:8b...
ollama list | find "dolphin-llama3:8b" >nul
if %ERRORLEVEL% neq 0 (
    echo   Модель не найдена! Скачиваю...
    ollama pull dolphin-llama3:8b
)

:: ═══ 2. Сервер Кеши ═══
echo.
echo [2/2] Запускаю мозг Кеши v5.1 (robot_brain_v5.py)...
echo.
echo   GPU: dolphin-llama3:8b (LLM, без цензуры) + YOLOv8n (зрение)
echo   CPU: Faster-Whisper (STT) + Piper (TTS) + FastAPI
echo   Облако: Fireworks AI (бесплатно, на случай падения Ollama)
echo   Привод: ESP32-CAM (мозг) + ESP32-WROOM (4WD mecanum, автодок)
echo   Психика: эпизодич.память, внутр.голос, темперамент, мотивация, мировоззрение
echo.
echo   Сервер: http://0.0.0.0:8000
echo   Документация API: http://localhost:8000/docs
echo.
echo ─────────────────────────────────────────────────────────
echo.

cd /d "%~dp0server"
python robot_brain_v5.py

:: Если упал — не закрывать окно
echo.
echo [!] Сервер остановлен. Нажмите клавишу для перезапуска...
pause
goto :eof
