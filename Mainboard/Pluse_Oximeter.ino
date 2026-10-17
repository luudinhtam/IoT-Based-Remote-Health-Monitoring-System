#include <Wire.h>
#include <LiquidCrystal_I2C.h>  // Thư viện I2C LCD
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ==================================================
// 1. CAU HINH LCD 16x2 I2C
// ==================================================
LiquidCrystal_I2C lcd(0x21, 16, 2); // Kiểm tra đúng địa chỉ I2C (0x27, 0x3F hoặc 0x25)

// ==================================================
// 2. CAU HINH CHAN LED VA BUZZER
// ==================================================
const int LED_PIN = 8;     // Đèn LED Digital 8
const int BUZZER_PIN = 9;  // Còi Buzzer Digital 9

const int BPM_LOW = 60;
const int BPM_HIGH = 100;
const int SPO2_LOW = 95;

unsigned long lastBlinkTime = 0;
const unsigned long BLINK_INTERVAL = 200;
bool alertState = false;

// ==================================================
// 3. CAU HINH CAM BIEN MAX30102
// ==================================================
MAX30105 particleSensor;

#define BUFFER_SIZE 100
#define NEW_SAMPLES 25
#define FINGER_THRESHOLD 20000UL

uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];

int32_t spo2 = 0;
int8_t spo2Valid = 0;

int32_t heartRate = 0;
int8_t heartRateValid = 0;

uint32_t currentIR = 0;
uint32_t currentRed = 0;

bool sensorReady = false;
bool hasFinger = false;

unsigned long lastDisplayTime = 0;
const unsigned long DISPLAY_INTERVAL = 500; // Cập nhật mỗi 500ms

// ==================================================
// 4. HIEN THI LCD
// ==================================================
void showStartup() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("IOT PULSE OXIM.");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
}

void showSensorError() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MAX30102 ERROR");
  lcd.setCursor(0, 1);
  lcd.print("Check wiring");
}

void showNoFinger() {
  lcd.setCursor(0, 0);
  lcd.print("Place your      ");
  lcd.setCursor(0, 1);
  lcd.print("finger on sensor");
}

void showResults() {
  lcd.setCursor(0, 0);
  lcd.print("BPM:");

  if (heartRateValid && heartRate >= 40 && heartRate <= 200) {
    lcd.print(heartRate);
    lcd.print("    ");
  } else {
    lcd.print("--  ");
  }

  lcd.setCursor(0, 1);
  lcd.print("SpO2:");

  if (spo2Valid && spo2 >= 70 && spo2 <= 100) {
    lcd.print(spo2);
    lcd.print("%   ");
  } else {
    lcd.print("--% ");
  }
}

// ==================================================
// 5. GUI JSON QUA SERIAL1 (CHO NODEMCU / ESP8266)
// ==================================================
void sendJSON() {
  // Tạo chuỗi JSON đơn giản và sạch sẽ gửi qua Serial1 (TX1 - Pin 18)
  Serial1.print("{\"bpm\":");
  if (hasFinger && heartRateValid && heartRate >= 40 && heartRate <= 200) {
    Serial1.print(heartRate);
  } else {
    Serial1.print("null");
  }

  Serial1.print(",\"spo2\":");
  if (hasFinger && spo2Valid && spo2 >= 70 && spo2 <= 100) {
    Serial1.print(spo2);
  } else {
    Serial1.print("null");
  }

  Serial1.print(",\"finger\":");
  if (hasFinger) {
    Serial1.print("true");
  } else {
    Serial1.print("false");
  }

  Serial1.println("}"); // Đóng ngoặc JSON và xuống dòng '\n'

  // Debug ra Serial Monitor USB của máy tính
  Serial.print("Finger: ");
  Serial.print(hasFinger ? "YES" : "NO");
  Serial.print(" | BPM: ");
  if (heartRateValid) Serial.print(heartRate); else Serial.print("--");
  Serial.print(" | SpO2: ");
  if (spo2Valid) Serial.print(spo2); else Serial.print("--");
  Serial.println("%");
}

