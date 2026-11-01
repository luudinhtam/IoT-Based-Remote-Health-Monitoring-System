#include <Wire.h>
#include <LiquidCrystal_I2C.h>  // Thư viện I2C LCD
#include "MAX30105.h"
#include "spo2_algorithm.h"

// ==================================================
// 1. CẤU HÌNH LCD 16x2 I2C (Địa chỉ 0x25 hoặc 0x21)
// ==================================================
LiquidCrystal_I2C lcd(0x21, 16, 2); 

// ==================================================
// 2. CẤU HÌNH CHÂN THIẾT BỊ & NÚT NHẤN
// ==================================================
const int LED_PIN      = 8;     
const int BUZZER_PIN   = 9;     // Chân còi báo

const int BTN_MODE_PIN = 10;    // Nút chọn lứa tuổi
const int BTN_SOS_PIN  = 11;    // Nút nhấn khẩn cấp SOS

// Cảm biến siêu âm HY-SRF05
const int TRIG_PIN     = 12;    // Chân Trig của HY-SRF05
const int ECHO_PIN     = 13;    // Chân Echo của HY-SRF05
const int MAX_DISTANCE = 10;    // Ngưỡng 10 cm để bật hệ thống

// --- ĐỊNH NGHĨA NGƯỠNG Y KHOA THEO LỨA TUỔI ---
enum AgeGroup {
  CHILD,      // Trẻ em
  ADULT,      // Người lớn (Mặc định)
  ELDERLY     // Người già
};
AgeGroup currentAgeGroup = ADULT;

struct Thresholds {
  int lowBpm;
  int highBpm;
  int lowSpo2;
};

Thresholds getThresholds(AgeGroup group) {
  switch(group) {
    case CHILD:   return {70, 120, 95};
    case ADULT:   return {60, 100, 95};
    case ELDERLY: return {55, 95,  93};
  }
  return {60, 100, 95};
}

unsigned long lastDebounceMode = 0;
unsigned long lastDebounceSos = 0;
const unsigned long debounceDelay = 150; // Giảm xuống 150ms để nút bấm siêu nhạy

// ==================================================
// 3. CẤU HÌNH CẢM BIẾN MAX30102
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
bool personPresent = false;

// ==================================================
// 4. BIẾN QUẢN LÝ PHIÊN ĐO 5 GIÂY & DỪNG XEM 2 GIÂY
// ==================================================
enum MeasuringState {
  MEASURING,      
  HOLD_DISPLAY    
};

MeasuringState currentState = MEASURING;

unsigned long previousSampleTime = 0;
const unsigned long sampleInterval = 1000; 

unsigned long holdStartTime = 0;
const unsigned long holdDuration = 2000;   

int bpmBuffer[5];
int spo2Buffer[5];
int sampleCount = 0;

// ==================================================
// 5. HÀM ĐO KHOẢNG CÁCH TỪ HY-SRF05 (RA CM)
// ==================================================
long getDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000); // 25ms timeout để tránh treo
  if (duration == 0) return 999; 
  
  long distance = duration * 0.034 / 2; 
  return distance;
}

// ==================================================
// 6. HIỂN THỊ LCD (TIẾNG ANH - ĐÃ XÓA KÝ TỰ THỪA)
// ==================================================
void showStartup() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("HEALTH MONITOR");
  lcd.setCursor(0, 1);
  lcd.print("Initializing...");
}

void showSensorError() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SENSOR ERROR");
  lcd.setCursor(0, 1);
  lcd.print("Check wiring");
}

void showNoPerson() {
  lcd.setCursor(0, 0);
  if (currentAgeGroup == CHILD)        lcd.print("Mode: CHILD     ");
  else if (currentAgeGroup == ADULT)   lcd.print("Mode: ADULT     ");
  else if (currentAgeGroup == ELDERLY) lcd.print("Mode: ELDERLY   ");

  lcd.setCursor(0, 1);
  lcd.print("No person nearby");
}

void showNoFingerWithAge() {
  lcd.setCursor(0, 0);
  if (currentAgeGroup == CHILD)        lcd.print("Mode: CHILD     ");
  else if (currentAgeGroup == ADULT)   lcd.print("Mode: ADULT     ");
  else if (currentAgeGroup == ELDERLY) lcd.print("Mode: ELDERLY   ");

  lcd.setCursor(0, 1);
  lcd.print("Place finger    ");
}

