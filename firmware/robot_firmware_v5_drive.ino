/*
  ╔═══════════════════════════════════════════════════════════════╗
  ║  КЕША — КОНТРОЛЛЕР ПРИВОДА v5.0                              ║
  ║  ESP32-WROOM-32 • 4WD Mecanum • Автостыковка с док-станцией  ║
  ║  UART-slave от ESP32-CAM • Индивидуальное управление колёс   ║
  ╚═══════════════════════════════════════════════════════════════╝

  Подключение:
    MX1508 #1:  FL_IN1=GPIO25, FL_IN2=GPIO26 | FR_IN1=GPIO27, FR_IN2=GPIO14
    MX1508 #2:  BL_IN1=GPIO33, BL_IN2=GPIO32 | BR_IN1=GPIO23, BR_IN2=GPIO22
    HC-SR04 F:  TRIG=GPIO18, ECHO=GPIO19
    HC-SR04 R:  TRIG=GPIO18 (общий), ECHO=GPIO21
    E18 IR L:   GPIO34 (INPUT_ONLY)
    E18 IR R:   GPIO35 (INPUT_ONLY)
    SG90 Pan:    GPIO17 (лево-право)
    SG90 Tilt:   GPIO5  (вверх-вниз)
    Батарея ADC: GPIO36 (VP, делитель 4.7kΩ/10kΩ → макс 4.2V на входе)
    Док контакт: GPIO39 (VN, делитель — HIGH когда на контактах есть напряжение)
    UART2 TX:    GPIO4  → ESP32-CAM GPIO4 (RX)
    UART2 RX:    GPIO16 ← ESP32-CAM GPIO13 (TX)

  Протокол UART (115200 бод):
    Входящие (от CAM):
      M:vy,vx,omega\n   — Mecanum (-255..255)
      W:fl,fr,bl,br\n   — Колёса напрямую (-255..255)
      S:angle\n          — Серво (0-180)
      D:1\n / D:0\n      — Автостыковка вкл/выкл
      ?\n                — Запрос сенсоров
    Исходящие (к CAM):
      S:df,db,il,ir,bat,dock\n  — Сенсоры
      D:status\n                  — Статус дока
      K\n                         — OK

  Библиотеки: ESP32Servo
  Плата: ESP32 Dev Module, Flash: 4MB, Upload: 921600
*/

#include <ESP32Servo.h>
#include <Wire.h>

// ═══ HMC5883L (HW-127) — I2C магнитометр/компас ═══
#define HMC5883L_ADDR  0x1E
#define HMC_SDA        13    // I2C SDA (свободный GPIO)
#define HMC_SCL        15    // I2C SCL (свободный GPIO)
// Регистры HMC5883L
#define HMC_CONFIG_A   0x00
#define HMC_CONFIG_B   0x01
#define HMC_MODE       0x02
#define HMC_DATA_X_H   0x03

// ═══ ПИНЫ МОТОРОВ (2x MX1508 = 4 мотора) ═══
#define FL_IN1  25   // Передний левый
#define FL_IN2  26
#define FR_IN1  27   // Передний правый
#define FR_IN2  14
#define BL_IN1  33   // Задний левый
#define BL_IN2  32
#define BR_IN1  23   // Задний правый
#define BR_IN2  22

// ═══ СЕНСОРЫ ═══
#define TRIG_PIN    18
#define ECHO_F_PIN  19   // Передний HC-SR04
#define ECHO_R_PIN  21   // Задний HC-SR04
#define IR_LEFT     34
#define IR_RIGHT    35
#define BAT_ADC     36   // VP — делитель напряжения
#define DOCK_ADC    39   // VN — контакт зарядной станции

// ═══ СЕРВО (pan-tilt подвес) ═══
#define SERVO_PAN_PIN   17
#define SERVO_TILT_PIN   5

