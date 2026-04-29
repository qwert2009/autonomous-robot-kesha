# Kesha — Autonomous Home Robot

изический домашний робот на колёсах с голосовым управлением, распознаванием объектов и языковой моделью. Строился итеративно как учебный проект: начал с простой езды по комнате, дорос до полноценного агента с памятью, расписанием, эмоциями и интеграцией с умным домом.

## ак это работает

обот состоит из двух частей:

**елезо (ESP32-CAM на корпусе шасси)**
— двигается, объезжает препятствия, слушает микрофон, отправляет кадры с камеры

**озг (Python FastAPI сервер на )**
— получает данные от робота по Wi-Fi HTTP, прогоняет через LLM (Ollama), STT, TTS и YOLOv8, отправляет команды обратно

```
ESP32-CAM  <---Wi-Fi HTTP--->  PC (FastAPI сервер)
  камера                         Ollama (LLM)
  микрофон    POST /api/brain     Faster-Whisper (STT)
  2x HC-SR04  POST /api/stt      Piper TTS
  моторы      POST /api/tts      YOLOv8n (vision)
  LED кольцо  POST /api/vision   память / расписание
  SG90 серво
```

## озможности

- **вижение** — объезд препятствий через 2x HC-SR04 и -датчики
- **ечь** — STT на Faster-Whisper (русский), TTS на Piper
- **рение** — детекция объектов и людей через YOLOv8n + ESP32-CAM
- **LLM-мозг** — dolphin-llama3:8b локально через Ollama, fallback на NVIDIA/Fireworks API
- **амять** — граф отношений + эпизодическая память в JSON
- **моции** — 8 базовых состояний (Plutchik), влияют на поведение
- **асписание** — напоминания, ритуалы, паттерны прихода/ухода хозяев
- **втономность** — самостоятельно решает что делать когда нет команд
- **нтеграции** — погода (Open-Meteo), новости (NewsAPI), Яндекс.узыка

## елезо и стоимость

### С AliExpress (~$21.50):
| омпонент | ена |
|-----------|------|
| ESP32-CAM (AI-Thinker) | $4 |
| 2x TT мотора + колёса | $3 |
| Шасси 2WD | $4 |
| TP4056 (зарядка 18650) | $0.50 |
| MT3608 (повышающий до 5V) | $0.50 |
| ереключатель + разъёмы | $1 |
| USB-UART CP2102 (прошивка) | $1.50 |

### з магазина (~850 ):
| омпонент | ена |
|-----------|------|
| MX1508 (драйвер моторов) | 25  |
| 2x HC-SR04 (ультразвук) | 50  |
| MAX9814 (микрофон) | 35  |
| SG90 (серво для камеры) | 40  |
| WS2812B 8шт (LED кольцо) | 45  |
| 2x  датчика | 40  |
| 2x 18650 батарея | 80  |
| MAX98357A (усилитель звука) | 45  |
| инамик 3W 4Ω | 25  |

**того: ~$53 + доставка**

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
GPIO36 -->  датчик левый
GPIO39 -->  датчик правый

итание: 2x 18650 --> TP4056 --> MT3608 (5V) --> ESP32 + MX1508
```

## Требования к 

- Python 3.10+
- [Ollama](https://ollama.com) с моделью `dolphin-llama3:8b` (~5.5 GB)
- елательно GPU (RTX 3050+) для нормальной скорости LLM
- RAM 16+ GB

## становка и запуск

### 1. Сервер ()

```bash
git clone https://github.com/qwert2009/autonomous-robot-kesha.git
cd autonomous-robot-kesha

python -m venv venv
# Windows:
venv\Scripts\activate
# Linux:
source venv/bin/activate

pip install -r requirements.txt
```

становить Ollama и скачать модель:

```bash
# становить с https://ollama.com
ollama pull dolphin-llama3:8b
```

апустить сервер:

```bash
cd server
python robot_brain_v5.py
```

Сервер запустится на `http://0.0.0.0:8000`. роверить: открыть `http://localhost:8000/docs`

