/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║  КЕША — ПРОШИВКА ESP32-CAM v4.0                              ║
  ║  Интеграция с robot_brain_v4.py                               ║
  ║  I2S Аудио • Музыка на колонке • Экспрессия голоса            ║
  ║  Навигация 170 м² • Управление скоростью • LED эмоции         ║
  ╚═══════════════════════════════════════════════════════════════╝

  Подключение:
    MX1508:       IN1=GPIO12, IN2=GPIO13, IN3=GPIO2, IN4=GPIO4
    HC-SR04 (F):  TRIG=GPIO14, ECHO=GPIO33
    HC-SR04 (B):  TRIG=GPIO14, ECHO=GPIO16 (общий TRIG)
    Серво SG90:   GPIO15
    WS2812B x8:   GPIO3 (RX0)
    INMP441 MIC:  GPIO33 (ADC, мультиплекс с ECHO_F)
    ИК левый:     GPIO36 (VP)
    ИК правый:    GPIO39 (VN)
    MicroSD:      встроенный слот (1-bit SDMMC)
    MAX98357A:    BCLK=GPIO14*, WS=GPIO15*, DIN=GPIO2*
                  (* мультиплекс: звук ТОЛЬКО при остановке моторов)

  Библиотеки (Arduino IDE):
    - ArduinoJson 6.x
    - ESP32Servo
    - Adafruit NeoPixel

  Размер Flash: "Huge APP (3MB No OTA)"
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
#include "driver/i2s.h"

// ═══════════════════════════════════════════════════════════════
//  КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASS     = "YOUR_PASSWORD";
const char* SERVER_URL    = "http://192.168.1.100:8000";

// Таймауты (мс)
#define BRAIN_INTERVAL       3000
#define VISION_INTERVAL      5000
#define VOICE_CHECK_INTERVAL 500
#define SENSOR_INTERVAL      100
#define RECONNECT_INTERVAL   10000
#define STATUS_CHECK         30000

// Пороги
#define VOICE_THRESHOLD      300
#define EMERGENCY_STOP_CM    10
#define CAUTION_CM           25

// Макс скорость (не на износ — 200 из 255)
#define MAX_SPEED            200

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

#define MIC_PIN    33

// I2S для MAX98357A (мультиплекс с моторами)
#define I2S_BCLK   14
#define I2S_WS     15
#define I2S_DOUT   2
#define I2S_PORT   I2S_NUM_0

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
//  СОСТОЯНИЕ
// ═══════════════════════════════════════════════════════════════
Servo cameraServo;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

struct RobotState {
    float distanceFront  = 999;
    float distanceBack   = 999;
    bool  irLeft         = false;
    bool  irRight        = false;
    int   batteryPercent = 100;
    int   servoAngle     = 90;

    String lastVisionJson = "[]";
    String lastSpeech     = "";
    String currentMood    = "calm";
    String currentLedColor = "off";
    String emotionExpr     = "neutral";
    int    ledBrightness   = 80;

    unsigned long lastBrainCall    = 0;
    unsigned long lastVisionCall   = 0;
    unsigned long lastSoundCheck   = 0;
    unsigned long lastSensorRead   = 0;
    unsigned long lastReconnect    = 0;
    unsigned long lastStatusCheck  = 0;

    bool wifiConnected    = false;
    bool serverAlive      = false;
    int  failedBrainCalls = 0;
    bool offlineMode      = false;
    float localEnergy     = 100.0;

    bool   playingMusic   = false;
    String musicTitle     = "";
    String musicStreamUrl = "";

    String currentTask    = "";
    String targetPerson   = "";

    // I2S состояние
    bool i2sInitialized   = false;
    bool motorsActive     = false;  // моторы и I2S мультиплекс
} state;


// ═══════════════════════════════════════════════════════════════
//  I2S АУДИО — воспроизведение на MAX98357A
// ═══════════════════════════════════════════════════════════════
void setupI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 22050,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };

    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
    state.i2sInitialized = true;
    Serial.println("[I2S] Audio output ready (MAX98357A)");
}

