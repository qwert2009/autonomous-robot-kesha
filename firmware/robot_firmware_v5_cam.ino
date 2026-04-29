/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║  КЕША — ПРОШИВКА ESP32-CAM v5.0                              ║
  ║  FreeRTOS мультизадачность • Одновременно: езда+речь+камера  ║
  ║  UART → ESP32-WROOM привод • I2S аудио • WS2812B LED         ║
  ╚═══════════════════════════════════════════════════════════════╝

  Архитектура:
    ESP32-CAM (этот файл) — мозг: камера, WiFi, аудио, LED
    ESP32-WROOM-32 (robot_firmware_v5_drive.ino) — тело: 4 мотора, сенсоры, серво

  Подключение ESP32-CAM:
    I2S (INMP441 + MAX98357A):
      BCLK=GPIO14, WS=GPIO15, DOUT=GPIO2 (MAX), DIN=GPIO12 (INMP)
    UART2 → WROOM:  TX=GPIO13, RX=GPIO4
    WS2812B x8:     GPIO3 (RX0, после загрузки)
    Статус LED:     GPIO33 (встроенный)
    Camera:         стандартные AI-Thinker пины

  ВАЖНО: GPIO12 — подтяжка 10кОм к GND (иначе boot в 1.8V режим)
  ВАЖНО: GPIO4 — Flash LED мигает при UART данных (косметический дефект)

  Мультизадачность:
    Core 0: uartTask — UART общение с WROOM (100 Гц)
    Core 1: loop()  — мозг, камера, аудио, LED (всё параллельно моторам!)

  Библиотеки:
    - ArduinoJson 6.x
    - Adafruit NeoPixel
  Размер Flash: "Huge APP (3MB No OTA)"
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "esp_camera.h"
#include "SD_MMC.h"
#include "esp_system.h"
#include "driver/i2s.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

// ═══════════════════════════════════════════════════════════════
//  КОНФИГУРАЦИЯ
// ═══════════════════════════════════════════════════════════════
const char* WIFI_SSID     = "YOUR_WIFI";
const char* WIFI_PASS     = "YOUR_PASSWORD";
const char* SERVER_URL    = "http://192.168.1.100:8000";

#define BRAIN_INTERVAL       3000
#define VISION_INTERVAL      5000
#define VOICE_CHECK_INTERVAL 500
#define RECONNECT_INTERVAL   10000
#define STATUS_CHECK         30000
#define VOICE_THRESHOLD      300
#define MAX_SPEED            220

// ═══ ПИНЫ ESP32-CAM ═══
// I2S аудио (INMP441 микрофон + MAX98357A усилитель)
#define I2S_BCLK  14
#define I2S_WS    15
#define I2S_DOUT   2   // → MAX98357A DIN (воспроизведение)
#define I2S_DIN   12   // ← INMP441 SD (запись)

// UART к контроллеру привода (ESP32-WROOM)
#define DRIVE_TX  13   // → WROOM RX (GPIO16)
#define DRIVE_RX   4   // ← WROOM TX (GPIO4)
#define DRIVE_BAUD 115200

// LED
#define LED_PIN    3   // WS2812B (RX0)
#define LED_COUNT  8
#define STATUS_LED 33  // Встроенный красный

// Камера AI-Thinker
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
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

struct RobotState {
    // Сенсоры (от WROOM через UART)
    volatile float distFront = 999, distRear = 999;
    volatile bool irLeft = false, irRight = false;
    volatile int batteryPct = 100;
    volatile bool dockContact = false;
    String dockStatus = "undocked";

    // Визуализация
    String lastVisionJson = "[]";
    String emotionExpr = "neutral";
    String currentMood = "calm";
    String ledColorReq = "mood";
    int ledBrightness = 80;

    // Речь
    String lastSpeech = "";

    // Сеть
    bool wifiConnected = false;
    bool serverAlive = false;
    int failedBrainCalls = 0;

    // Таймеры
    unsigned long lastBrainCall = 0;
    unsigned long lastVisionCall = 0;
    unsigned long lastSoundCheck = 0;
    unsigned long lastReconnect = 0;
    unsigned long lastStatusCheck = 0;

    // I2S
    bool i2sMode = 0;  // 0=TX (воспроизведение), 1=RX (запись)
    bool i2sReady = false;
} state;