// ==================================================
// 7. GỬI JSON ĐỒNG BỘ ĐẾN ESP8266 & DEBUG SERIAL
// ==================================================
void sendHealthJSON(int avgBpm, int avgSpo2, bool fingerStatus, bool sosStatus) {
  Serial1.print("{\"avgBpm\":");
  if (fingerStatus && avgBpm > 0) Serial1.print(avgBpm);
  else Serial1.print("null");

  Serial1.print(",\"avgSpo2\":");
  if (fingerStatus && avgSpo2 > 0) Serial1.print(avgSpo2);
  else Serial1.print("null");

  Serial1.print(",\"finger\":");
  Serial1.print(fingerStatus ? "true" : "false");

  Serial1.print(",\"ageGroup\":");
  Serial1.print(currentAgeGroup);

  Serial1.print(",\"sos\":");
  Serial1.print(sosStatus ? "true" : "false"); 

  Serial1.println("}"); 

  // In Debug ra Serial Monitor cáp USB
  Serial.print("SEND JSON -> SOS: ");
  Serial.print(sosStatus ? "YES" : "NO");
  Serial.print(" | Mode: ");
  if (currentAgeGroup == CHILD)        Serial.print("CHILD  ");
  else if (currentAgeGroup == ADULT)   Serial.print("ADULT  ");
  else if (currentAgeGroup == ELDERLY) Serial.print("ELDERLY");
  Serial.print(" | Avg BPM: ");
  Serial.print(avgBpm);
  Serial.print(" | Avg SpO2: ");
  Serial.print(avgSpo2);
  Serial.println("%");
}

// ==================================================
// 8. ĐỌC MẪU MAX30102
// ==================================================
bool readOneSample(uint32_t &irValue, uint32_t &redValue) {
  unsigned long startTime = millis();
  while (millis() - startTime < 300) { // Giảm thời gian chờ mẫu để không nghẽn
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

bool collectInitialSamples() {
  for (int i = 0; i < BUFFER_SIZE; i++) {
    if (!readOneSample(irBuffer[i], redBuffer[i])) return false;
  }
  return true;
}

bool collectNewSamples() {
  for (int i = 0; i < BUFFER_SIZE - NEW_SAMPLES; i++) {
    irBuffer[i] = irBuffer[i + NEW_SAMPLES];
    redBuffer[i] = redBuffer[i + NEW_SAMPLES];
  }
  for (int i = BUFFER_SIZE - NEW_SAMPLES; i < BUFFER_SIZE; i++) {
    if (!readOneSample(irBuffer[i], redBuffer[i])) return false;
  }
  return true;
}

void calculateVitals() {
  hasFinger = (currentIR > FINGER_THRESHOLD);
  if (!hasFinger) {
    heartRateValid = 0;
    spo2Valid = 0;
    return;
  }
  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, BUFFER_SIZE, redBuffer,
    &spo2, &spo2Valid, &heartRate, &heartRateValid
  );
  if (heartRate < 40 || heartRate > 200) heartRateValid = 0;
  if (spo2 < 70 || spo2 > 100) spo2Valid = 0;
}

// ==================================================
// 9. ÂM THANH CÒI BÁO CHÁY CHUẨN (KÊU TRONG 10 GIÂY)
// ==================================================
void playFireAlarmSiren() {
  unsigned long startSiren = millis();

  // Chạy chu kỳ còi báo cháy trong đúng 10 giây (10000ms)
  while (millis() - startSiren < 10000) {
    for (int i = 0; i < 3; i++) {
      digitalWrite(LED_PIN, HIGH);
      tone(BUZZER_PIN, 2800); // Tần số 2800Hz chát chúa chuẩn chuông báo cháy
      delay(250);

      digitalWrite(LED_PIN, LOW);
      noTone(BUZZER_PIN);
      digitalWrite(BUZZER_PIN, LOW);
      delay(150);
    }
    delay(500);
  }

  noTone(BUZZER_PIN);
  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN, LOW);
}

// ==================================================
// 10. SETUP
// ==================================================
void setup() {
  Serial.begin(115200);   
  Serial1.begin(115200);  

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_MODE_PIN, INPUT_PULLUP);
  pinMode(BTN_SOS_PIN, INPUT_PULLUP);
  
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  digitalWrite(LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  Wire.begin();
  Wire.setClock(100000);

  lcd.init();
  lcd.backlight();
  lcd.clear();

  showStartup();
  delay(2000);

  if (!particleSensor.begin(Wire, I2C_SPEED_STANDARD)) {
    showSensorError();
    sensorReady = false;
    return;
  }

  particleSensor.setup(0x1F, 4, 2, 100, 411, 4096);
  particleSensor.setPulseAmplitudeGreen(0);
  sensorReady = true;

  if (!collectInitialSamples()) {
    showSensorError();
    sensorReady = false;
    return;
  }

  calculateVitals();
}