// ==================================================
// 6. DOC MOT MAU TU MAX30102
// ==================================================
bool readOneSample(uint32_t &irValue, uint32_t &redValue) {
  unsigned long startTime = millis();

  while (millis() - startTime < 500) { // Đã fix lỗi dấu trừ '-'
    particleSensor.check();

    if (particleSensor.available()) {
      redValue = particleSensor.getRed();
      irValue = particleSensor.getIR();

      particleSensor.nextSample();

      currentIR = irValue;
      currentRed = redValue;

      return true;
    }
    delay(1);
  }

  return false;
}

// ==================================================
// 7. THU THAP MAU
// ==================================================
bool collectInitialSamples() {
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (!readOneSample(irBuffer[i], redBuffer[i])) {
      return false;
    }
  }
  return true;
}

bool collectNewSamples() {
  for (int i = 0; i < BUFFER_SIZE - NEW_SAMPLES; i++) {
    irBuffer[i] = irBuffer[i + NEW_SAMPLES];
    redBuffer[i] = redBuffer[i + NEW_SAMPLES];
  }

  for (int i = BUFFER_SIZE - NEW_SAMPLES; i < BUFFER_SIZE; i++) {
    if (!readOneSample(irBuffer[i], redBuffer[i])) {
      return false;
    }
  }
  return true;
}

// ==================================================
// 8. TINH NHIP TIM VA SPO2
// ==================================================
void calculateVitals() {
  hasFinger = (currentIR > FINGER_THRESHOLD);

  if (!hasFinger) {
    heartRateValid = 0;
    spo2Valid = 0;
    return;
  }

  maxim_heart_rate_and_oxygen_saturation(
    irBuffer,
    BUFFER_SIZE,
    redBuffer,
    &spo2,
    &spo2Valid,
    &heartRate,
    &heartRateValid
  );

  if (heartRate < 40 || heartRate > 200) {
    heartRateValid = 0;
  }

  if (spo2 < 70 || spo2 > 100) {
    spo2Valid = 0;
  }
}

// ==================================================
// 9. PHAT CANH BAO (COI + LED)
// ==================================================
void checkAlerts() {
  if (!hasFinger) {
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    alertState = false;
    return;
  }

  bool alertNeeded = false;
  if (heartRateValid && (heartRate < BPM_LOW || heartRate > BPM_HIGH)) {
    alertNeeded = true;
  }
  if (spo2Valid && (spo2 < SPO2_LOW)) {
    alertNeeded = true;
  }

  if (alertNeeded) {
    unsigned long currentMillis = millis();
    if (currentMillis - lastBlinkTime >= BLINK_INTERVAL) {
      lastBlinkTime = currentMillis;
      alertState = !alertState;

      if (alertState) {
        digitalWrite(LED_PIN, HIGH);
        tone(BUZZER_PIN, 1500);
      } else {
        digitalWrite(LED_PIN, LOW);
        noTone(BUZZER_PIN);
      }
    }
  } else {
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
    alertState = false;
  }
}

// ==================================================
// 10. SETUP
// ==================================================
void setup() {
  Serial.begin(115200);   // Debug Monitor USB
  Serial1.begin(115200);  // Truyền JSON sang NodeMCU v3 (TX1 - Pin 18)

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);

  Wire.begin();
  Wire.setClock(100000);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  showStartup();

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    showSensorError();
    Serial.println("ERROR: MAX30102 not found");
    sensorReady = false;
    return;
  }

  particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeGreen(0);

  sensorReady = true;

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("MAX30102 READY");
  lcd.setCursor(0, 1);
  lcd.print("Place finger");

  delay(1000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Reading sensor");

  if (!collectInitialSamples()) {
    showSensorError();
    Serial.println("ERROR: Initial sample timeout");
    sensorReady = false;
    return;
  }

  calculateVitals();
  lastDisplayTime = millis();
}

// ==================================================
// 11. LOOP
// ==================================================
void loop() {
  if (!sensorReady) {
    delay(1000);
    return;
  }

  if (!collectNewSamples()) {
    Serial.println("WARNING: Sensor read timeout");
    return;
  }

  calculateVitals();
  checkAlerts();

  if (millis() - lastDisplayTime >= DISPLAY_INTERVAL) { // Đã fix lỗi dấu trừ '-'
    lastDisplayTime = millis();

    if (hasFinger) {
      showResults();
    } else {
      showNoFinger();
    }

    sendJSON(); // Gửi dữ liệu JSON sang NodeMCU v3
  }
}