// Очередь команд для UART task
QueueHandle_t driveCommandQueue;
struct DriveCommand {
    char cmd[64];
};

// ═══════════════════════════════════════════════════════════════
//  I2S АУДИО — переключение между записью и воспроизведением
// ═══════════════════════════════════════════════════════════════
void setupI2S_TX() {
    if (state.i2sReady) {
        i2s_driver_uninstall(I2S_NUM_0);
    }
    i2s_config_t cfg = {
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
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num = I2S_PIN_NO_CHANGE,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
    i2s_zero_dma_buffer(I2S_NUM_0);
    state.i2sMode = 0;
    state.i2sReady = true;
}

void setupI2S_RX() {
    if (state.i2sReady) {
        i2s_driver_uninstall(I2S_NUM_0);
    }
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
    };
    i2s_pin_config_t pins = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_DIN,
    };
    i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pins);
    state.i2sMode = 1;
    state.i2sReady = true;
}


// ═══════════════════════════════════════════════════════════════
//  КАМЕРА
// ═══════════════════════════════════════════════════════════════
void setupCamera() {
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer   = LEDC_TIMER_0;
    config.pin_d0  = Y2_GPIO_NUM;  config.pin_d1  = Y3_GPIO_NUM;
    config.pin_d2  = Y4_GPIO_NUM;  config.pin_d3  = Y5_GPIO_NUM;
    config.pin_d4  = Y6_GPIO_NUM;  config.pin_d5  = Y7_GPIO_NUM;
    config.pin_d6  = Y8_GPIO_NUM;  config.pin_d7  = Y9_GPIO_NUM;
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

    if (esp_camera_init(&config) == ESP_OK) {
        Serial.println("[CAM] OK");
    } else {
        Serial.println("[CAM] FAIL!");
    }
}


// ═══════════════════════════════════════════════════════════════
//  UART К ПРИВОДУ — отправка команд (thread-safe через очередь)
// ═══════════════════════════════════════════════════════════════
void sendDriveCmd(const char* fmt, ...) {
    DriveCommand dc;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(dc.cmd, sizeof(dc.cmd), fmt, ap);
    va_end(ap);
    xQueueSend(driveCommandQueue, &dc, pdMS_TO_TICKS(10));
}

// Отправка движения серверу → WROOM
void sendAction(String action, int speed, int durationMs, int servoAngle, int tiltAngle) {
    // Преобразование action в mecanum вектора
    int vy = 0, vx = 0, omega = 0;
    speed = constrain(speed, 0, MAX_SPEED);

    if (action == "forward")            { vy = speed; }
    else if (action == "backward")      { vy = -speed; }
    else if (action == "left")          { vx = -speed; }  // КРАБ
    else if (action == "right")         { vx = speed; }   // КРАБ
    else if (action == "rotate_left")   { omega = -speed; }
    else if (action == "rotate_right")  { omega = speed; }
    else { vy = 0; vx = 0; omega = 0; }

    if (vy != 0 || vx != 0 || omega != 0) {
        sendDriveCmd("M:%d,%d,%d", vy, vx, omega);
        if (durationMs > 0) {
            vTaskDelay(pdMS_TO_TICKS(min(durationMs, 5000)));
            sendDriveCmd("M:0,0,0");
        }
    } else if (action == "stop") {
        sendDriveCmd("M:0,0,0");
    }

    if (servoAngle >= 0 && servoAngle <= 180) {
        sendDriveCmd("S:%d", servoAngle);
    }
    if (tiltAngle >= 0 && tiltAngle <= 180) {
        sendDriveCmd("T:%d", tiltAngle);
    }
}


// ═══════════════════════════════════════════════════════════════
//  UART TASK — Core 0, непрерывная связь с WROOM
// ═══════════════════════════════════════════════════════════════
char uartRxBuf[128];
int uartRxIdx = 0;