void deinitI2S() {
    if (state.i2sInitialized) {
        i2s_driver_uninstall(I2S_PORT);
        state.i2sInitialized = false;
    }
}

// Переключение: моторы ↔ аудио (мультиплекс пинов)
void switchToAudio() {
    if (state.motorsActive) {
        // Остановить моторы
        analogWrite(MOTOR_L1, 0);
        analogWrite(MOTOR_L2, 0);
        analogWrite(MOTOR_R1, 0);
        analogWrite(MOTOR_R2, 0);
        state.motorsActive = false;
        delay(10);
    }
    if (!state.i2sInitialized) {
        setupI2S();
    }
}

void switchToMotors() {
    if (state.i2sInitialized) {
        deinitI2S();
    }
    // Восстановить пины моторов
    pinMode(MOTOR_L1, OUTPUT);
    pinMode(MOTOR_L2, OUTPUT);
    pinMode(MOTOR_R1, OUTPUT);
    pinMode(MOTOR_R2, OUTPUT);
    state.motorsActive = true;
}

// Воспроизвести WAV из HTTP потока на колонке
void playAudioFromServer(String url) {
    switchToAudio();

    HTTPClient http;
    http.begin(String(SERVER_URL) + url);
    http.setTimeout(15000);

    int code = http.GET();
    if (code == 200) {
        WiFiClient* stream = http.getStreamPtr();
        int totalSize = http.getSize();

        // Пропустить WAV заголовок (44 байта)
        uint8_t header[44];
        stream->readBytes(header, 44);

        // Читаем и воспроизводим чанками
        uint8_t buf[1024];
        size_t bytesWritten;
        int bytesRead = 0;

        Serial.printf("[AUDIO] Playing %d bytes...\n", totalSize);

        while (http.connected() && (totalSize < 0 || bytesRead < totalSize - 44)) {
            int available = stream->available();
            if (available > 0) {
                int toRead = min(available, (int)sizeof(buf));
                int len = stream->readBytes(buf, toRead);
                if (len > 0) {
                    i2s_write(I2S_PORT, buf, len, &bytesWritten, portMAX_DELAY);
                    bytesRead += len;
                }
            } else {
                delay(1);
            }
        }

        // Дать буферу проиграть
        delay(100);
        i2s_zero_dma_buffer(I2S_PORT);
        Serial.println("[AUDIO] Done");
    } else {
        Serial.printf("[AUDIO] HTTP error: %d\n", code);
    }
    http.end();
}

// Воспроизвести TTS с управлением громкостью/скоростью
void speakText(String text, float voiceSpeed, float voiceVolume) {
    if (text.length() == 0) return;
    Serial.printf("[TTS] \"%s\" (spd=%.2f, vol=%.2f)\n",
                  text.c_str(), voiceSpeed, voiceVolume);

    switchToAudio();

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/tts");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    // Отправляем параметры голоса
    DynamicJsonDocument doc(512);
    doc["text"] = text;
    doc["voice_speed"] = voiceSpeed;
    doc["voice_volume"] = voiceVolume;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code == 200) {
        WiFiClient* stream = http.getStreamPtr();
        int totalSize = http.getSize();

        // Пропустить WAV заголовок
        uint8_t header[44];
        stream->readBytes(header, 44);

        uint8_t buf[1024];
        size_t bytesWritten;
        int bytesRead = 0;

        // Пульсирующий LED при речи
        setLedColor("blue");

        while (http.connected() && (totalSize < 0 || bytesRead < totalSize - 44)) {
            int available = stream->available();
            if (available > 0) {
                int toRead = min(available, (int)sizeof(buf));
                int len = stream->readBytes(buf, toRead);
                if (len > 0) {
                    i2s_write(I2S_PORT, buf, len, &bytesWritten, portMAX_DELAY);
                    bytesRead += len;
                }
            } else {
                delay(1);
            }

            // LED пульс при речи
            uint8_t br = (sin(millis() / 200.0) + 1.0) * 60 + 20;
            leds.setBrightness(br);
            leds.fill(leds.Color(0, 100, 255));
            leds.show();
        }

        delay(100);
        i2s_zero_dma_buffer(I2S_PORT);
        setLedColor("mood");
    } else {
        Serial.printf("[TTS] Error: %d\n", code);
    }
    http.end();
}

