/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║  КЕША — ПРОШИВКА ESP32-CAM v3.0                              ║
  ║  Полная интеграция с robot_brain_v3.py                        ║
  ║  Настроение • Музыка • Задачи • Энергосбережение              ║
  ╚═══════════════════════════════════════════════════════════════╝

  Подключение:
    MX1508:       IN1=GPIO12, IN2=GPIO13, IN3=GPIO2, IN4=GPIO4
    HC-SR04 (F):  TRIG=GPIO14, ECHO=GPIO33
    HC-SR04 (B):  TRIG=GPIO14, ECHO=GPIO16 (общий TRIG)
    Серво SG90:   GPIO15
    WS2812B x8:   GPIO3 (RX0)
    MAX9814 MIC:  GPIO33 (ADC, мультиплекс с ECHO_F)
    ИК левый:     GPIO36 (VP)
    ИК правый:    GPIO39 (VN)
    MicroSD:      встроенный слот (1-bit SDMMC)
    MAX98357A:    BCLK=GPIO14*, WS=GPIO15*, DIN=GPIO2*
                  (* — мультиплекс с моторами, звук только при stop)

  Библиотеки (Arduino IDE):
    - ArduinoJson 6.x
    - ESP32Servo
    - Adafruit NeoPixel

  Размер Flash: используй partition scheme "Huge APP (3MB No OTA)"
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include "esp_camera.h"
#include "SD_MMC.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_sleep.h"

// ═══════════════════════════════════════════════════════════════
//  КОНФИГУРАЦИЯ — ИЗМЕНИ ПОД СЕБЯ
// ═══════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASS     = "YOUR_PASSWORD";
const char* SERVER_URL    = "http://192.168.1.100:8000";

// Таймауты (мс)
#define BRAIN_INTERVAL       3000   // Автономный вызов мозга
#define VISION_INTERVAL      5000   // Отправка кадра
#define VOICE_CHECK_INTERVAL 500    // Проверка микрофона
#define SENSOR_INTERVAL      100    // Чтение сенсоров (10 Hz)
#define RECONNECT_INTERVAL   10000  // Переподключение Wi-Fi
#define STATUS_CHECK         30000  // Пинг /api/health

// Пороги
#define VOICE_THRESHOLD      300    // Порог детекции голоса (ADC амплитуда)
#define EMERGENCY_STOP_CM    10     // Экстренное торможение
#define CAUTION_CM           25     // Осторожно

// ═══ ПИНЫ ═══
#define MOTOR_L1   12
#define MOTOR_L2   13
#define MOTOR_R1   2
#define MOTOR_R2   4

#define TRIG_PIN   14
#define ECHO_F_PIN 33
#define ECHO_B_PIN 16

#define SERVO_PIN  15

#define LED_PIN    3
#define LED_COUNT  8

#define IR_LEFT    36
#define IR_RIGHT   39

#define MIC_PIN    33  // MAX9814 аналоговый (мультиплекс с ECHO_F)

// ═══ КАМЕРА ESP32-CAM (AI-Thinker) ═══
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ═══════════════════════════════════════════════════════════════
//  ОБЪЕКТЫ И СОСТОЯНИЕ
// ═══════════════════════════════════════════════════════════════
Servo cameraServo;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

struct RobotState {
    // Сенсоры
    float distanceFront  = 999;
    float distanceBack   = 999;
    bool  irLeft         = false;
    bool  irRight        = false;
    int   batteryPercent = 100;

    // Серво
    int servoAngle = 90;

    // Зрение
    String lastVisionJson = "[]";

    // Речь
    String lastSpeech = "";

    // Настроение (от сервера)
    String currentMood = "calm";
    String currentLedColor = "off";

    // Таймеры
    unsigned long lastBrainCall    = 0;
    unsigned long lastVisionCall   = 0;
    unsigned long lastSoundCheck   = 0;
    unsigned long lastSensorRead   = 0;
    unsigned long lastReconnect    = 0;
    unsigned long lastStatusCheck  = 0;