void parseWROOMData(char* line) {
    if (line[0] == 'S' && line[1] == ':') {
        // S:df,db,il,ir,bat,dock — данные сенсоров
        float df, db;
        int il, ir, bat, dock;
        if (sscanf(line + 2, "%f,%f,%d,%d,%d,%d", &df, &db, &il, &ir, &bat, &dock) == 6) {
            state.distFront = df;
            state.distRear = db;
            state.irLeft = (il == 1);
            state.irRight = (ir == 1);
            state.batteryPct = bat;
            state.dockContact = (dock == 1);
        }
    } else if (line[0] == 'D' && line[1] == ':') {
        // D:status — статус стыковки
        state.dockStatus = String(line + 2);
        Serial.printf("[DOCK] %s\n", line + 2);
    }
}

void uartTask(void* param) {
    Serial.println("[UART_TASK] Started on Core 0");
    for (;;) {
        // Отправка команд из очереди
        DriveCommand dc;
        while (xQueueReceive(driveCommandQueue, &dc, 0) == pdTRUE) {
            Serial2.println(dc.cmd);
        }

        // Чтение ответов от WROOM
        while (Serial2.available()) {
            char c = Serial2.read();
            if (c == '\n' || c == '\r') {
                if (uartRxIdx > 0) {
                    uartRxBuf[uartRxIdx] = '\0';
                    parseWROOMData(uartRxBuf);
                    uartRxIdx = 0;
                }
            } else if (uartRxIdx < (int)sizeof(uartRxBuf) - 1) {
                uartRxBuf[uartRxIdx++] = c;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));  // 100 Гц
    }
}


// ═══════════════════════════════════════════════════════════════
//  LED — ЭМОЦИИ (не блокирует моторы!)
// ═══════════════════════════════════════════════════════════════
uint32_t emotionToColor(String emotion) {
    if (emotion == "happy" || emotion == "excited") return leds.Color(255, 200, 0);
    if (emotion == "loving")    return leds.Color(255, 50, 100);
    if (emotion == "sad")       return leds.Color(0, 0, 100);
    if (emotion == "angry")     return leds.Color(200, 0, 0);
    if (emotion == "scared")    return leds.Color(100, 0, 150);
    if (emotion == "curious" || emotion == "thinking") return leds.Color(0, 200, 200);
    if (emotion == "bored")     return leds.Color(30, 30, 30);
    if (emotion == "sleepy")    return leds.Color(10, 5, 20);
    if (emotion == "surprised") return leds.Color(255, 255, 100);
    return leds.Color(0, 100, 50);  // calm/neutral
}

void updateLED() {
    String color = state.ledColorReq;
    leds.setBrightness(state.ledBrightness);

    if (color == "rainbow") {
        for (int i = 0; i < LED_COUNT; i++) {
            int hue = (i * 65536 / LED_COUNT + millis() * 10) % 65536;
            leds.setPixelColor(i, leds.gamma32(leds.ColorHSV(hue)));
        }
    } else if (color == "mood" || color == "breathing") {
        uint8_t br = (sin(millis() / 1000.0) + 1.0) * 40 + 10;
        leds.setBrightness(min((int)br, state.ledBrightness));
        leds.fill(emotionToColor(state.emotionExpr));
    } else if (color == "red")    leds.fill(leds.Color(255, 0, 0));
    else if (color == "green")    leds.fill(leds.Color(0, 255, 0));
    else if (color == "blue")     leds.fill(leds.Color(0, 0, 255));
    else if (color == "yellow")   leds.fill(leds.Color(255, 200, 0));
    else if (color == "purple")   leds.fill(leds.Color(100, 0, 150));
    else if (color == "cyan")     leds.fill(leds.Color(0, 200, 200));
    else if (color == "pink")     leds.fill(leds.Color(255, 50, 100));
    else if (color == "charging") {
        // Пульсирующий зелёный при зарядке
        uint8_t br = (sin(millis() / 500.0) + 1.0) * 80 + 20;
        leds.setBrightness(br);
        leds.fill(leds.Color(0, 255, 50));
    } else {
        leds.clear();
    }
    leds.show();
}


// ═══════════════════════════════════════════════════════════════
//  ГОЛОС — I2S запись через INMP441
// ═══════════════════════════════════════════════════════════════
bool detectVoice() {
    // Быстрая проверка уровня сигнала через ADC (GPIO не мультиплексирован!)
    // INMP441 всегда подключён — читаем через I2S
    if (state.i2sMode != 1) {
        setupI2S_RX();
    }
    int16_t samples[64];
    size_t bytesRead;
    i2s_read(I2S_NUM_0, samples, sizeof(samples), &bytesRead, pdMS_TO_TICKS(50));
    if (bytesRead == 0) return false;

    long sum = 0;
    int count = bytesRead / 2;
    for (int i = 0; i < count; i++) {
        sum += abs(samples[i]);
    }
    int avg = sum / max(count, 1);
    return avg > VOICE_THRESHOLD;
}