// Воспроизведение музыки на колонке робота
void playMusic(String streamUrl) {
    if (streamUrl.length() == 0) return;
    Serial.printf("[MUSIC] Streaming: %s\n", streamUrl.c_str());
    state.playingMusic = true;

    // LED радуга при музыке
    setLedColor("rainbow");
    playAudioFromServer(streamUrl);
    setLedColor("mood");

    state.playingMusic = false;
}


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
    config.frame_size   = FRAMESIZE_QVGA;
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
//  МОТОРЫ — ограничение скорости
// ═══════════════════════════════════════════════════════════════
void setMotors(String action, int speed) {
    // Ограничение: не выше MAX_SPEED (200)
    speed = constrain(speed, 0, MAX_SPEED);

    switchToMotors();

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
    } else {
        analogWrite(MOTOR_L1, 0); analogWrite(MOTOR_L2, 0);
        analogWrite(MOTOR_R1, 0); analogWrite(MOTOR_R2, 0);
        state.motorsActive = false;
    }
}


// ═══════════════════════════════════════════════════════════════
//  СЕНСОРЫ
// ═══════════════════════════════════════════════════════════════
float measureDistance(int echoPin) {
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
    state.irLeft  = digitalRead(IR_LEFT)  == LOW;
    state.irRight = digitalRead(IR_RIGHT) == LOW;
}


// ═══════════════════════════════════════════════════════════════
//  LED — ЭМОЦИОНАЛЬНАЯ ЭКСПРЕССИЯ
// ═══════════════════════════════════════════════════════════════
uint32_t emotionToColor(String emotion) {
    if (emotion == "happy" || emotion == "excited")
        return leds.Color(255, 200, 0);       // Тёплый жёлтый
    if (emotion == "loving")
        return leds.Color(255, 50, 100);      // Розовый
    if (emotion == "sad")
        return leds.Color(0, 0, 100);         // Тёмно-синий
    if (emotion == "angry")
        return leds.Color(200, 0, 0);         // Красный
    if (emotion == "scared")
        return leds.Color(100, 0, 150);       // Фиолетовый
    if (emotion == "curious" || emotion == "thinking")
        return leds.Color(0, 200, 200);       // Циан
    if (emotion == "bored")
        return leds.Color(30, 30, 30);        // Тусклый
    if (emotion == "sleepy")
        return leds.Color(10, 5, 20);         // Почти выключено
    if (emotion == "surprised")
        return leds.Color(255, 255, 100);     // Яркий жёлтый
    // neutral / calm
    return leds.Color(0, 100, 50);            // Зелёный
}

void setLedColor(String color) {
    if (color == state.currentLedColor) return;
    state.currentLedColor = color;

    leds.setBrightness(state.ledBrightness);

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
    } else if (color == "mood" || color == "breathing") {
        leds.fill(emotionToColor(state.emotionExpr));
    } else {
        leds.clear();
    }
    leds.show();
}

// Анимация эмоции — быстрые вспышки при сильных эмоциях
void emotionFlash(String emotion, int count) {
    uint32_t color = emotionToColor(emotion);
    for (int i = 0; i < count; i++) {
        leds.fill(color);
        leds.setBrightness(255);
        leds.show();
        delay(80);
        leds.setBrightness(20);
        leds.show();
        delay(80);
    }
    leds.setBrightness(state.ledBrightness);
}


// ═══════════════════════════════════════════════════════════════
//  ГОЛОС — ДЕТЕКЦИЯ И ЗАПИСЬ
// ═══════════════════════════════════════════════════════════════
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
    adaptiveThreshold = noiseFloor + 150;
    Serial.printf("[MIC] Noise: %d, threshold: %d\n", noiseFloor, adaptiveThreshold);
}