    // Состояние подключения
    bool wifiConnected   = false;
    bool serverAlive     = false;
    int  failedBrainCalls = 0;

    // Автономный режим (без сервера)
    bool offlineMode     = false;

    // Энергия (оценка, сервер тоже считает)
    float localEnergy    = 100.0;

    // Музыка
    bool playingMusic    = false;
    String musicTitle    = "";

    // Задача
    String currentTask   = "";
    String targetPerson  = "";
} state;


// ═══════════════════════════════════════════════════════════════
//  КАМЕРА
// ═══════════════════════════════════════════════════════════════
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0       = Y2_GPIO_NUM;
    config.pin_d1       = Y3_GPIO_NUM;
    config.pin_d2       = Y4_GPIO_NUM;
    config.pin_d3       = Y5_GPIO_NUM;
    config.pin_d4       = Y6_GPIO_NUM;
    config.pin_d5       = Y7_GPIO_NUM;
    config.pin_d6       = Y8_GPIO_NUM;
    config.pin_d7       = Y9_GPIO_NUM;
    config.pin_xclk     = XCLK_GPIO_NUM;
    config.pin_pclk     = PCLK_GPIO_NUM;
    config.pin_vsync    = VSYNC_GPIO_NUM;
    config.pin_href     = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn     = PWDN_GPIO_NUM;
    config.pin_reset    = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size   = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 12;
    config.fb_count     = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Init failed: 0x%x\n", err);
    } else {
        Serial.println("[CAM] OK");
    }
}


// ═══════════════════════════════════════════════════════════════
//  МОТОРЫ
// ═══════════════════════════════════════════════════════════════
void setMotors(String action, int speed) {
    if (action == "forward") {
        analogWrite(MOTOR_L1, speed); analogWrite(MOTOR_L2, 0);
        analogWrite(MOTOR_R1, speed); analogWrite(MOTOR_R2, 0);
    } else if (action == "backward") {
        analogWrite(MOTOR_L1, 0); analogWrite(MOTOR_L2, speed);
        analogWrite(MOTOR_R1, 0); analogWrite(MOTOR_R2, speed);
    } else if (action == "left" || action == "rotate_left") {
        analogWrite(MOTOR_L1, 0); analogWrite(MOTOR_L2, speed);
        analogWrite(MOTOR_R1, speed); analogWrite(MOTOR_R2, 0);
    } else if (action == "right" || action == "rotate_right") {
        analogWrite(MOTOR_L1, speed); analogWrite(MOTOR_L2, 0);
        analogWrite(MOTOR_R1, 0); analogWrite(MOTOR_R2, speed);
    } else {  // stop, none
        analogWrite(MOTOR_L1, 0); analogWrite(MOTOR_L2, 0);
        analogWrite(MOTOR_R1, 0); analogWrite(MOTOR_R2, 0);
    }
}


// ═══════════════════════════════════════════════════════════════
//  СЕНСОРЫ
// ═══════════════════════════════════════════════════════════════
float measureDistance(int echoPin) {
    // Мультиплекс: GPIO33 используется и для MIC и для ECHO_F
    // Перед замером — переключить пин в INPUT
    if (echoPin == ECHO_F_PIN) {
        pinMode(ECHO_F_PIN, INPUT);
    }

    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(echoPin, HIGH, 30000);

    if (duration == 0) return 999;
    return duration * 0.034 / 2.0;
}

void readSensors() {
    state.distanceFront = measureDistance(ECHO_F_PIN);
    delay(30);
    state.distanceBack = measureDistance(ECHO_B_PIN);
    state.irLeft  = digitalRead(IR_LEFT)  == LOW;  // LOW = препятствие
    state.irRight = digitalRead(IR_RIGHT) == LOW;
    // Батарея — замерять через делитель напряжения (если подключен)
    // state.batteryPercent = constrain(map(analogRead(BAT_PIN), 2400, 3200, 0, 100), 0, 100);
}


// ═══════════════════════════════════════════════════════════════
//  LED — НАСТРОЕНИЕ ЧЕРЕЗ ЦВЕТ
// ═══════════════════════════════════════════════════════════════