String recordI2SAudio(int durationMs) {
    if (state.i2sMode != 1) setupI2S_RX();

    String filename = "/audio_" + String(millis()) + ".wav";
    File file = SD_MMC.open(filename, FILE_WRITE);
    if (!file) return "";

    int sampleRate = 16000;
    int totalSamples = sampleRate * durationMs / 1000;
    uint32_t dataSize = totalSamples * 2;
    uint32_t fileSize = 36 + dataSize;

    // WAV заголовок
    uint8_t header[44] = {
        'R','I','F','F',
        (uint8_t)(fileSize), (uint8_t)(fileSize>>8),
        (uint8_t)(fileSize>>16), (uint8_t)(fileSize>>24),
        'W','A','V','E', 'f','m','t',' ',
        16,0,0,0, 1,0, 1,0,
        0x80,0x3E,0,0,  // 16000 sample rate
        0x00,0x7D,0,0,  // byte rate
        2,0, 16,0,
        'd','a','t','a',
        (uint8_t)(dataSize), (uint8_t)(dataSize>>8),
        (uint8_t)(dataSize>>16), (uint8_t)(dataSize>>24),
    };
    file.write(header, 44);

    // Чтение из I2S чанками
    int16_t buf[512];
    int written = 0;
    while (written < totalSamples) {
        size_t bytesRead;
        int toRead = min(512, totalSamples - written) * 2;
        i2s_read(I2S_NUM_0, buf, toRead, &bytesRead, pdMS_TO_TICKS(100));
        if (bytesRead > 0) {
            file.write((uint8_t*)buf, bytesRead);
            written += bytesRead / 2;
        }
    }
    file.close();
    Serial.printf("[REC] %s — %d samples\n", filename.c_str(), written);
    return filename;
}


// ═══════════════════════════════════════════════════════════════
//  ВОСПРОИЗВЕДЕНИЕ — I2S TX через MAX98357A (МОТОРЫ НЕ СТОПЯТ!)
// ═══════════════════════════════════════════════════════════════
void playAudioStream(WiFiClient* stream, int totalSize) {
    if (state.i2sMode != 0) setupI2S_TX();

    // Пропустить WAV заголовок
    uint8_t header[44];
    stream->readBytes(header, 44);

    uint8_t buf[1024];
    size_t bytesWritten;
    int bytesRead = 0;
    int dataSize = totalSize > 0 ? totalSize - 44 : -1;

    while (stream->connected() && (dataSize < 0 || bytesRead < dataSize)) {
        int avail = stream->available();
        if (avail > 0) {
            int toRead = min(avail, (int)sizeof(buf));
            int len = stream->readBytes(buf, toRead);
            if (len > 0) {
                i2s_write(I2S_NUM_0, buf, len, &bytesWritten, portMAX_DELAY);
                bytesRead += len;
            }
        } else {
            vTaskDelay(1);
        }
    }
    delay(50);
    i2s_zero_dma_buffer(I2S_NUM_0);
}

void speakText(String text, float voiceSpeed, float voiceVolume) {
    if (text.length() == 0) return;
    Serial.printf("[TTS] \"%s\"\n", text.c_str());

    if (state.i2sMode != 0) setupI2S_TX();

    // LED пульс при речи
    state.ledColorReq = "blue";

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/tts");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    DynamicJsonDocument doc(512);
    doc["text"] = text;
    doc["voice_speed"] = voiceSpeed;
    doc["voice_volume"] = voiceVolume;
    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code == 200) {
        playAudioStream(http.getStreamPtr(), http.getSize());
    }
    http.end();

    state.ledColorReq = "mood";
}

