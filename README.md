# Kesha — Autonomous Home Robot

<p align="center">
  <img src="https://img.shields.io/badge/ESP32--CAM-arduino-E34F26?logo=arduino">
  <img src="https://img.shields.io/badge/FastAPI-Python-009688?logo=fastapi">
  <img src="https://img.shields.io/badge/YOLOv8-vision-7B2FBE">
  <img src="https://img.shields.io/badge/Ollama-LLM-black">
  <img src="https://img.shields.io/badge/ROS2_Nav2-SLAM-22314E">
  <img src="https://img.shields.io/badge/license-MIT-green">
</p>

<p align="center">
  <a href="#english">English</a> · <a href="#russian">Русский</a>
</p>

---

<a name="english"></a>

## Overview

**Kesha** is a physical wheeled home robot with voice control, object recognition, and an LLM brain.
Built iteratively as a learning project: started with simple room navigation, evolved into a full autonomous agent with memory, scheduling, emotions, and smart home integration.

## How It Works

The robot has two parts:

**Hardware (ESP32-CAM on a 2WD chassis)** — moves, avoids obstacles, listens to microphone, sends camera frames

**Brain (Python FastAPI server on PC)** — receives data from robot over Wi-Fi HTTP, runs through LLM (Ollama), STT, TTS and YOLOv8, sends commands back

```
ESP32-CAM  <---Wi-Fi HTTP--->  PC (FastAPI server)
  camera                         Ollama (LLM)
  microphone  POST /api/brain    Faster-Whisper (STT)
  2x HC-SR04  POST /api/stt     Piper TTS
  motors      POST /api/tts     YOLOv8n (vision)
  LED ring    POST /api/vision  memory / schedule
  SG90 servo
```

## Features

- **Navigation** — obstacle avoidance via 2x HC-SR04 + IR sensors
- **Speech** — STT with Faster-Whisper (Russian), TTS with Piper
- **Vision** — object and person detection via YOLOv8n + ESP32-CAM
- **LLM Brain** — dolphin-llama3:8b locally via Ollama, fallback to NVIDIA/Fireworks API
- **Memory** — relationship graph + episodic memory in JSON
- **Emotions** — 8 base states (Plutchik), affect behavior
- **Schedule** — reminders, rituals, owner arrival/departure patterns
- **Autonomy** — independently decides what to do when no commands
- **Integrations** — weather (Open-Meteo), news (NewsAPI), Yandex.Music

## Hardware & Cost

### From AliExpress (~$21.50):
| Component | Price |
|-----------|-------|
| ESP32-CAM (AI-Thinker) | $4 |
| 2x TT motors + wheels | $3 |
| 2WD chassis | $4 |
| TP4056 (18650 charger) | $0.50 |
| MT3608 (boost to 5V) | $0.50 |
| Switch + connectors | $1 |
| USB-UART CP2102 (flashing) | $1.50 |

### From local store (~$10):
| Component | |
|-----------|--|
| MX1508 (motor driver) | |
| 2x HC-SR04 (ultrasonic) | |
| MAX9814 (microphone) | |
| SG90 (camera servo) | |
| WS2812B 8pcs (LED ring) | |
| 2x IR sensor | |
| 2x 18650 battery | |
| MAX98357A (amplifier) | |
| Speaker 3W 4Ω | |

**Total: ~$53 + shipping**

## ESP32-CAM Wiring

```
GPIO12 --> MX1508 IN1 (left motor +)
GPIO13 --> MX1508 IN2 (left motor -)
GPIO2  --> MX1508 IN3 (right motor +)
GPIO4  --> MX1508 IN4 (right motor -)

GPIO14 --> HC-SR04 TRIG (both sensors, shared)
GPIO33 --> HC-SR04 #1 ECHO (front)
GPIO16 --> HC-SR04 #2 ECHO (rear)

GPIO15 --> SG90 servo (camera pan)
GPIO3  --> WS2812B DIN (LED ring)
GPIO33 --> MAX9814 OUT (microphone, ADC)
GPIO36 --> IR sensor left
GPIO39 --> IR sensor right

Power: 2x 18650 --> TP4056 --> MT3608 (5V) --> ESP32 + MX1508
```