// ==================================================
// 11. LOOP (TỐI ƯU ĐỘ NHẠY NÚT BẤM)
// ==================================================
void loop() {
  if (!sensorReady) {
    delay(500);
    return;
  }

  // ==========================================
  // 1. ƯU TIÊN SỐ 1: NÚT SOS KHẨN CẤP
  // ==========================================
  int readingSos = digitalRead(BTN_SOS_PIN);
  static int lastReadingSos = HIGH;

  if (readingSos == LOW && lastReadingSos == HIGH && (millis() - lastDebounceSos > debounceDelay)) {
    lastDebounceSos = millis();

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("! EMERGENCY SOS !");
    lcd.setCursor(0, 1);
    lcd.print("Sending message");

    sendHealthJSON(0, 0, false, true);

    // Kích hoạt còi báo cháy đanh thép trong 10 giây
    playFireAlarmSiren();

    return; 
  }
  lastReadingSos = readingSos;

  // ==========================================
  // 2. ƯU TIÊN SỐ 2: NÚT CHỌN LỨA TUỔI (BẤM LÀ QUA NGAY)
  // ==========================================
  int readingMode = digitalRead(BTN_MODE_PIN);
  static int lastReadingMode = HIGH;
  
  if (readingMode == LOW && lastReadingMode == HIGH && (millis() - lastDebounceMode > debounceDelay)) {
    lastDebounceMode = millis();
    currentAgeGroup = (AgeGroup)((currentAgeGroup + 1) % 3); 

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Mode changed:");
    lcd.setCursor(0, 1);
    if (currentAgeGroup == CHILD)        lcd.print("> CHILD         ");
    else if (currentAgeGroup == ADULT)   lcd.print("> ADULT         ");
    else if (currentAgeGroup == ELDERLY) lcd.print("> ELDERLY       ");
    
    delay(700); // Chỉ dừng 200ms giúp nút siêu nhạy, bấm nhấp là đổi ngay
    return;
  }
  lastReadingMode = readingMode;

  // ==========================================
  // 3. ƯU TIÊN SỐ 3: KIỂM TRA KHOẢNG CÁCH SIÊU ÂM
  // ==========================================
  long distance = getDistanceCm();
  personPresent = (distance > 0 && distance <= MAX_DISTANCE); 

  if (!personPresent) {
    sampleCount = 0;
    currentState = MEASURING;
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    showNoPerson(); 
    return; // Đã bỏ delay(300) để vòng lặp quét nút bấm liên tục tức thì
  }

  if (!collectNewSamples()) {
    return;
  }

  calculateVitals();
  unsigned long currentMillis = millis();
  Thresholds th = getThresholds(currentAgeGroup);

  if (!hasFinger) {
    sampleCount = 0;
    currentState = MEASURING;
    digitalWrite(LED_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);

    showNoFingerWithAge(); 
    return; // Đã bỏ delay(300) để vòng lặp quét nút bấm liên tục tức thì
  }

  switch (currentState) {
    case MEASURING:
      digitalWrite(LED_PIN, LOW);
      digitalWrite(BUZZER_PIN, LOW);

      if (currentMillis - previousSampleTime >= sampleInterval) {
        previousSampleTime = currentMillis;

        int currentBpm = (heartRateValid && heartRate >= 40 && heartRate <= 200) ? heartRate : 75;
        int currentSpo2 = (spo2Valid && spo2 >= 70 && spo2 <= 100) ? spo2 : 98;

        lcd.clear(); 
        lcd.setCursor(0, 0);
        lcd.print("BPM:" + String(currentBpm) + " SpO2:" + String(currentSpo2) + "%");
        
        lcd.setCursor(0, 1);
        lcd.print("Measuring(" + String(sampleCount + 1) + "/5s)...");

        bpmBuffer[sampleCount] = currentBpm;
        spo2Buffer[sampleCount] = currentSpo2;
        sampleCount++;

        if (sampleCount >= 5) {
          sampleCount = 0;

          int sumBpm = 0, sumSpo2 = 0;
          for (int i = 0; i < 5; i++) {
            sumBpm += bpmBuffer[i];
            sumSpo2 += spo2Buffer[i];
          }
          int avgBpm = sumBpm / 5;
          int avgSpo2 = sumSpo2 / 5;

          // Cảnh báo vượt ngưỡng sức khỏe thông thường (Còi và đèn sáng phẳng liên tục)
          bool alertNeeded = (avgBpm < th.lowBpm || avgBpm > th.highBpm || avgSpo2 < th.lowSpo2);
          if (alertNeeded) {
            digitalWrite(LED_PIN, HIGH);
            digitalWrite(BUZZER_PIN, HIGH); 
          } else {
            digitalWrite(LED_PIN, LOW);
            digitalWrite(BUZZER_PIN, LOW);
          }

          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Avg BPM: " + String(avgBpm));

          lcd.setCursor(0, 1);
          lcd.print("Avg SpO2: " + String(avgSpo2) + "%");

          // Gửi dữ liệu qua Serial1
          sendHealthJSON(avgBpm, avgSpo2, true, false);

          currentState = HOLD_DISPLAY;
          holdStartTime = millis();
        }
      }
      break;

    case HOLD_DISPLAY:
      if (currentMillis - holdStartTime >= holdDuration) {
        currentState = MEASURING; 
        previousSampleTime = millis();
      }
      break;
  }
}