void playMusic(String streamUrl) {
    if (streamUrl.length() == 0) return;
    if (state.i2sMode != 0) setupI2S_TX();

    state.ledColorReq = "rainbow";

    HTTPClient http;
    http.begin(String(SERVER_URL) + streamUrl);
    http.setTimeout(15000);
    int code = http.GET();
    if (code == 200) {
        playAudioStream(http.getStreamPtr(), http.getSize());
    }
    http.end();

    state.ledColorReq = "mood";
}


// ═══════════════════════════════════════════════════════════════
//  СЕТЬ — STT, VISION, BRAIN (мотори при этом РАБОТАЮТ!)
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

void callBrain(String humanSpeech) {
    if (!state.wifiConnected) return;

    state.ledColorReq = "yellow";

    HTTPClient http;
    http.begin(String(SERVER_URL) + "/api/brain");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(20000);

    DynamicJsonDocument doc(4096);
    doc["distance_front"]  = state.distFront;
    doc["distance_back"]   = state.distRear;
    doc["ir_left"]         = (bool)state.irLeft;
    doc["ir_right"]        = (bool)state.irRight;
    doc["battery_percent"] = state.batteryPct;
    doc["dock_contact"]    = (bool)state.dockContact;
    doc["dock_status"]     = state.dockStatus;

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

        DynamicJsonDocument resp(8192);
        deserializeJson(resp, http.getString());

        String action     = resp["action"]             | "none";
        int speed         = resp["speed"]              | 0;
        int duration      = resp["duration_ms"]        | 0;
        int servoAng      = resp["servo_angle"]        | 90;
        int tiltAng       = resp["servo_tilt"]         | 90;
        String ledColor   = resp["led_color"]          | "mood";
        int ledBr         = resp["led_brightness"]     | 80;
        bool ttsNeeded    = resp["tts_needed"]         | false;
        String speech     = resp["speech"]             | "";
        String emotion    = resp["emotion_expression"] | "neutral";
        float voiceSpd    = resp["voice_speed"]        | 1.0;
        float voiceVol    = resp["voice_volume"]       | 0.85;
        String interject  = resp["interjection"]       | "";
        bool autoDock     = resp["auto_dock"]          | false;

        state.emotionExpr = emotion;
        state.ledBrightness = constrain(ledBr, 10, 255);
        state.ledColorReq = ledColor;

        // Автостыковка
        if (autoDock) {
            sendDriveCmd("D:1");
        }

        // Междометие
        if (interject.length() > 0 && interject != "null") {
            speakText(interject, voiceSpd * 1.2, voiceVol);
            delay(100);
        }

        // Основная речь — МОТОРЫ ПРОДОЛЖАЮТ РАБОТАТЬ!
        if (ttsNeeded && speech.length() > 0) {
            speakText(speech, voiceSpd, voiceVol);
        }

        // Музыка
        JsonObject music = resp["play_music"];
        if (!music.isNull()) {
            String streamUrl = music["stream_url"] | "";
            if (streamUrl.length() > 0) {
                playMusic(streamUrl);
            }
        }

        // Движение — отправляем команду WROOM через UART
        sendAction(action, speed, duration, servoAng, tiltAng);

        Serial.printf("[BRAIN] emo=%s act=%s spd=%d\n",
                      emotion.c_str(), action.c_str(), speed);

    } else {
        state.failedBrainCalls++;
        if (state.failedBrainCalls >= 5) state.serverAlive = false;
    }
    http.end();
}


// ═══════════════════════════════════════════════════════════════
//  WI-FI
// ═══════════════════════════════════════════════════════════════
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[WiFi] Connecting");
    int att = 0;
    while (WiFi.status() != WL_CONNECTED && att < 20) {
        delay(500); Serial.print("."); att++;
    }
    state.wifiConnected = (WiFi.status() == WL_CONNECTED);
    if (state.wifiConnected) {
        Serial.println("\n[WiFi] " + WiFi.localIP().toString());
    } else {
        Serial.println("\n[WiFi] FAIL");
    }
}

void checkWiFi() {
    if (WiFi.status() != WL_CONNECTED) {
        state.wifiConnected = false;
        if (millis() - state.lastReconnect > RECONNECT_INTERVAL) {
            state.lastReconnect = millis();
            WiFi.disconnect(); delay(100);
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            int att = 0;
            while (WiFi.status() != WL_CONNECTED && att < 10) { delay(500); att++; }
            state.wifiConnected = (WiFi.status() == WL_CONNECTED);
        }
    }
}


// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n╔═══════════════════════════════════════╗");
    Serial.println("║  КЕША v5.0 — ESP32-CAM Brain          ║");
    Serial.println("║  FreeRTOS • 4WD Mecanum • Multitask   ║");
    Serial.println("╚═══════════════════════════════════════╝\n");

    // Статус LED
    pinMode(STATUS_LED, OUTPUT);
    digitalWrite(STATUS_LED, LOW);

    // WS2812B
    leds.begin();
    leds.setBrightness(80);
    for (int i = 0; i < LED_COUNT; i++) {
        leds.setPixelColor(i, leds.Color(0, 200, 100));
        leds.show(); delay(80);
    }

    // UART к приводу
    Serial2.begin(DRIVE_BAUD, SERIAL_8N1, DRIVE_RX, DRIVE_TX);
    driveCommandQueue = xQueueCreate(16, sizeof(DriveCommand));
    Serial.println("[UART] → WROOM on GPIO13/GPIO4");

    // MicroSD
    if (!SD_MMC.begin("/sdcard", true)) {
        Serial.println("[SD] Mount failed!");
    } else {
        Serial.printf("[SD] OK %lluMB\n", SD_MMC.totalBytes() / (1024*1024));
    }

    // Камера
    setupCamera();

    // I2S (начинаем в режиме воспроизведения)
    setupI2S_TX();
    Serial.println("[I2S] Audio TX ready (MAX98357A)");

    // WiFi
    connectWiFi();

    // Запуск UART задачи на Core 0
    xTaskCreatePinnedToCore(
        uartTask,        // функция
        "uartTask",      // имя
        4096,            // стек
        NULL,            // параметры
        2,               // приоритет (выше loop)
        NULL,            // хэндл
        0                // Core 0
    );

    // Проверка сервера
    if (state.wifiConnected) {
        HTTPClient http;
        http.begin(String(SERVER_URL) + "/api/health");
        http.setTimeout(5000);
        state.serverAlive = (http.GET() == 200);
        http.end();

        if (state.serverAlive) {
            speakText("Привет! Кеша версия пять. Четыре колеса, полный привод, поехали!", 1.1, 0.9);
        }
    }

    state.ledColorReq = "mood";
    Serial.println("[KESHA v5.0] Ready! Multitasking ON.\n");
}


// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP — Core 1 (моторы на Core 0 через UART, не блокируют!)
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // 1. WiFi
    checkWiFi();

    // 2. Пинг сервера (30 сек)
    if (now - state.lastStatusCheck > STATUS_CHECK) {
        if (state.wifiConnected) {
            HTTPClient http;
            http.begin(String(SERVER_URL) + "/api/health");
            http.setTimeout(5000);
            state.serverAlive = (http.GET() == 200);
            http.end();
        }
        state.lastStatusCheck = now;
    }

    // 3. Зрение (5 сек) — камера снимает пока моторы едут!
    if (state.wifiConnected && state.serverAlive &&
        now - state.lastVisionCall > VISION_INTERVAL) {
        updateVision();
        state.lastVisionCall = now;
    }

    // 4. Голос (500 мс) — слушает пока моторы едут!
    if (now - state.lastSoundCheck > VOICE_CHECK_INTERVAL) {
        state.lastSoundCheck = now;
        if (detectVoice()) {
            Serial.println("[MIC] Voice! Recording 5s...");
            state.ledColorReq = "cyan";
            // НЕ СТОПИМ моторы! Слушаем на ходу!
            String audioFile = recordI2SAudio(5000);
            if (audioFile.length() > 0) {
                String text = sendAudioForSTT(audioFile);
                if (text.length() > 0) {
                    state.lastSpeech = text;
                    callBrain(text);
                    state.lastBrainCall = millis();
                }
            }
            state.ledColorReq = "mood";
        }
    }

    // 5. Мозг: автономная жизнь (3 сек)
    if (now - state.lastBrainCall > BRAIN_INTERVAL) {
        callBrain("");
        state.lastBrainCall = now;
    }

    // 6. Зарядка — LED индикация
    if (state.dockContact) {
        state.ledColorReq = "charging";
    }

    // 7. LED анимация (каждый цикл)
    updateLED();

    delay(50);
}