// Настроение → цвет кольца (если сервер не задал цвет)
uint32_t moodToColor(String mood) {
    if (mood == "happy" || mood == "joyful" || mood == "радостный")
        return leds.Color(255, 200, 0);       // Тёплый жёлтый
    if (mood == "excited" || mood == "вдохновлённый")
        return leds.Color(255, 100, 0);       // Оранжевый
    if (mood == "sad" || mood == "грустный")
        return leds.Color(0, 0, 100);         // Тёмно-синий
    if (mood == "angry" || mood == "злой")
        return leds.Color(200, 0, 0);         // Красный
    if (mood == "scared" || mood == "напуганный")
        return leds.Color(100, 0, 150);       // Фиолетовый
    if (mood == "curious" || mood == "любопытный")
        return leds.Color(0, 200, 200);       // Циан
    if (mood == "loving" || mood == "нежный")
        return leds.Color(255, 50, 100);      // Розовый
    if (mood == "bored" || mood == "скучающий")
        return leds.Color(30, 30, 30);        // Тусклый
    if (mood == "sleepy" || mood == "сонный")
        return leds.Color(10, 5, 20);         // Почти выключено
    if (mood == "calm" || mood == "спокойный")
        return leds.Color(0, 100, 50);        // Зелёный
    // Default
    return leds.Color(50, 50, 50);            // Нейтральный
}

void setLedColor(String color) {
    if (color == state.currentLedColor) return;
    state.currentLedColor = color;

    if (color == "red") {
        leds.fill(leds.Color(255, 0, 0));
    } else if (color == "green") {
        leds.fill(leds.Color(0, 255, 0));
    } else if (color == "blue") {
        leds.fill(leds.Color(0, 0, 255));
    } else if (color == "yellow") {
        leds.fill(leds.Color(255, 200, 0));
    } else if (color == "pink") {
        leds.fill(leds.Color(255, 50, 100));
    } else if (color == "purple") {
        leds.fill(leds.Color(100, 0, 150));
    } else if (color == "cyan") {
        leds.fill(leds.Color(0, 200, 200));
    } else if (color == "rainbow") {
        for (int i = 0; i < LED_COUNT; i++) {
            int hue = (i * 65536 / LED_COUNT + millis() * 10) % 65536;
            leds.setPixelColor(i, leds.gamma32(leds.ColorHSV(hue)));
        }
    } else if (color == "mood") {
        // Специальный режим — цвет по настроению
        leds.fill(moodToColor(state.currentMood));
    } else if (color == "breathing") {
        // Дыхание — пульсирующая яркость
        uint8_t brightness = (sin(millis() / 500.0) + 1.0) * 80;
        uint32_t col = moodToColor(state.currentMood);
        uint8_t r = ((col >> 16) & 0xFF) * brightness / 160;
        uint8_t g = ((col >> 8)  & 0xFF) * brightness / 160;
        uint8_t b = (col         & 0xFF) * brightness / 160;
        leds.fill(leds.Color(r, g, b));
    } else {  // off
        leds.clear();
    }
    leds.show();
}

// Анимация "пульс" — мигание при речи
void ledPulse(uint32_t color, int count) {
    for (int i = 0; i < count; i++) {
        leds.fill(color);
        leds.show();
        delay(100);
        leds.clear();
        leds.show();
        delay(100);
    }
}


// ═══════════════════════════════════════════════════════════════
//  ГОЛОС — ДЕТЕКЦИЯ И ЗАПИСЬ
// ═══════════════════════════════════════════════════════════════
bool detectVoice() {
    // GPIO33 мультиплекс: сначала переключить на ADC
    pinMode(MIC_PIN, INPUT);
    delay(2);  // Стабилизация ADC

    int samples = 50;
    long sum = 0;
    for (int i = 0; i < samples; i++) {
        int val = analogRead(MIC_PIN);
        sum += abs(val - 2048);
        delayMicroseconds(200);
    }
    int avgAmplitude = sum / samples;
    return avgAmplitude > VOICE_THRESHOLD;
}

