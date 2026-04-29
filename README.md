# Kesha — Autonomous Home Robot

Физический домашний робот на колёсах с голосовым управлением, распознаванием объектов и языковой моделью. Строился итеративно как учебный проект: начал с простой езды по комнате, дорос до полноценного агента с памятью, расписанием, эмоциями и интеграцией с умным домом.

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
git clone https://github.com/qwert2009/autonomous-robot-kesha.git
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

После запуска `robot_brain_v5.py` доступны эндпоинты:

| Эндпоинт | Метод | Описание |
|----------|-------|----------|
| `/api/brain` | POST | Главный цикл: состояние → команда роботу |
| `/api/stt` | POST | Распознавание речи (WAV → текст) |
| `/api/tts` | POST | Синтез речи (текст → WAV) |
| `/api/vision` | POST | Детекция объектов на кадре |
| `/api/status` | GET | Статус всех систем |
| `/docs` | GET | Swagger UI |

## Переменные окружения

| Переменная | По умолчанию | Описание |
|-----------|-------------|----------|
| `OLLAMA_URL` | `http://localhost:11434` | URL Ollama сервера |
| `ROS2_UBUNTU_IP` | автоопределение | IP Ubuntu машины с ROS2 |
| `ROS2_ENABLED` | `true` | Включить ROS2 Nav2/SLAM |
| `NVIDIA_API_KEY` | — | Ключ NVIDIA API (fallback LLM) |
| `FIREWORKS_API_KEY` | — | Ключ Fireworks AI (fallback LLM) |
| `YOLO_CONFIDENCE` | `0.35` | Порог уверенности YOLO |

## Версии

| Версия | Что добавилось |
|--------|---------------|
| v3.0 | Базовое движение, объезд препятствий, речь, LED эмоции |
| v4.0 | YOLOv8 зрение, NLU парсер, Obsidian база знаний |
| v5.0 | Граф памяти, психология Big Five, CuriosityEngine |
| v6.0 | ROS2/Nav2 мост, WebSocket, tool-use агент |
| v7.0 | TaskPlanner, ScheduleManager, RAG, DreamEngine, полная автономность |

## Лицензия

MIT — см. [LICENSE](LICENSE).
