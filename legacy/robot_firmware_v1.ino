/*
  Прошивка ESP32-CAM для ИИ-робота
  Подключается к Wi-Fi, отправляет изображения/аудио на ПК API-сервер
  Управляет моторами через MX1508 (L298N mini)

  Подключение:
    MX1508:  IN1=GPIO12, IN2=GPIO13, IN3=GPIO14, IN4=GPIO15
    HC-SR04: TRIG=GPIO2, ECHO=GPIO4
    MPU6050: SDA=GPIO26, SCL=GPIO27 (I2C)

  Библиотеки (Arduino IDE):
    - ESP32 Board Support
    - HTTPClient
    - ArduinoJson
    - Wire (I2C для MPU6050)
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include "esp_camera.h"

// === КОНФИГУРАЦИЯ ===
const char* WIFI_SSID = "YOUR_WIFI";
const char* WIFI_PASS = "YOUR_PASSWORD";
const char* SERVER_URL = "http://192.168.1.100:8000";  // IP вашего ПК

// Пины MX1508 (L298N mini)
#define MOTOR_L1 12
#define MOTOR_L2 13
#define MOTOR_R1 14
#define MOTOR_R2 15

// HC-SR04
#define TRIG_PIN 2
#define ECHO_PIN 4

// === Камера ESP32-CAM (AI-Thinker) ===
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

// === МОТОРЫ ===
void motorForward(int speed) {
    analogWrite(MOTOR_L1, speed);
    analogWrite(MOTOR_L2, 0);
    analogWrite(MOTOR_R1, speed);
    analogWrite(MOTOR_R2, 0);
}

void motorBackward(int speed) {
    analogWrite(MOTOR_L1, 0);
    analogWrite(MOTOR_L2, speed);
    analogWrite(MOTOR_R1, 0);
    analogWrite(MOTOR_R2, speed);
}

void motorLeft(int speed) {
    analogWrite(MOTOR_L1, 0);
    analogWrite(MOTOR_L2, speed);
    analogWrite(MOTOR_R1, speed);
    analogWrite(MOTOR_R2, 0);
}

void motorRight(int speed) {
    analogWrite(MOTOR_L1, speed);
    analogWrite(MOTOR_L2, 0);
    analogWrite(MOTOR_R1, 0);
    analogWrite(MOTOR_R2, speed);
}

void motorStop() {
    analogWrite(MOTOR_L1, 0);
    analogWrite(MOTOR_L2, 0);
    analogWrite(MOTOR_R1, 0);
    analogWrite(MOTOR_R2, 0);
}

// === HC-SR04 ===
float getDistance() {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long duration = pulseIn(ECHO_PIN, HIGH, 30000);
    return duration * 0.034 / 2.0;
}

// === Отправка кадра на сервер ===
String sendImageToServer() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) return "camera_error";

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/vision");
    http.addHeader("Content-Type", "image/jpeg");
    int code = http.POST(fb->buf, fb->len);
    String response = "";
    if (code == 200) {
        response = http.getString();
    }
    http.end();
    esp_camera_fb_return(fb);
    return response;
}

// === Запрос навигации ===
void requestNavigation(float distance) {
    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/navigate");
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["distance_front"] = distance;
    doc["ir_left"] = false;
    doc["ir_right"] = false;

    String body;
    serializeJson(doc, body);
    int code = http.POST(body);

    if (code == 200) {
        String resp = http.getString();
        StaticJsonDocument<200> respDoc;
        deserializeJson(respDoc, resp);

        const char* action = respDoc["action"];
        int speed = respDoc["speed"];
        int duration = respDoc["duration_ms"];

        if (strcmp(action, "forward") == 0) motorForward(speed);
        else if (strcmp(action, "backward") == 0) motorBackward(speed);
        else if (strcmp(action, "left") == 0) motorLeft(speed);
        else if (strcmp(action, "right") == 0) motorRight(speed);
        else motorStop();

        if (duration > 0) {
            delay(duration);
            motorStop();
        }
    }
    http.end();
}

void setup() {
    Serial.begin(115200);

    // Моторы
    pinMode(MOTOR_L1, OUTPUT);
    pinMode(MOTOR_L2, OUTPUT);
    pinMode(MOTOR_R1, OUTPUT);
    pinMode(MOTOR_R2, OUTPUT);

    // HC-SR04
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_PIN, INPUT);

    // Wi-Fi
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("Connecting WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected: " + WiFi.localIP().toString());

    // Камера
    setupCamera();

    Serial.println("Robot ready!");
}

void loop() {
    // 1. Измерить расстояние
    float dist = getDistance();
    Serial.printf("Distance: %.1f cm\n", dist);

    // 2. Запросить навигацию у сервера
    requestNavigation(dist);

    // 3. Каждые 5 секунд отправлять кадр на распознавание
    static unsigned long lastVision = 0;
    if (millis() - lastVision > 5000) {
        String visionResult = sendImageToServer();
        Serial.println("Vision: " + visionResult);
        lastVision = millis();
    }

    delay(100);  // 10 Hz основной цикл
}