// Адаптивный порог — шум в комнате меняется
int adaptiveThreshold = VOICE_THRESHOLD;
void calibrateMic() {
    pinMode(MIC_PIN, INPUT);
    delay(5);
    long sum = 0;
    for (int i = 0; i < 200; i++) {
        sum += abs(analogRead(MIC_PIN) - 2048);
        delayMicroseconds(200);
    }
    int noiseFloor = sum / 200;
    adaptiveThreshold = noiseFloor + 150;  // Порог = шум + запас
    Serial.printf("[MIC] Noise floor: %d, threshold: %d\n", noiseFloor, adaptiveThreshold);
}

String recordAudioToSD(int durationMs) {
    pinMode(MIC_PIN, INPUT);
    delay(2);

    int sampleRate = 8000;
    int totalSamples = sampleRate * durationMs / 1000;
    String filename = "/audio_" + String(millis()) + ".wav";

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) {
        Serial.println("[REC] SD open failed!");
        return "";
    }

    // WAV header (16-bit mono 8kHz PCM)
    uint32_t dataSize = totalSamples * 2;
    uint32_t fileSize = 36 + dataSize;
    uint8_t header[44] = {
        'R','I','F','F',
        (uint8_t)(fileSize), (uint8_t)(fileSize>>8),
        (uint8_t)(fileSize>>16), (uint8_t)(fileSize>>24),
        'W','A','V','E',
        'f','m','t',' ',
        16,0,0,0,         // chunk size
        1,0,              // PCM
        1,0,              // mono
        0x40,0x1F,0,0,   // 8000 Hz
        0x80,0x3E,0,0,   // byte rate = 16000
        2,0,              // block align
        16,0,             // bits per sample
        'd','a','t','a',
        (uint8_t)(dataSize), (uint8_t)(dataSize>>8),
        (uint8_t)(dataSize>>16), (uint8_t)(dataSize>>24),
    };
    file.write(header, 44);

    // Запись
    for (int i = 0; i < totalSamples; i++) {
        int16_t sample = (analogRead(MIC_PIN) - 2048) * 16;  // Усиление
        file.write((uint8_t*)&sample, 2);
        delayMicroseconds(115);  // ~8kHz (минус время ADC ~10мкс)
    }
    file.close();
    Serial.printf("[REC] %dms → %s (%d bytes)\n", durationMs, filename.c_str(), 44 + dataSize);
    return filename;
}

// ═══════════════════════════════════════════════════════════════
//  СЕТЬ: STT, TTS, VISION, BRAIN
// ═══════════════════════════════════════════════════════════════

String sendAudioForSTT(String filename) {
    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) return "";

    size_t fileSize = file.size();
    uint8_t* buf = (uint8_t*)ps_malloc(fileSize);  // PSRAM если есть
    if (!buf) buf = (uint8_t*)malloc(fileSize);
    if (!buf) { file.close(); return ""; }
    file.read(buf, fileSize);
    file.close();

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/stt");
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(10000);

    int code = http.POST(buf, fileSize);
    free(buf);

    String text = "";
    if (code == 200) {
        StaticJsonDocument<1024> doc;
        deserializeJson(doc, http.getString());
        text = doc["text"].as<String>();
    } else {
        Serial.printf("[STT] Error: %d\n", code);
    }
    http.end();

    // Удалить временный файл
    SD_MMC.remove(filename);
    return text;
}

void updateVision() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("[VIS] Frame grab failed");
        return;
    }

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/vision");
    http.addHeader("Content-Type", "image/jpeg");
    http.setTimeout(8000);

    int code = http.POST(fb->buf, fb->len);
    if (code == 200) {
        DynamicJsonDocument doc(4096);
        deserializeJson(doc, http.getString());
        JsonArray objects = doc["objects"];
        state.lastVisionJson = "";
        serializeJson(objects, state.lastVisionJson);
    } else {
        Serial.printf("[VIS] Error: %d\n", code);
    }
    http.end();
    esp_camera_fb_return(fb);
}

