@echo off
chcp 65001 >nul
title Установка КЕША v4.1

echo ╔═══════════════════════════════════════════════════════════════╗
echo ║  КЕША v4.1 — Полная установка (всё на D:\Kesha)             ║
echo ║  Графовая память │ Big Five │ Маслоу │ Самомодификация       ║
echo ║  i7-11700K + RTX 3050 8GB — Оптимальное распределение       ║
echo ╚═══════════════════════════════════════════════════════════════╝
echo.

:: ═══════════════════════════════════════════════════════════════
:: ПУТИ — ВСЁ НА D:
:: ═══════════════════════════════════════════════════════════════
set KESHA_DIR=D:\Kesha
set PIPER_DIR=%KESHA_DIR%\piper
set PIPER_BIN=%PIPER_DIR%\bin\piper
set PIPER_MODELS=%PIPER_DIR%\models
set FFMPEG_DIR=%KESHA_DIR%\ffmpeg
set OLLAMA_DIR=%KESHA_DIR%\ollama

:: ═══════════════════════════════════════════════════════════════
:: ПРОВЕРКИ
:: ═══════════════════════════════════════════════════════════════
echo [1/7] Проверка Python...
python --version 2>nul
if %ERRORLEVEL% neq 0 (
    echo ОШИБКА: Python не найден! Установи Python 3.10+ с python.org
    echo При установке обязательно ✓ Add Python to PATH
    pause
    exit /b 1
)

echo [2/7] Проверка NVIDIA GPU...
nvidia-smi >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo ПРЕДУПРЕЖДЕНИЕ: nvidia-smi не найден. Убедись что установлены драйверы NVIDIA.
)

:: Создать структуру
if not exist "%KESHA_DIR%" mkdir "%KESHA_DIR%"
if not exist "%PIPER_DIR%\models" mkdir "%PIPER_DIR%\models"
if not exist "%PIPER_DIR%\bin" mkdir "%PIPER_DIR%\bin"
if not exist "%FFMPEG_DIR%" mkdir "%FFMPEG_DIR%"
if not exist "%OLLAMA_DIR%\models" mkdir "%OLLAMA_DIR%\models"

:: ═══════════════════════════════════════════════════════════════
:: OLLAMA — Модели на D:
:: ═══════════════════════════════════════════════════════════════
echo.
echo [3/7] Настройка Ollama...
setx OLLAMA_MODELS "%OLLAMA_DIR%\models"
set OLLAMA_MODELS=%OLLAMA_DIR%\models

where ollama >nul 2>&1
if %ERRORLEVEL% neq 0 (
    echo Ollama не найдена. Скачай и установи: https://ollama.com/download/OllamaSetup.exe
    echo После установки перезапусти этот скрипт!
    pause
    exit /b 0
) else (
    echo Ollama установлена.
)

echo Скачиваем модель dolphin-qwen2:7b (~4.5 GB на D:)...
ollama pull dolphin-qwen2:7b
echo Модель готова!

:: ═══════════════════════════════════════════════════════════════
:: PYTHON ЗАВИСИМОСТИ
:: ═══════════════════════════════════════════════════════════════
echo.
echo [4/7] Установка Python библиотек...
pip install --upgrade pip
pip install fastapi uvicorn[standard] httpx pillow numpy scipy
pip install faster-whisper ultralytics
pip install yandex-music feedparser pydub python-multipart
echo Библиотеки установлены!

:: ═══════════════════════════════════════════════════════════════
:: PIPER TTS
:: ═══════════════════════════════════════════════════════════════
echo.
echo [5/7] Установка Piper TTS...

if not exist "%PIPER_BIN%\piper.exe" (
    echo Скачиваем Piper TTS...
    curl.exe -L -o "%PIPER_DIR%\piper_windows_amd64.zip" "https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_windows_amd64.zip" --progress-bar
    echo Распаковываем...
    powershell -Command "Expand-Archive -Path '%PIPER_DIR%\piper_windows_amd64.zip' -DestinationPath '%PIPER_DIR%\bin' -Force"
    echo Piper TTS установлен!
) else (
    echo Piper TTS уже установлен.
)

if not exist "%PIPER_MODELS%\ru_RU-ruslan-medium.onnx" (
    echo Скачиваем русский голос...
    curl.exe -L -o "%PIPER_MODELS%\ru_RU-ruslan-medium.onnx" "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx" --progress-bar
    curl.exe -L -o "%PIPER_MODELS%\ru_RU-ruslan-medium.onnx.json" "https://huggingface.co/rhasspy/piper-voices/resolve/v1.0.0/ru/ru_RU/ruslan/medium/ru_RU-ruslan-medium.onnx.json" --progress-bar
    echo Голос скачан!
) else (
    echo Русский голос уже скачан.
)

:: ═══════════════════════════════════════════════════════════════
:: FFMPEG
:: ═══════════════════════════════════════════════════════════════
echo.
echo [6/7] Установка FFmpeg...
if not exist "%FFMPEG_DIR%\bin\ffmpeg.exe" (
    echo Скачиваем FFmpeg...
    curl.exe -L -o "%FFMPEG_DIR%\ffmpeg.zip" "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip" --progress-bar
    echo Распаковываем...
    powershell -Command "Expand-Archive -Path '%FFMPEG_DIR%\ffmpeg.zip' -DestinationPath '%FFMPEG_DIR%' -Force"
    :: Переместить из подпапки в bin
    if exist "%FFMPEG_DIR%\ffmpeg-master-latest-win64-gpl\bin" (
        if not exist "%FFMPEG_DIR%\bin" mkdir "%FFMPEG_DIR%\bin"
        copy /Y "%FFMPEG_DIR%\ffmpeg-master-latest-win64-gpl\bin\ffmpeg.exe" "%FFMPEG_DIR%\bin\"
        copy /Y "%FFMPEG_DIR%\ffmpeg-master-latest-win64-gpl\bin\ffprobe.exe" "%FFMPEG_DIR%\bin\"
    )
    echo FFmpeg установлен в %FFMPEG_DIR%\bin
) else (
    echo FFmpeg уже установлен.
)

:: ═══════════════════════════════════════════════════════════════
:: СТРУКТУРА
:: ═══════════════════════════════════════════════════════════════
echo.
echo [7/7] Рабочие папки...
set SERVER_DIR=%~dp0server
if not exist "%SERVER_DIR%\memory" mkdir "%SERVER_DIR%\memory"

:: ═══════════════════════════════════════════════════════════════
:: ГОТОВО
:: ═══════════════════════════════════════════════════════════════
echo.
echo ╔═══════════════════════════════════════════════════════════════╗
echo ║  УСТАНОВКА ЗАВЕРШЕНА!  (всё на D:\Kesha)                    ║
echo ║                                                               ║
echo ║  D:\Kesha\piper\     — Piper TTS + русский голос             ║
echo ║  D:\Kesha\ffmpeg\    — FFmpeg                                 ║
echo ║  D:\Kesha\ollama\    — Модели Ollama (dolphin-qwen2:7b)      ║
echo ║                                                               ║
echo ║  GPU (~5.5/8 GB VRAM):                                       ║
echo ║    dolphin-qwen2:7b (LLM) ~4.5 GB + YOLOv8n ~1 GB          ║
echo ║  CPU (4-6/16 потоков):                                       ║
echo ║    Whisper small + Piper TTS + FastAPI                       ║
echo ║                                                               ║
echo ║  Запуск: start_kesha.bat                                      ║
echo ╚═══════════════════════════════════════════════════════════════╝
echo.
pause
