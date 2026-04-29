/*
  ╔═══════════════════════════════════════════════════════════╗
  ║  ПРОШИВКА ESP32-CAM v2.0 — АВТОНОМНЫЙ РОБОТ              ║
  ║  Один endpoint /api/brain — отправляет ВСЁ, получает ВСЁ  ║
  ╚═══════════════════════════════════════════════════════════╝

  Подключение:
    MX1508:     IN1=GPIO12, IN2=GPIO13, IN3=GPIO2, IN4=GPIO4
    HC-SR04 F:  TRIG=GPIO14, ECHO=GPIO33
    HC-SR04 B:  TRIG=GPIO14, ECHO=GPIO16 (общий TRIG)
    Серво SG90:  GPIO15
    WS2812B:    GPIO3 (RX0)
    MAX9814:    GPIO33 (ADC — общий с HC-SR04 ECHO, мультиплекс)
    ИК левый:   GPIO36 (VP)
    ИК правый:  GPIO39 (VN)
    MicroSD:    встроен

  Библиотеки:
    - ArduinoJson
    - ESP32Servo
    - Adafruit NeoPixel
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32Servo.h>
#include <Adafruit_NeoPixel.h>
#include "esp_camera.h"
#include "driver/i2s.h"
#include "SD_MMC.h"

// ═══ КОНФИГУРАЦИЯ ═══
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* SERVER_URL = "http://192.168.1.100:8000";

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

#define MIC_PIN    33  // MAX9814 аналоговый выход (ADC)

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

// ═══ ОБЪЕКТЫ ═══
Servo cameraServo;
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// ═══ СОСТОЯНИЕ РОБОТА ═══
struct RobotState {
    float distanceFront = 999;
    float distanceBack = 999;
    bool irLeft = false;
    bool irRight = false;
    int batteryPercent = 100;
    int servoAngle = 90;
    String lastVisionJson = "[]";
    String lastSpeech = "";
    bool isRecordingAudio = false;
    unsigned long lastBrainCall = 0;
    unsigned long lastVisionCall = 0;
    unsigned long lastSoundCheck = 0;
    String currentLedColor = "off";
} state;

// ═══ ФУНКЦИИ КАМЕРЫ ═══
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;  // 320x240
    config.jpeg_quality = 12;
    config.fb_count = 1;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
    }
}

// ═══ МОТОРЫ ═══
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

// ═══ HC-SR04 ═══
float measureDistance(int echoPin) {
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
    delay(30);  // Пауза между двумя HC-SR04
    state.distanceBack = measureDistance(ECHO_B_PIN);
    state.irLeft = digitalRead(IR_LEFT) == LOW;   // LOW = препятствие
    state.irRight = digitalRead(IR_RIGHT) == LOW;
    // Батарея — простой ADC замер через делитель
    // (нужен реальный делитель напряжения на свободном ADC пине)
    // state.batteryPercent = map(analogRead(BAT_PIN), 2400, 3200, 0, 100);
}

// ═══ LED КОЛЬЦО ═══
void setLedColor(String color) {
    if (color == state.currentLedColor) return;
    state.currentLedColor = color;

    if (color == "red") {
        for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(255, 0, 0));
    } else if (color == "green") {
        for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(0, 255, 0));
    } else if (color == "blue") {
        for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(0, 0, 255));
    } else if (color == "yellow") {
        for (int i = 0; i < LED_COUNT; i++) leds.setPixelColor(i, leds.Color(255, 200, 0));
    } else if (color == "rainbow") {
        for (int i = 0; i < LED_COUNT; i++) {
            int hue = (i * 65536 / LED_COUNT + millis() * 10) % 65536;
            leds.setPixelColor(i, leds.gamma32(leds.ColorHSV(hue)));
        }
    } else {  // off
        leds.clear();
    }
    leds.show();
}

// ═══ ОБНАРУЖЕНИЕ ГОЛОСА (простой порог на MAX9814) ═══
bool detectVoice() {
    int samples = 50;
    long sum = 0;
    for (int i = 0; i < samples; i++) {
        int val = analogRead(MIC_PIN);
        sum += abs(val - 2048);  // Центр ADC = 2048
        delayMicroseconds(200);
    }
    int avgAmplitude = sum / samples;
    return avgAmplitude > 300;  // Порог — настроить экспериментально
}

// ═══ ЗАПИСЬ АУДИО (WAV на SD карту) ═══
String recordAudioToSD(int durationMs) {
    // Простая аналоговая запись через ADC
    int sampleRate = 8000;
    int totalSamples = sampleRate * durationMs / 1000;
    String filename = "/audio_" + String(millis()) + ".wav";

    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) return "";

    // WAV header (16-bit mono 8kHz)
    uint32_t dataSize = totalSamples * 2;
    uint32_t fileSize = 36 + dataSize;
    uint8_t header[44] = {
        'R','I','F','F',
        (uint8_t)(fileSize), (uint8_t)(fileSize>>8), (uint8_t)(fileSize>>16), (uint8_t)(fileSize>>24),
        'W','A','V','E',
        'f','m','t',' ',
        16,0,0,0,      // chunk size
        1,0,            // PCM
        1,0,            // mono
        0x40,0x1F,0,0,  // 8000 Hz
        0x80,0x3E,0,0,  // byte rate
        2,0,            // block align
        16,0,           // bits per sample
        'd','a','t','a',
        (uint8_t)(dataSize), (uint8_t)(dataSize>>8), (uint8_t)(dataSize>>16), (uint8_t)(dataSize>>24),
    };
    file.write(header, 44);

    for (int i = 0; i < totalSamples; i++) {
        int16_t sample = (analogRead(MIC_PIN) - 2048) * 16;  // Усиление
        file.write((uint8_t*)&sample, 2);
        delayMicroseconds(125 - 10);  // ~8kHz минус время ADC
    }
    file.close();
    return filename;
}

// ═══ ОТПРАВКА АУДИО НА СЕРВЕР STT ═══
String sendAudioForSTT(String filename) {
    File file = SD_MMC.open(filename, FILE_READ);
    if (!file) return "";

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/stt");
    http.addHeader("Content-Type", "audio/wav");

    size_t fileSize = file.size();
    uint8_t* buf = (uint8_t*)malloc(fileSize);
    if (!buf) { file.close(); return ""; }
    file.read(buf, fileSize);
    file.close();

    int code = http.POST(buf, fileSize);
    free(buf);

    String text = "";
    if (code == 200) {
        DynamicJsonDocument doc(1024);
        deserializeJson(doc, http.getString());
        text = doc["text"].as<String>();
    }
    http.end();

    // Удалить временный файл
    SD_MMC.remove(filename);
    return text;
}

// ═══ ОТПРАВКА КАДРА НА VISION ═══
void updateVision() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return;

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/vision");
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.POST(fb->buf, fb->len);

    if (code == 200) {
        DynamicJsonDocument doc(4096);
        deserializeJson(doc, http.getString());
        // Сериализуем objects обратно в строку для brain
        JsonArray objects = doc["objects"];
        serializeJson(objects, state.lastVisionJson);
    }
    http.end();
    esp_camera_fb_return(fb);
}

// ═══ ЗАПРОС TTS АУДИО И ВОСПРОИЗВЕДЕНИЕ ═══
void speakText(String text) {
    if (text.length() == 0) return;

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/tts");
    http.addHeader("Content-Type", "application/json");

    DynamicJsonDocument doc(512);
    doc["text"] = text;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code == 200) {
        // Получаем WAV и воспроизводим через DAC/I2S
        // (упрощённо — требует настройки I2S для MAX98357A)
        WiFiClient* stream = http.getStreamPtr();
        // TODO: I2S воспроизведение WAV потока
        Serial.println("[TTS] Received audio, playing...");
    }
    http.end();
}

// ═══ ГЛАВНЫЙ ВЫЗОВ: /api/brain ═══
void callBrain(String humanSpeech) {
    setLedColor("yellow");  // Думаю...

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/brain");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);  // 15 сек таймаут для LLM

    DynamicJsonDocument doc(4096);
    doc["distance_front"] = state.distanceFront;
    doc["distance_back"] = state.distanceBack;
    doc["ir_left"] = state.irLeft;
    doc["ir_right"] = state.irRight;
    doc["battery_percent"] = state.batteryPercent;

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
        DynamicJsonDocument respDoc(2048);
        deserializeJson(respDoc, http.getString());

        // Выполняем команды от мозга
        String action = respDoc["action"] | "none";
        int speed = respDoc["speed"] | 0;
        int duration = respDoc["duration_ms"] | 0;
        int servoAngle = respDoc["servo_angle"] | 90;
        String ledColor = respDoc["led_color"] | "off";
        String speech = respDoc["speech"] | "";
        bool ttsNeeded = respDoc["tts_needed"] | false;

        // 1. LED
        setLedColor(ledColor);

        // 2. Серво камеры
        if (servoAngle != state.servoAngle) {
            cameraServo.write(servoAngle);
            state.servoAngle = servoAngle;
        }

        // 3. Говорим
        if (ttsNeeded && speech.length() > 0) {
            setLedColor("blue");  // Говорю
            speakText(speech);
        }

        // 4. Двигаемся
        setMotors(action, speed);
        if (duration > 0) {
            delay(duration);
            setMotors("stop", 0);
        }

        Serial.printf("[BRAIN] action=%s speed=%d speech=%s\n",
                      action.c_str(), speed, speech.c_str());
    } else {
        Serial.printf("[BRAIN] Error: %d\n", code);
        // Fallback: простое избегание
        if (state.distanceFront < 20) {
            setMotors("backward", 150);
            delay(500);
            setMotors("left", 150);
            delay(300);
        }
        setMotors("stop", 0);
    }
    http.end();
}


// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n[ROBOT] Initializing...");

    // Моторы
    pinMode(MOTOR_L1, OUTPUT);
    pinMode(MOTOR_L2, OUTPUT);
    pinMode(MOTOR_R1, OUTPUT);
    pinMode(MOTOR_R2, OUTPUT);

    // HC-SR04
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_F_PIN, INPUT);
    pinMode(ECHO_B_PIN, INPUT);

    // ИК датчики
    pinMode(IR_LEFT, INPUT);
    pinMode(IR_RIGHT, INPUT);

    // Серво
    cameraServo.attach(SERVO_PIN);
    cameraServo.write(90);  // Центр

    // LED
    leds.begin();
    setLedColor("rainbow");

    // MicroSD
    if (!SD_MMC.begin("/sdcard", true)) {  // 1-bit mode
        Serial.println("[SD] Mount failed!");
    } else {
        Serial.println("[SD] Mounted OK");
    }

    // Wi-Fi
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
        delay(500);
        Serial.print(".");
        attempts++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\n[WiFi] Connected: " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WiFi] FAILED! Running offline.");
    }

    // Камера
    setupCamera();

    setLedColor("green");
    Serial.println("[ROBOT] Ready! Starting autonomous life...\n");
}


// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // 1. ВСЕГДА: Читать сенсоры (10 раз/сек)
    readSensors();

    // 2. ЭКСТРЕННОЕ ТОРМОЖЕНИЕ (локально, без сервера!)
    if (state.distanceFront < 10) {
        setMotors("stop", 0);
        setLedColor("red");
    }

    // 3. VISION: каждые 5 секунд отправлять кадр
    if (now - state.lastVisionCall > 5000) {
        updateVision();
        state.lastVisionCall = now;
    }

    // 4. ГОЛОС: каждые 500мс проверять микрофон
    if (now - state.lastSoundCheck > 500) {
        state.lastSoundCheck = now;
        if (detectVoice()) {
            Serial.println("[MIC] Voice detected! Recording 5 sec...");
            setLedColor("blue");  // Слушаю
            setMotors("stop", 0);

            // Записать 5 секунд
            String audioFile = recordAudioToSD(5000);
            if (audioFile.length() > 0) {
                // Отправить на STT
                String spokenText = sendAudioForSTT(audioFile);
                Serial.println("[STT] Heard: " + spokenText);

                if (spokenText.length() > 0) {
                    state.lastSpeech = spokenText;
                    // Сразу вызвать мозг с речью
                    callBrain(spokenText);
                    state.lastBrainCall = millis();
                }
            }
        }
    }

    // 5. МОЗГ: каждые 3 секунды (автономная жизнь)
    if (now - state.lastBrainCall > 3000) {
        callBrain("");  // Без речи — просто "что делать?"
        state.lastBrainCall = now;
    }

    delay(100);  // 10 Hz
}