void speakText(String text) {
    if (text.length() == 0) return;
    Serial.printf("[TTS] Говорю: %s\n", text.c_str());

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/tts");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    StaticJsonDocument<512> doc;
    doc["text"] = text;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code == 200) {
        // Получаем WAV поток → воспроизводим через MAX98357A
        // (требует I2S конфигурации — см. setupI2SAudio)
        WiFiClient* stream = http.getStreamPtr();
        // TODO: Реальное воспроизведение через I2S DAC
        // playWavFromStream(stream);
        Serial.println("[TTS] Audio received, playing...");
    } else {
        Serial.printf("[TTS] Error: %d\n", code);
    }
    http.end();
}


// ═══════════════════════════════════════════════════════════════
//  ГЛАВНЫЙ ВЫЗОВ: /api/brain
// ═══════════════════════════════════════════════════════════════
void callBrain(String humanSpeech) {
    if (!state.wifiConnected) {
        offlineBehavior();
        return;
    }

    setLedColor("yellow");  // Думаю...

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/brain");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);  // 15 сек — LLM может думать долго

    // Собираем весь контекст робота
    DynamicJsonDocument doc(4096);
    doc["distance_front"]   = state.distanceFront;
    doc["distance_back"]    = state.distanceBack;
    doc["ir_left"]          = state.irLeft;
    doc["ir_right"]         = state.irRight;
    doc["battery_percent"]  = state.batteryPercent;

    if (humanSpeech.length() > 0) {
        doc["human_speech"] = humanSpeech;
    }

    // Vision objects
    DynamicJsonDocument visionDoc(2048);
    deserializeJson(visionDoc, state.lastVisionJson);
    doc["vision_objects"] = visionDoc;

    String body;
    serializeJson(doc, body);
    int code = http.POST(body);

    if (code == 200) {
        state.failedBrainCalls = 0;
        state.serverAlive = true;

        DynamicJsonDocument respDoc(4096);
        deserializeJson(respDoc, http.getString());

        // ── Парсим ответ v3 ──

        // Действия
        String action   = respDoc["action"]      | "none";
        int    speed     = respDoc["speed"]        | 0;
        int    duration  = respDoc["duration_ms"]  | 0;
        int    servoAng  = respDoc["servo_angle"]  | 90;
        String ledColor  = respDoc["led_color"]    | "mood";
        bool   ttsNeeded = respDoc["tts_needed"]   | false;
        String speech    = respDoc["speech"]       | "";

        // Новое в v3: настроение и мысли
        String mood      = respDoc["mood"]          | "calm";
        String thought   = respDoc["inner_thought"] | "";

        // Музыка
        JsonObject music = respDoc["play_music"];
        if (!music.isNull()) {
            state.playingMusic = true;
            state.musicTitle = music["title"] | "Unknown";
            Serial.printf("[MUSIC] 🎵 %s — %s\n",
                (music["artist"] | "?"), state.musicTitle.c_str());
        }

        // Обновляем настроение
        state.currentMood = mood;

        // Задача (для отображения на Serial)
        if (!respDoc["extra"].isNull()) {
            JsonObject extra = respDoc["extra"];
            if (!extra["weather"].isNull()) {
                Serial.printf("[WORLD] Погода: %s\n",
                    (extra["weather"]["description"] | "?"));
            }
        }

        // ── Выполняем команды ──

        // 1. LED (если сервер не указал — используем настроение)
        if (ledColor == "off" || ledColor == "mood") {
            setLedColor("mood");
        } else {
            setLedColor(ledColor);
        }

        // 2. Серво камеры
        if (servoAng != state.servoAngle) {
            cameraServo.write(constrain(servoAng, 0, 180));
            state.servoAngle = servoAng;
        }

        // 3. Речь
        if (ttsNeeded && speech.length() > 0) {
            setLedColor("blue");  // Говорю
            speakText(speech);
            setLedColor("mood");  // Обратно к настроению
        }

        // 4. Движение (с проверкой безопасности)
        if (state.distanceFront < EMERGENCY_STOP_CM && action == "forward") {
            action = "stop";
            speed = 0;
            setLedColor("red");
        }
        if (state.distanceBack < EMERGENCY_STOP_CM && action == "backward") {
            action = "stop";
            speed = 0;
        }

        setMotors(action, min(speed, 255));
        if (duration > 0) {
            delay(min(duration, 5000));  // Макс 5 сек непрерывного движения
            setMotors("stop", 0);
        }

        // ── Логирование ──
        Serial.printf("[BRAIN] mood=%s action=%s spd=%d\n",
            mood.c_str(), action.c_str(), speed);
        if (speech.length() > 0) {
            Serial.printf("[SAY] %s\n", speech.c_str());
        }
        if (thought.length() > 0) {
            Serial.printf("[THINK] 💭 %s\n", thought.c_str());
        }

    } else {
        state.failedBrainCalls++;
        Serial.printf("[BRAIN] Error %d (fail #%d)\n", code, state.failedBrainCalls);

        // После 5 неудач — переходим в оффлайн
        if (state.failedBrainCalls >= 5) {
            state.serverAlive = false;
            Serial.println("[BRAIN] Server lost. Switching to offline mode.");
        }

        // Локальный fallback
        offlineBehavior();
    }
    http.end();
}