bool detectVoice() {
    pinMode(MIC_PIN, INPUT);
    delay(2);
    int samples = 50;
    long sum = 0;
    for (int i = 0; i < samples; i++) {
        int val = analogRead(MIC_PIN);
        sum += abs(val - 2048);
        delayMicroseconds(200);
    }
    return (sum / samples) > adaptiveThreshold;
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

    uint32_t dataSize = totalSamples * 2;
    uint32_t fileSize = 36 + dataSize;
    uint8_t header[44] = {
        'R','I','F','F',
        (uint8_t)(fileSize), (uint8_t)(fileSize>>8),
        (uint8_t)(fileSize>>16), (uint8_t)(fileSize>>24),
        'W','A','V','E',
        'f','m','t',' ',
        16,0,0,0, 1,0, 1,0,
        0x40,0x1F,0,0,
        0x80,0x3E,0,0,
        2,0, 16,0,
        'd','a','t','a',
        (uint8_t)(dataSize), (uint8_t)(dataSize>>8),
        (uint8_t)(dataSize>>16), (uint8_t)(dataSize>>24),
    };
    file.write(header, 44);

    for (int i = 0; i < totalSamples; i++) {
        int16_t sample = (analogRead(MIC_PIN) - 2048) * 16;
        file.write((uint8_t*)&sample, 2);
        delayMicroseconds(115);
    }
    file.close();
    return filename;
}


// ═══════════════════════════════════════════════════════════════
//  СЕТЬ: STT, VISION, BRAIN
// ═══════════════════════════════════════════════════════════════
String sendAudioForSTT(String filename) {
    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) return "";

    size_t fileSize = file.size();
    uint8_t* buf = (uint8_t*)ps_malloc(fileSize);
    if (!buf) buf = (uint8_t*)malloc(fileSize);
    if (!buf) { file.close(); return ""; }
    file.read(buf, fileSize);
    file.close();

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/stt");
    http.addHeader("Content-Type", "audio/wav");
    http.setTimeout(15000);

    int code = http.POST(buf, fileSize);
    free(buf);

    String text = "";
    if (code == 200) {
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, http.getString());
        text = doc["text"].as<String>();
    }
    http.end();
    SD_MMC.remove(filename);
    return text;
}

void updateVision() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

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
    }
    http.end();
    esp_camera_fb_return(fb);
}