## PC Requirements

- Python 3.10+
- [Ollama](https://ollama.com) with model `dolphin-llama3:8b` (~5.5 GB)
- GPU recommended (RTX 3050+) for acceptable LLM speed
- RAM 16+ GB

## Setup & Run

### 1. Server (PC)

```bash
git clone https://github.com/ambartsumov/autonomous-robot-kesha.git
cd autonomous-robot-kesha

python -m venv venv
# Windows:
venv\Scripts\activate
# Linux:
source venv/bin/activate

pip install -r requirements.txt
```

Install Ollama and download the model:

```bash
# Install from https://ollama.com
ollama pull dolphin-llama3:8b
```

Start the server:

```bash
cd server
python robot_brain_v5.py
```

Server runs on `http://0.0.0.0:8000`. Check it: open `http://localhost:8000/docs`

### 2. Flash ESP32-CAM

1. Open `firmware/robot_firmware_v3.ino` in Arduino IDE
2. Install libraries: `ArduinoJson` (6.x), `ESP32Servo`, `Adafruit NeoPixel`
3. Board: **AI Thinker ESP32-CAM**
4. Partition: **Huge APP (3MB No OTA/1MB SPIFFS)**
5. Edit at file top:
   ```cpp
   const char* WIFI_SSID  = "YOUR_WIFI";
   const char* WIFI_PASS  = "YOUR_PASSWORD";
   const char* SERVER_URL = "http://192.168.1.XXX:8000"; // your PC IP
   ```
6. Connect CP2102: `TX→U0R`, `RX→U0T`, `GND→GND`
7. Press IO0 → press RST → release IO0 → upload
8. After flashing, disconnect CP2102 and press RST

### Quick Start (Linux)

```bash
chmod +x setup_kesha.sh
./setup_kesha.sh
```

Script installs all dependencies, Ollama with model, Piper TTS with Russian voice.

## Project Structure

```
autonomous-robot-kesha/
├── server/
│   ├── robot_brain_v5.py      # Main server (FastAPI + LLM + STT/TTS + Vision)
│   ├── robot_brain_v4.py      # Previous version (for reference)
│   ├── kesha_memory_v5.json   # Robot memory (auto-created)
│   └── yolov8n.pt             # YOLO model (auto-downloaded)
├── firmware/
│   ├── robot_firmware_v3.ino  # Main ESP32-CAM firmware
│   ├── robot_firmware_v4.ino  # Improved motion version
│   └── robot_firmware_v5_cam.ino  # Extended vision version
├── docs/
│   └── BUILD_GUIDE_v5.md      # Detailed build guide
├── legacy/                    # Old versions
├── requirements.txt
├── setup_kesha.sh             # Linux setup
└── setup_kesha_v4.bat         # Windows setup
```

## Server API

After starting `robot_brain_v5.py`:

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/brain` | POST | Main loop: state → robot command |
| `/api/stt` | POST | Speech recognition (WAV → text) |
| `/api/tts` | POST | Speech synthesis (text → WAV) |
| `/api/vision` | POST | Object detection on frame |
| `/api/status` | GET | Status of all systems |
| `/docs` | GET | Swagger UI |

## Version History

| Version | What's New |
|---------|-----------|
| v3.0 | Basic movement, obstacle avoidance, speech, LED emotions |
| v4.0 | YOLOv8 vision, NLU parser, Obsidian knowledge base |
| v5.0 | Memory graph, Big Five psychology, CuriosityEngine |
| v6.0 | ROS2/Nav2 bridge, WebSocket, tool-use agent |
| v7.0 | TaskPlanner, ScheduleManager, RAG, DreamEngine, full autonomy |

## License

MIT — see [LICENSE](LICENSE)

## Author

Ambartsumov Vyacheslav — [GitHub](https://github.com/ambartsumov)

---
---

<a name="russian"></a>

## Описание

**Кеша** — физический домашний робот на колёсах с голосовым управлением, распознаванием объектов и языковой моделью. Строился итеративно как учебный проект: начал с простой езды по комнате, дорос до полноценного агента с памятью, расписанием, эмоциями и интеграцией с умным домом.

## Как это работает

Робот состоит из двух частей:

**Железо (ESP32-CAM на корпусе шасси)** — двигается, объезжает препятствия, слушает микрофон, отправляет кадры с камеры

**Мозг (Python FastAPI сервер на ПК)** — получает данные от робота по Wi-Fi HTTP, прогоняет через LLM (Ollama), STT, TTS и YOLOv8, отправляет команды обратно

```
ESP32-CAM  <---Wi-Fi HTTP--->  PC (FastAPI сервер)
  камера                         Ollama (LLM)
  микрофон    POST /api/brain     Faster-Whisper (STT)
  2x HC-SR04  POST /api/stt      Piper TTS
  моторы      POST /api/tts      YOLOv8n (vision)
  LED кольцо  POST /api/vision   память / расписание
  SG90 серво
```

## Возможности

- **Движение** — объезд препятствий через 2x HC-SR04 и ИК-датчики
- **Речь** — STT на Faster-Whisper (русский), TTS на Piper
- **Зрение** — детекция объектов и людей через YOLOv8n + ESP32-CAM
- **LLM-мозг** — dolphin-llama3:8b локально через Ollama, fallback на NVIDIA/Fireworks API
- **Память** — граф отношений + эпизодическая память в JSON
- **Эмоции** — 8 базовых состояний (Plutchik), влияют на поведение
- **Расписание** — напоминания, ритуалы, паттерны прихода/ухода хозяев
- **Автономность** — самостоятельно решает что делать когда нет команд
- **Интеграции** — погода (Open-Meteo), новости (NewsAPI), Яндекс.Музыка

## Железо и стоимость

### С AliExpress (~$21.50):
| Компонент | Цена |
|-----------|------|
| ESP32-CAM (AI-Thinker) | $4 |
| 2x TT мотора + колёса | $3 |
| Шасси 2WD | $4 |
| TP4056 (зарядка 18650) | $0.50 |
| MT3608 (повышающий до 5V) | $0.50 |
| Переключатель + разъёмы | $1 |
| USB-UART CP2102 (прошивка) | $1.50 |

### Из магазина (~850 ₽):
| Компонент | Цена |
|-----------|------|
| MX1508 (драйвер моторов) | 25 ₽ |
| 2x HC-SR04 (ультразвук) | 50 ₽ |
| MAX9814 (микрофон) | 35 ₽ |
| SG90 (серво для камеры) | 40 ₽ |
| WS2812B 8шт (LED кольцо) | 45 ₽ |
| 2x ИК датчика | 40 ₽ |
| 2x 18650 батарея | 80 ₽ |
| MAX98357A (усилитель звука) | 45 ₽ |
| Динамик 3W 4Ω | 25 ₽ |

**Итого: ~$53 + доставка**

## Схема подключения ESP32-CAM

```
GPIO12 --> MX1508 IN1 (левый мотор +)
GPIO13 --> MX1508 IN2 (левый мотор -)
GPIO2  --> MX1508 IN3 (правый мотор +)
GPIO4  --> MX1508 IN4 (правый мотор -)

GPIO14 --> HC-SR04 TRIG (оба датчика, общий)
GPIO33 --> HC-SR04 #1 ECHO (передний)
GPIO16 --> HC-SR04 #2 ECHO (задний)

GPIO15 --> SG90 серво (поворот камеры)
GPIO3  --> WS2812B DIN (LED кольцо)
GPIO33 --> MAX9814 OUT (микрофон, ADC)
GPIO36 --> ИК датчик левый
GPIO39 --> ИК датчик правый

Питание: 2x 18650 --> TP4056 --> MT3608 (5V) --> ESP32 + MX1508
```

## Требования к ПК

- Python 3.10+
- [Ollama](https://ollama.com) с моделью `dolphin-llama3:8b` (~5.5 GB)
- Желательно GPU (RTX 3050+) для нормальной скорости LLM
- RAM 16+ GB

## Установка и запуск

### 1. Сервер (ПК)

```bash
git clone https://github.com/ambartsumov/autonomous-robot-kesha.git
cd autonomous-robot-kesha

python -m venv venv
# Windows:
venv\Scripts\activate
# Linux:
source venv/bin/activate

pip install -r requirements.txt
```

Установить Ollama и скачать модель:

```bash
# Установить с https://ollama.com
ollama pull dolphin-llama3:8b
```

Запустить сервер:

```bash
cd server
python robot_brain_v5.py
```

Сервер запустится на `http://0.0.0.0:8000`. Проверить: открыть `http://localhost:8000/docs`

### 2. Прошивка ESP32-CAM

1. Открыть `firmware/robot_firmware_v3.ino` в Arduino IDE
2. Установить библиотеки: `ArduinoJson` (6.x), `ESP32Servo`, `Adafruit NeoPixel`
3. Board: **AI Thinker ESP32-CAM**
4. Partition: **Huge APP (3MB No OTA/1MB SPIFFS)**
5. Изменить в начале файла:
   ```cpp
   const char* WIFI_SSID  = "ВАШ_WIFI";
   const char* WIFI_PASS  = "ВАШ_ПАРОЛЬ";
   const char* SERVER_URL = "http://192.168.1.XXX:8000"; // IP вашего ПК
   ```
6. Подключить CP2102: `TX→U0R`, `RX→U0T`, `GND→GND`
7. Нажать кнопку IO0 → нажать RST → отпустить IO0 → загрузить
8. После прошивки отключить CP2102 и нажать RST

### Быстрый старт (Linux)

```bash
chmod +x setup_kesha.sh
./setup_kesha.sh
```

Скрипт установит все зависимости, Ollama с моделью, Piper TTS с русским голосом.

## Структура проекта

```
autonomous-robot-kesha/
├── server/
│   ├── robot_brain_v5.py      # Основной сервер (FastAPI + LLM + STT/TTS + Vision)
│   ├── robot_brain_v4.py      # Предыдущая версия (для справки)
│   ├── kesha_memory_v5.json   # Память робота (создаётся автоматически)
│   └── yolov8n.pt             # Модель YOLO (скачивается автоматически)
├── firmware/
│   ├── robot_firmware_v3.ino  # Основная прошивка ESP32-CAM
│   ├── robot_firmware_v4.ino  # Версия с улучшенным движением
│   └── robot_firmware_v5_cam.ino  # Версия с расширенным vision
├── docs/
│   └── BUILD_GUIDE_v5.md      # Детальный гайд по сборке
├── legacy/                    # Старые версии
├── requirements.txt
├── setup_kesha.sh             # Установка на Linux
└── setup_kesha_v4.bat         # Установка на Windows
```

## API сервера

| Эндпоинт | Метод | Описание |
|----------|-------|----------|
| `/api/brain` | POST | Главный цикл: состояние → команда роботу |
| `/api/stt` | POST | Распознавание речи (WAV → текст) |
| `/api/tts` | POST | Синтез речи (текст → WAV) |
| `/api/vision` | POST | Детекция объектов на кадре |
| `/api/status` | GET | Статус всех систем |
| `/docs` | GET | Swagger UI |

## Версии

| Версия | Что добавилось |
|--------|---------------|
| v3.0 | Базовое движение, объезд препятствий, речь, LED эмоции |
| v4.0 | YOLOv8 зрение, NLU парсер, Obsidian база знаний |
| v5.0 | Граф памяти, психология Big Five, CuriosityEngine |
| v6.0 | ROS2/Nav2 мост, WebSocket, tool-use агент |
| v7.0 | TaskPlanner, ScheduleManager, RAG, DreamEngine, полная автономность |

## Лицензия

MIT — см. [LICENSE](LICENSE)

## Автор

Амбарцумов Вячеслав — [GitHub](https://github.com/ambartsumov)