// ═══════════════════════════════════════════════════════════════
//  АВТОНОМНОЕ ПОВЕДЕНИЕ БЕЗ СЕРВЕРА
// ═══════════════════════════════════════════════════════════════
void offlineBehavior() {
    // Простое избегание препятствий + случайный поиск
    if (state.distanceFront < EMERGENCY_STOP_CM) {
        setMotors("stop", 0);
        setLedColor("red");
        delay(200);
        setMotors("backward", 150);
        delay(500);
        // Случайный поворот
        if (random(2) == 0) {
            setMotors("left", 150);
        } else {
            setMotors("right", 150);
        }
        delay(random(200, 600));
        setMotors("stop", 0);
    } else if (state.distanceFront < CAUTION_CM) {
        setLedColor("yellow");
        if (state.irLeft && !state.irRight) {
            setMotors("right", 120);
        } else if (!state.irLeft && state.irRight) {
            setMotors("left", 120);
        } else {
            setMotors("left", 120);
        }
        delay(300);
    } else {
        setLedColor("green");
        setMotors("forward", 120);
    }
}


// ═══════════════════════════════════════════════════════════════
//  WI-FI — ПОДКЛЮЧЕНИЕ И ПЕРЕПОДКЛЮЧЕНИЕ
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");

    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        state.wifiConnected = true;
        state.offlineMode = false;
        Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
        setLedColor("green");
    } else {
        state.wifiConnected = false;
        state.offlineMode = true;
        Serial.println("\n[WiFi] FAILED → Offline mode");
        setLedColor("purple");
    }
}

void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        state.wifiConnected = false;
        if (millis() - state.lastReconnect > RECONNECT_INTERVAL) {
            state.lastReconnect = millis();
            Serial.println("[WiFi] Reconnecting...");
            WiFi.disconnect();
            delay(100);
            WiFi.begin(WIFI_SSID, WIFI_PASS);

            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 10) {
                delay(500);
                attempts++;
            }
            state.wifiConnected = (WiFi.status() == WL_CONNECTED);
            if (state.wifiConnected) {
                Serial.println("[WiFi] Reconnected!");
                state.failedBrainCalls = 0;
                state.serverAlive = true;
                state.offlineMode = false;
            }
        }
    } else {
        state.wifiConnected = true;
    }
}

// Пинг сервера
void checkServer() {
    if (!state.wifiConnected) return;

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/health");
    http.setTimeout(5000);
    int code = http.GET();
    state.serverAlive = (code == 200);
    http.end();

    if (state.serverAlive) {
        state.failedBrainCalls = 0;
        if (state.offlineMode) {
            Serial.println("[SERVER] Back online!");
            state.offlineMode = false;
        }
    } else {
        Serial.println("[SERVER] Health check failed");
    }
}


// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  КЕША v3.0 — Firmware ESP32-CAM   ║");
    Serial.println("╚═══════════════════════════════════╝\n");

    // Моторы
    pinMode(MOTOR_L1, OUTPUT);
    pinMode(MOTOR_L2, OUTPUT);
    pinMode(MOTOR_R1, OUTPUT);
    pinMode(MOTOR_R2, OUTPUT);
    setMotors("stop", 0);

    // HC-SR04
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_F_PIN, INPUT);
    pinMode(ECHO_B_PIN, INPUT);

    // ИК
    pinMode(IR_LEFT, INPUT);
    pinMode(IR_RIGHT, INPUT);

    // Серво
    cameraServo.attach(SERVO_PIN);
    cameraServo.write(90);

    // LED — стартовая анимация
    leds.begin();
    leds.setBrightness(80);
    setLedColor("rainbow");
    delay(1000);

    // MicroSD
    if (!SD_MMC.begin("/sdcard", true)) {  // 1-bit mode (меньше пинов)
        Serial.println("[SD] Mount failed!");
    } else {
        Serial.printf("[SD] OK. Total: %lluMB\n", SD_MMC.totalBytes() / (1024*1024));
    }

    // Калибровка микрофона
    calibrateMic();

    // Камера
    setupCamera();

    // Wi-Fi
    connectWiFi();

    // Проверка сервера
    if (state.wifiConnected) {
        checkServer();
    }

    setLedColor("mood");
    Serial.println("\n[KESHA] Ready! Autonomous life starting...\n");
}


// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── 1. Проверка Wi-Fi ──
    checkWiFi();

    // ── 2. Пинг сервера (каждые 30 сек) ──
    if (now - state.lastStatusCheck > STATUS_CHECK) {
        checkServer();
        state.lastStatusCheck = now;
    }

    // ── 3. Сенсоры (10 Hz) ──
    if (now - state.lastSensorRead > SENSOR_INTERVAL) {
        readSensors();
        state.lastSensorRead = now;

        // ЭКСТРЕННОЕ ТОРМОЖЕНИЕ — локально, мгновенно
        if (state.distanceFront < EMERGENCY_STOP_CM) {
            setMotors("stop", 0);
            setLedColor("red");
        }
    }

    // ── 4. Зрение (каждые 5 сек) ──
    if (state.wifiConnected && state.serverAlive &&
        now - state.lastVisionCall > VISION_INTERVAL) {
        updateVision();
        state.lastVisionCall = now;
    }

    // ── 5. Голос (каждые 500мс) ──
    if (now - state.lastSoundCheck > VOICE_CHECK_INTERVAL) {
        state.lastSoundCheck = now;

        if (detectVoice()) {
            Serial.println("[MIC] Voice detected! Recording 5s...");
            setLedColor("blue");    // Слушаю
            setMotors("stop", 0);   // Стоп при разговоре

            String audioFile = recordAudioToSD(5000);
            if (audioFile.length() > 0) {
                String spokenText = sendAudioForSTT(audioFile);
                Serial.println("[STT] Heard: " + spokenText);

                if (spokenText.length() > 0) {
                    state.lastSpeech = spokenText;
                    callBrain(spokenText);
                    state.lastBrainCall = millis();
                }
            }
            setLedColor("mood");
        }
    }

    // ── 6. Мозг: автономная жизнь (каждые 3 сек) ──
    if (now - state.lastBrainCall > BRAIN_INTERVAL) {
        callBrain("");  // Без речи — "что делать?"
        state.lastBrainCall = now;
    }

    // ── 7. LED breathing в idle ──
    if (state.currentLedColor == "mood") {
        // Обновить пульсацию
        uint8_t brightness = (sin(millis() / 1000.0) + 1.0) * 60 + 10;
        leds.setBrightness(brightness);
        leds.fill(moodToColor(state.currentMood));
        leds.show();
    }

    delay(50);  // 20 Hz main loop
}