### 2. рошивка ESP32-CAM

1. ткрыть `firmware/robot_firmware_v3.ino` в Arduino IDE
2. становить библиотеки: `ArduinoJson` (6.x), `ESP32Servo`, `Adafruit NeoPixel`
3. Board: **AI Thinker ESP32-CAM**
4. Partition: **Huge APP (3MB No OTA/1MB SPIFFS)**
5. зменить в начале файла:
   ```cpp
   const char* WIFI_SSID  = "Ш_WIFI";
   const char* WIFI_PASS  = "Ш_Ь";
   const char* SERVER_URL = "http://192.168.1.XXX:8000"; // IP вашего 
   ```
6. одключить CP2102: `TX→U0R`, `RX→U0T`, `GND→GND`
7. ажать кнопку IO0 → нажать RST → отпустить IO0 → загрузить
8. осле прошивки отключить CP2102 и нажать RST

### ыстрый старт (Linux)

```bash
chmod +x setup_kesha.sh
./setup_kesha.sh
```

Скрипт установит все зависимости, Ollama с моделью, Piper TTS с русским голосом.

## Структура проекта

```
autonomous-robot-kesha/
├── server/
│   ├── robot_brain_v5.py      # сновной сервер (FastAPI + LLM + STT/TTS + Vision)
│   ├── robot_brain_v4.py      # редыдущая версия (для справки)
│   ├── kesha_memory_v5.json   # амять робота (создаётся автоматически)
│   └── yolov8n.pt             # одель YOLO (скачивается автоматически)
├── firmware/
│   ├── robot_firmware_v3.ino  # сновная прошивка ESP32-CAM
│   ├── robot_firmware_v4.ino  # ерсия с улучшенным движением
│   └── robot_firmware_v5_cam.ino  # ерсия с расширенным vision
├── docs/
│   └── BUILD_GUIDE_v5.md      # етальный гайд по сборке
├── legacy/                    # Старые версии
├── requirements.txt
├── setup_kesha.sh             # становка на Linux
└── setup_kesha_v4.bat         # становка на Windows
```

## API сервера

осле запуска `robot_brain_v5.py` доступны эндпоинты:

| ндпоинт | етод | писание |
|----------|-------|----------|
| `/api/brain` | POST | лавный цикл: состояние → команда роботу |
| `/api/stt` | POST | аспознавание речи (WAV → текст) |
| `/api/tts` | POST | Синтез речи (текст → WAV) |
| `/api/vision` | POST | етекция объектов на кадре |
| `/api/status` | GET | Статус всех систем |
| `/docs` | GET | Swagger UI |

## еременные окружения

| еременная | о умолчанию | писание |
|-----------|-------------|----------|
| `OLLAMA_URL` | `http://localhost:11434` | URL Ollama сервера |
| `ROS2_UBUNTU_IP` | автоопределение | IP Ubuntu машины с ROS2 |
| `ROS2_ENABLED` | `true` | ключить ROS2 Nav2/SLAM |
| `NVIDIA_API_KEY` | — | люч NVIDIA API (fallback LLM) |
| `FIREWORKS_API_KEY` | — | люч Fireworks AI (fallback LLM) |
| `YOLO_CONFIDENCE` | `0.35` | орог уверенности YOLO |

## ерсии

| ерсия | то добавилось |
|--------|---------------|
| v3.0 | азовое движение, объезд препятствий, речь, LED эмоции |
| v4.0 | YOLOv8 зрение, NLU парсер, Obsidian база знаний |
| v5.0 | раф памяти, психология Big Five, CuriosityEngine |
| v6.0 | ROS2/Nav2 мост, WebSocket, tool-use агент |
| v7.0 | TaskPlanner, ScheduleManager, RAG, DreamEngine, полная автономность |

## ицензия

MIT — см. [LICENSE](LICENSE).