// ═══ UART К ESP32-CAM ═══
#define CAM_TX  4    // Наш TX → CAM RX
#define CAM_RX  16   // Наш RX ← CAM TX
#define UART_BAUD 115200

// ═══ КОНСТАНТЫ ═══
#define MAX_SPEED        220
#define EMERGENCY_CM     8
#define CAUTION_CM       20
#define SENSOR_INTERVAL  50    // 20 Гц
#define REPORT_INTERVAL  100   // 10 Гц
#define DOCK_VOLTAGE_THRESHOLD 2000  // АЦП порог (из 4095)
#define BAT_LOW_VOLTAGE  3200  // ~10.5V через делитель → разряжен

Servo panServo;
Servo tiltServo;

// ═══ СОСТОЯНИЕ ═══
struct DriveState {
    // Целевые скорости колёс (-255..255)
    int wheelFL = 0, wheelFR = 0, wheelBL = 0, wheelBR = 0;
    // Сенсоры
    float distFront = 999, distRear = 999;
    bool irLeft = false, irRight = false;
    float batteryV = 12.6;
    int batteryPct = 100;
    bool dockContact = false;
    // HMC5883L компас
    float heading = 0.0;       // градусы 0-360
    int16_t magX = 0, magY = 0, magZ = 0;  // сырые данные
    bool compassOK = false;
    // Время
    unsigned long lastSensor = 0;
    unsigned long lastReport = 0;
    // Автодок
    bool dockRequested = false;
    int dockPhase = 0;  // 0=нет, 1=разворот, 2=назад, 3=стыковка, 4=подзарядка
    unsigned long dockTimer = 0;
    // Серво pan-tilt
    int panAngle = 90;
    int tiltAngle = 90;
} state;

// ═══ UART буфер ═══
char uartBuf[128];
int uartIdx = 0;


// ═══════════════════════════════════════════════════════════════
//  HMC5883L КОМПАС — инициализация и чтение
// ═══════════════════════════════════════════════════════════════
void hmc5883l_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(HMC5883L_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}

bool hmc5883l_init() {
    Wire.begin(HMC_SDA, HMC_SCL);
    Wire.setClock(100000);  // 100 кГц I2C

    // Проверка ID (регистры 10,11,12 = 'H','4','3')
    Wire.beginTransmission(HMC5883L_ADDR);
    Wire.write(0x0A);
    if (Wire.endTransmission() != 0) return false;
    Wire.requestFrom((uint8_t)HMC5883L_ADDR, (uint8_t)3);
    if (Wire.available() < 3) return false;
    char id_a = Wire.read(), id_b = Wire.read(), id_c = Wire.read();
    if (id_a != 'H' || id_b != '4' || id_c != '3') return false;

    // Config A: 8 samples average, 15 Hz, normal measurement
    hmc5883l_write(HMC_CONFIG_A, 0x70);
    // Config B: Gain = 1.3 Ga (default)
    hmc5883l_write(HMC_CONFIG_B, 0x20);
    // Mode: Continuous measurement
    hmc5883l_write(HMC_MODE, 0x00);

    delay(10);
    return true;
}

void readCompass() {
    if (!state.compassOK) return;

    Wire.beginTransmission(HMC5883L_ADDR);
    Wire.write(HMC_DATA_X_H);
    if (Wire.endTransmission() != 0) {
        state.compassOK = false;
        return;
    }
    Wire.requestFrom((uint8_t)HMC5883L_ADDR, (uint8_t)6);
    if (Wire.available() < 6) return;

    // Порядок данных: X_H, X_L, Z_H, Z_L, Y_H, Y_L (внимание: Z перед Y!)
    state.magX = (Wire.read() << 8) | Wire.read();
    state.magZ = (Wire.read() << 8) | Wire.read();
    state.magY = (Wire.read() << 8) | Wire.read();

    // Вычисление heading (азимут) в градусах
    float headingRad = atan2((float)state.magY, (float)state.magX);
    // Магнитное склонение для Ашхабада ≈ +4.3° (0.0750 рад)
    // Измените для вашего региона: https://www.ngdc.noaa.gov/geomag/declination.shtml
    headingRad += 0.0750;
    if (headingRad < 0) headingRad += 2.0 * PI;
    if (headingRad > 2.0 * PI) headingRad -= 2.0 * PI;

    state.heading = headingRad * 180.0 / PI;
}