// ═══════════════════════════════════════════════════════════════
//  ГЛАВНЫЙ МОЗГ — /api/brain v4
// ═══════════════════════════════════════════════════════════════
void callBrain(String humanSpeech) {
    if (!state.wifiConnected) {
        offlineBehavior();
        return;
    }

    setLedColor("yellow");

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/brain");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(20000);

    DynamicJsonDocument doc(4096);
    doc["distance_front"]   = state.distanceFront;
    doc["distance_back"]    = state.distanceBack;
    doc["ir_left"]          = state.irLeft;
    doc["ir_right"]         = state.irRight;
    doc["battery_percent"]  = state.batteryPercent;

    if (humanSpeech.length() > 0) {
        doc["human_speech"] = humanSpeech;
    }

    DynamicJsonDocument visionDoc(2048);
    deserializeJson(visionDoc, state.lastVisionJson);
    doc["vision_objects"] = visionDoc;

    String body;
    serializeJson(doc, body);
    int code = http.POST(body);

    if (code == 200) {
        state.failedBrainCalls = 0;
        state.serverAlive = true;

        DynamicJsonDocument respDoc(8192);
        deserializeJson(respDoc, http.getString());

        // Парсим ответ v4
        String action     = respDoc["action"]           | "none";
        int    speed      = respDoc["speed"]             | 0;
        int    duration   = respDoc["duration_ms"]       | 0;
        int    servoAng   = respDoc["servo_angle"]       | 90;
        String ledColor   = respDoc["led_color"]         | "mood";
        int    ledBr      = respDoc["led_brightness"]    | 80;
        bool   ttsNeeded  = respDoc["tts_needed"]        | false;
        String speech     = respDoc["speech"]            | "";
        String mood       = respDoc["mood"]              | "calm";
        String emotion    = respDoc["emotion_expression"] | "neutral";
        float  voiceSpd   = respDoc["voice_speed"]       | 1.0;
        float  voiceVol   = respDoc["voice_volume"]      | 0.85;
        String interject  = respDoc["interjection"]      | "";

        // Обновляем состояние
        state.currentMood = mood;
        state.emotionExpr = emotion;
        state.ledBrightness = constrain(ledBr, 10, 255);

        // Музыка
        JsonObject music = respDoc["play_music"];
        if (!music.isNull()) {
            state.playingMusic = true;
            state.musicTitle = String(music["artist"] | "?") + " - " + String(music["title"] | "?");
            state.musicStreamUrl = music["stream_url"] | "";
            Serial.printf("[MUSIC] 🎵 %s\n", state.musicTitle.c_str());
        }

        // ── Выполняем команды ──

        // 1. Эмоциональная вспышка при сильных эмоциях
        if (emotion == "excited" || emotion == "surprised") {
            emotionFlash(emotion, 3);
        } else if (emotion == "angry") {
            emotionFlash(emotion, 2);
        }

        // 2. LED
        if (ledColor == "off" || ledColor == "mood" || ledColor == "breathing") {
            setLedColor("mood");
        } else {
            setLedColor(ledColor);
        }

        // 3. Серво камеры
        if (servoAng != state.servoAngle) {
            cameraServo.write(constrain(servoAng, 0, 180));
            state.servoAngle = servoAng;
        }

        // 4. Междометие (быстрое: "ой", "ого", "хм")
        if (interject.length() > 0 && interject != "null") {
            speakText(interject, voiceSpd * 1.2, voiceVol);
            delay(200);
        }

        // 5. Речь (с экспрессией!)
        if (ttsNeeded && speech.length() > 0) {
            speakText(speech, voiceSpd, voiceVol);
        }

        // 6. Музыка на колонке робота
        if (state.musicStreamUrl.length() > 0) {
            playMusic(state.musicStreamUrl);
            state.musicStreamUrl = "";  // одноразовый стрим
        }

        // 7. Движение (с ограничением скорости)
        speed = constrain(speed, 0, MAX_SPEED);

        if (state.distanceFront < EMERGENCY_STOP_CM && action == "forward") {
            action = "stop";
            speed = 0;
            setLedColor("red");
        }
        if (state.distanceBack < EMERGENCY_STOP_CM && action == "backward") {
            action = "stop";
            speed = 0;
        }

        if (action != "none" && action != "stop") {
            setMotors(action, speed);
            if (duration > 0) {
                delay(min(duration, 5000));
                setMotors("stop", 0);
            }
        } else if (action == "stop") {
            setMotors("stop", 0);
        }

        // Логирование
        Serial.printf("[BRAIN] mood=%s emo=%s action=%s spd=%d\n",
            mood.c_str(), emotion.c_str(), action.c_str(), speed);
        if (speech.length() > 0) {
            Serial.printf("[SAY] %s (spd=%.1f vol=%.1f)\n",
                speech.c_str(), voiceSpd, voiceVol);
        }

    } else {
        state.failedBrainCalls++;
        Serial.printf("[BRAIN] Error %d (fail #%d)\n", code, state.failedBrainCalls);
        if (state.failedBrainCalls >= 5) {
            state.serverAlive = false;
        }
        offlineBehavior();
    }
    http.end();
}