// ═══════════════════════════════════════════════════════════════
//  МОТОРЫ — PWM управление каждым колесом
// ═══════════════════════════════════════════════════════════════
void setWheel(int in1, int in2, int speed) {
    speed = constrain(speed, -MAX_SPEED, MAX_SPEED);
    if (speed > 0) {
        analogWrite(in1, speed);
        analogWrite(in2, 0);
    } else if (speed < 0) {
        analogWrite(in1, 0);
        analogWrite(in2, -speed);
    } else {
        analogWrite(in1, 0);
        analogWrite(in2, 0);
    }
}

void setAllWheels(int fl, int fr, int bl, int br) {
    state.wheelFL = fl; state.wheelFR = fr;
    state.wheelBL = bl; state.wheelBR = br;
    setWheel(FL_IN1, FL_IN2, fl);
    setWheel(FR_IN1, FR_IN2, fr);
    setWheel(BL_IN1, BL_IN2, bl);
    setWheel(BR_IN1, BR_IN2, br);
}

void stopAll() {
    setAllWheels(0, 0, 0, 0);
}

// ═══════════════════════════════════════════════════════════════
//  MECANUM КИНЕМАТИКА
//  Колёса расположены:   FL ╲  ╱ FR
//                        BL ╱  ╲ BR
//  vy = вперёд(+)/назад(-)
//  vx = вправо(+)/влево(-)
//  omega = по часовой(+)/против часовой(-)
// ═══════════════════════════════════════════════════════════════
void mecanumMove(int vy, int vx, int omega) {
    int fl = vy + vx + omega;
    int fr = vy - vx - omega;
    int bl = vy - vx + omega;
    int br = vy + vx - omega;

    // Нормализация: если максимум > 255, масштабируем все
    int maxVal = max(max(abs(fl), abs(fr)), max(abs(bl), abs(br)));
    if (maxVal > MAX_SPEED) {
        float scale = (float)MAX_SPEED / maxVal;
        fl *= scale; fr *= scale; bl *= scale; br *= scale;
    }

    setAllWheels(fl, fr, bl, br);
}

// Преобразование текстовой команды в mecanum вектора
void actionToMecanum(String action, int speed) {
    speed = constrain(speed, 0, MAX_SPEED);
    if (action == "forward")       mecanumMove(speed, 0, 0);
    else if (action == "backward") mecanumMove(-speed, 0, 0);
    else if (action == "left")     mecanumMove(0, -speed, 0);  // КРАБ влево
    else if (action == "right")    mecanumMove(0, speed, 0);   // КРАБ вправо
    else if (action == "rotate_left")  mecanumMove(0, 0, -speed);
    else if (action == "rotate_right") mecanumMove(0, 0, speed);
    else stopAll();
}