// ═══════════════════════════════════════════════════════════════
//  АВТОНОМНОЕ ПОВЕДЕНИЕ БЕЗ СЕРВЕРА
// ═══════════════════════════════════════════════════════════════
void offlineBehavior() {
    if (state.distanceFront < EMERGENCY_STOP_CM) {
        setMotors("stop", 0);
        setLedColor("red");
        delay(200);
        setMotors("backward", 120);
        delay(500);
        if (random(2) == 0) {
            setMotors("left", 120);
        } else {
            setMotors("right", 120);
        }
        delay(random(200, 600));
        setMotors("stop", 0);
    } else if (state.distanceFront < CAUTION_CM) {
        setLedColor("yellow");
        if (state.irLeft && !state.irRight) {
            setMotors("right", 100);
        } else if (!state.irLeft && state.irRight) {
            setMotors("left", 100);
        } else {
            setMotors("left", 100);
        }
        delay(300);
    } else {
        setLedColor("green");
        setMotors("forward", 100);  // спокойная скорость
    }
}


// ═══════════════════════════════════════════════════════════════
//  WI-FI
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
                state.failedBrainCalls = 0;
                state.serverAlive = true;
                state.offlineMode = false;
            }
        }
    }
}

void checkServer() {
    if (!state.wifiConnected) return;
    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/health");
    http.setTimeout(5000);
    int code = http.GET();
    state.serverAlive = (code == 200);
    http.end();
    if (!state.serverAlive) {
        Serial.println("[SERVER] Health check failed");
    }
}


// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  КЕША v4.0 — Firmware ESP32-CAM   ║");
    Serial.println("║  I2S Audio • Emotions • 170 m²    ║");
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

    // LED
    leds.begin();
    leds.setBrightness(80);
    // Стартовая анимация: перебегание цветов
    for (int i = 0; i < LED_COUNT; i++) {
        leds.setPixelColor(i, leds.Color(0, 200, 100));
        leds.show();
        delay(100);
    }
    delay(500);

    // MicroSD
    if (!SD_MMC.begin("/sdcard", true)) {
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

    // I2S Audio
    setupI2S();

    // Проверка сервера
    if (state.wifiConnected) {
        checkServer();
        if (state.serverAlive) {
            // Приветствие при включении
            speakText("Привет! Кеша на связи, версия четыре. Поехали!", 1.1, 0.9);
        }
    }

    setLedColor("mood");
    Serial.println("\n[KESHA v4] Ready! Let's roll.\n");
}


// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // 1. Wi-Fi
    checkWiFi();

    // 2. Пинг сервера (30 сек)
    if (now - state.lastStatusCheck > STATUS_CHECK) {
        checkServer();
        state.lastStatusCheck = now;
    }

    // 3. Сенсоры (10 Hz)
    if (now - state.lastSensorRead > SENSOR_INTERVAL) {
        readSensors();
        state.lastSensorRead = now;

        if (state.distanceFront < EMERGENCY_STOP_CM) {
            setMotors("stop", 0);
            setLedColor("red");
        }
    }

    // 4. Зрение (5 сек)
    if (state.wifiConnected && state.serverAlive &&
        now - state.lastVisionCall > VISION_INTERVAL) {
        updateVision();
        state.lastVisionCall = now;
    }

    // 5. Голос (500 мс)
    if (now - state.lastSoundCheck > VOICE_CHECK_INTERVAL) {
        state.lastSoundCheck = now;

        if (detectVoice()) {
            Serial.println("[MIC] Voice detected! Recording 5s...");
            setLedColor("cyan");    // Слушаю
            setMotors("stop", 0);

            String audioFile = recordAudioToSD(5000);
            if (audioFile.length() > 0) {
                String spokenText = sendAudioForSTT(audioFile);
                if (spokenText.length() > 0) {
                    state.lastSpeech = spokenText;
                    callBrain(spokenText);
                    state.lastBrainCall = millis();
                }
            }
            setLedColor("mood");
        }
    }

    // 6. Мозг: автономная жизнь (3 сек)
    if (now - state.lastBrainCall > BRAIN_INTERVAL) {
        callBrain("");
        state.lastBrainCall = now;
    }

    // 7. LED breathing
    if (state.currentLedColor == "mood") {
        uint8_t brightness = (sin(millis() / 1000.0) + 1.0) * 40 + 10;
        leds.setBrightness(min((int)brightness, state.ledBrightness));
        leds.fill(emotionToColor(state.emotionExpr));
        leds.show();
    }

    delay(50);
}