// ═══════════════════════════════════════════════════════════════
//  СЕНСОРЫ
// ═══════════════════════════════════════════════════════════════
float measureDistance(int echoPin) {
    digitalWrite(TRIG_PIN, LOW);
    delayMicroseconds(2);
    digitalWrite(TRIG_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(TRIG_PIN, LOW);
    long dur = pulseIn(echoPin, HIGH, 25000);  // таймаут 25мс ≈ 4.25м
    if (dur == 0) return 999;
    return dur * 0.034 / 2.0;
}

void readSensors() {
    state.distFront = measureDistance(ECHO_F_PIN);
    delay(15);
    state.distRear = measureDistance(ECHO_R_PIN);
    state.irLeft  = (digitalRead(IR_LEFT)  == LOW);
    state.irRight = (digitalRead(IR_RIGHT) == LOW);

    // Батарея: делитель 4.7kΩ + 10kΩ = коэфф ≈ 3.128
    // 3S Li-ion: 9.0V (разряжен) — 12.6V (полный)
    int rawBat = analogRead(BAT_ADC);
    state.batteryV = (rawBat / 4095.0) * 3.3 * 3.128;
    state.batteryPct = constrain(map((int)(state.batteryV * 100),
                                     900, 1260, 0, 100), 0, 100);

    // Док: напряжение на контактах зарядки
    int rawDock = analogRead(DOCK_ADC);
    state.dockContact = (rawDock > DOCK_VOLTAGE_THRESHOLD);

    // HMC5883L компас
    readCompass();
}


// ═══════════════════════════════════════════════════════════════
//  ЭКСТРЕННАЯ БЕЗОПАСНОСТЬ
// ═══════════════════════════════════════════════════════════════
void safetyCheck() {
    // Экстренная остановка при столкновении спереди
    if (state.distFront < EMERGENCY_CM) {
        // Если едем вперёд — стоп
        if (state.wheelFL > 0 || state.wheelFR > 0) {
            stopAll();
        }
    }
    // Экстренная остановка при столкновении сзади
    if (state.distRear < EMERGENCY_CM) {
        if (state.wheelFL < 0 || state.wheelFR < 0) {
            stopAll();
        }
    }
    // ИК препятствия — мягкая коррекция
    if (state.irLeft && (state.wheelFL > 0 || state.wheelBL > 0)) {
        // Объект слева — подруливаем вправо
        setWheel(FL_IN1, FL_IN2, state.wheelFL / 2);
        setWheel(BL_IN1, BL_IN2, state.wheelBL / 2);
    }
    if (state.irRight && (state.wheelFR > 0 || state.wheelBR > 0)) {
        setWheel(FR_IN1, FR_IN2, state.wheelFR / 2);
        setWheel(BR_IN1, BR_IN2, state.wheelBR / 2);
    }
}


// ═══════════════════════════════════════════════════════════════
//  АВТОСТЫКОВКА С ЗАРЯДНОЙ СТАНЦИЕЙ
//  Фазы: 1=разворот 180°, 2=едем назад, 3=ищем контакт, 4=зарядка
// ═══════════════════════════════════════════════════════════════
void updateDock() {
    if (!state.dockRequested && state.dockPhase == 0) return;

    unsigned long now = millis();

    switch (state.dockPhase) {
        case 0:
            // Старт — начинаем разворот
            state.dockPhase = 1;
            state.dockTimer = now;
            Serial2.println("D:searching");
            // Разворот на 180° (≈1.5 сек при speed=150)
            mecanumMove(0, 0, 150);
            break;

        case 1:
            // Разворот — ждём 1.5 сек
            if (now - state.dockTimer > 1500) {
                stopAll();
                delay(200);
                state.dockPhase = 2;
                state.dockTimer = now;
                Serial2.println("D:approaching");
            }
            break;

        case 2:
            // Едем задом к стене / доку
            if (state.dockContact) {
                // Контакт! Стыковка успешна
                stopAll();
                state.dockPhase = 4;
                Serial2.println("D:docked");
                break;
            }
            if (state.distRear < 3) {
                // Упёрлись в стену но нет контакта
                stopAll();
                state.dockPhase = 0;
                state.dockRequested = false;
                Serial2.println("D:failed");
                break;
            }
            // Замедляемся ближе к стене
            {
                int backSpeed = (state.distRear < 15) ? 80 : 120;
                mecanumMove(-backSpeed, 0, 0);
            }
            // Таймаут 15 сек
            if (now - state.dockTimer > 15000) {
                stopAll();
                state.dockPhase = 0;
                state.dockRequested = false;
                Serial2.println("D:failed");
            }
            break;

        case 4:
            // Заряжается — стоим на месте
            if (!state.dockContact) {
                // Потеряли контакт
                state.dockPhase = 0;
                state.dockRequested = false;
                Serial2.println("D:undocked");
            }
            // Батарея полная — можно отъезжать
            if (state.batteryPct >= 95) {
                mecanumMove(100, 0, 0);  // Отъезжаем вперёд
                delay(1000);
                stopAll();
                state.dockPhase = 0;
                state.dockRequested = false;
                Serial2.println("D:undocked");
            }
            break;
    }
}


// ═══════════════════════════════════════════════════════════════
//  UART ПРОТОКОЛ — разбор команд от ESP32-CAM
// ═══════════════════════════════════════════════════════════════
void processCommand(char* cmd) {
    if (cmd[0] == 'M' && cmd[1] == ':') {
        // M:vy,vx,omega — mecanum движение
        int vy = 0, vx = 0, omega = 0;
        sscanf(cmd + 2, "%d,%d,%d", &vy, &vx, &omega);
        if (state.dockPhase >= 1 && state.dockPhase <= 3) return;  // не прерывать стыковку
        mecanumMove(vy, vx, omega);
        Serial2.println("K");

    } else if (cmd[0] == 'W' && cmd[1] == ':') {
        // W:fl,fr,bl,br — прямое управление колёсами
        int fl = 0, fr = 0, bl = 0, br = 0;
        sscanf(cmd + 2, "%d,%d,%d,%d", &fl, &fr, &bl, &br);
        if (state.dockPhase >= 1 && state.dockPhase <= 3) return;
        setAllWheels(fl, fr, bl, br);
        Serial2.println("K");

    } else if (cmd[0] == 'A' && cmd[1] == ':') {
        // A:action,speed — текстовая команда (обратная совместимость)
        char action[20] = "";
        int speed = 0;
        sscanf(cmd + 2, "%[^,],%d", action, &speed);
        if (state.dockPhase >= 1 && state.dockPhase <= 3) return;
        actionToMecanum(String(action), speed);
        Serial2.println("K");

    } else if (cmd[0] == 'S' && cmd[1] == ':') {
        // S:panAngle — серво pan (лево-право)
        int angle = atoi(cmd + 2);
        angle = constrain(angle, 0, 180);
        if (angle != state.panAngle) {
            panServo.write(angle);
            state.panAngle = angle;
        }
        Serial2.println("K");

    } else if (cmd[0] == 'T' && cmd[1] == ':') {
        // T:tiltAngle — серво tilt (вверх-вниз)
        int angle = atoi(cmd + 2);
        angle = constrain(angle, 0, 180);
        if (angle != state.tiltAngle) {
            tiltServo.write(angle);
            state.tiltAngle = angle;
        }
        Serial2.println("K");

    } else if (cmd[0] == 'D' && cmd[1] == ':') {
        // D:1 = начать стыковку, D:0 = отмена
        if (cmd[2] == '1') {
            state.dockRequested = true;
            state.dockPhase = 0;
            updateDock();  // запуск
        } else {
            state.dockRequested = false;
            state.dockPhase = 0;
            stopAll();
            Serial2.println("D:undocked");
        }

    } else if (cmd[0] == '?') {
        // Запрос сенсоров
        sendSensorReport();

    } else if (cmd[0] == 'X') {
        // Экстренная остановка
        stopAll();
        state.dockRequested = false;
        state.dockPhase = 0;
        Serial2.println("K");
    }
}

void readUART() {
    while (Serial2.available()) {
        char c = Serial2.read();
        if (c == '\n' || c == '\r') {
            if (uartIdx > 0) {
                uartBuf[uartIdx] = '\0';
                processCommand(uartBuf);
                uartIdx = 0;
            }
        } else if (uartIdx < (int)sizeof(uartBuf) - 1) {
            uartBuf[uartIdx++] = c;
        }
    }
}

void sendSensorReport() {
    char buf[128];
    snprintf(buf, sizeof(buf), "S:%.0f,%.0f,%d,%d,%d,%d,%.1f,%d,%d,%d",
             state.distFront, state.distRear,
             state.irLeft ? 1 : 0, state.irRight ? 1 : 0,
             state.batteryPct,
             state.dockContact ? 1 : 0,
             state.heading,
             state.magX, state.magY, state.magZ);
    Serial2.println(buf);
}


// ═══════════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Serial.println("\n╔═══════════════════════════════════╗");
    Serial.println("║  КЕША DRIVE v5.0 — 4WD Mecanum    ║");
    Serial.println("║  ESP32-WROOM-32 Motor Controller   ║");
    Serial.println("╚═══════════════════════════════════╝\n");

    // Моторы
    pinMode(FL_IN1, OUTPUT); pinMode(FL_IN2, OUTPUT);
    pinMode(FR_IN1, OUTPUT); pinMode(FR_IN2, OUTPUT);
    pinMode(BL_IN1, OUTPUT); pinMode(BL_IN2, OUTPUT);
    pinMode(BR_IN1, OUTPUT); pinMode(BR_IN2, OUTPUT);
    stopAll();

    // Сенсоры
    pinMode(TRIG_PIN, OUTPUT);
    pinMode(ECHO_F_PIN, INPUT);
    pinMode(ECHO_R_PIN, INPUT);
    pinMode(IR_LEFT, INPUT);
    pinMode(IR_RIGHT, INPUT);

    // Серво pan-tilt
    panServo.attach(SERVO_PAN_PIN);
    panServo.write(90);
    tiltServo.attach(SERVO_TILT_PIN);
    tiltServo.write(90);
    Serial.println("[SERVO] Pan(GPIO17) + Tilt(GPIO5) ready");

    // UART к ESP32-CAM
    Serial2.begin(UART_BAUD, SERIAL_8N1, CAM_RX, CAM_TX);
    Serial.println("[UART] Ready on GPIO4/GPIO16");

    // HMC5883L компас
    state.compassOK = hmc5883l_init();
    if (state.compassOK) {
        readCompass();
        Serial.printf("[COMPASS] HMC5883L OK — heading=%.1f°\n", state.heading);
    } else {
        Serial.println("[COMPASS] HMC5883L not found on I2C!");
    }

    // Тест моторов — кратко
    Serial.println("[MOTORS] Self-test...");
    setAllWheels(80, 80, 80, 80);
    delay(200);
    setAllWheels(-80, -80, -80, -80);
    delay(200);
    stopAll();
    Serial.println("[MOTORS] OK — 4 wheels active");

    // Начальное чтение сенсоров
    readSensors();
    Serial.printf("[SENSORS] F=%.0fcm R=%.0fcm Bat=%.1fV(%d%%) Dock=%s\n",
                  state.distFront, state.distRear,
                  state.batteryV, state.batteryPct,
                  state.dockContact ? "YES" : "NO");

    Serial.println("[DRIVE v5.0] Ready!\n");
}


// ═══════════════════════════════════════════════════════════════
//  MAIN LOOP — 20 Гц сенсоры, 10 Гц отчёт, UART непрерывно
// ═══════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // 1. UART команды (каждый цикл)
    readUART();

    // 2. Сенсоры (20 Гц)
    if (now - state.lastSensor >= SENSOR_INTERVAL) {
        readSensors();
        safetyCheck();
        state.lastSensor = now;
    }

    // 3. Автостыковка
    updateDock();

    // 4. Отчёт сенсоров (10 Гц)
    if (now - state.lastReport >= REPORT_INTERVAL) {
        sendSensorReport();
        state.lastReport = now;
    }

    delay(5);
}
