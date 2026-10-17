#define BLYNK_TEMPLATE_ID   "TMPL6rby1xvF4"
#define BLYNK_TEMPLATE_NAME "Health Monitoring"
#define BLYNK_AUTH_TOKEN    "1fK7fvu5XW0KGWHGbt_Qq4gVcbj5q9VR" // Thay Auth Token chính xác của bạn

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <ArduinoJson.h>

char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = "LaSiMi 99 Le Van Viet";  // Thay tên Wi-Fi
char pass[] = "lasimicamon";     // Thay mật khẩu Wi-Fi

void setup() {
  Serial.begin(115200); // Đọc JSON từ chân RX (TX1 Pin 18 Mega)
  delay(1000);

  // Kết nối Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 20) {
    delay(500);
    yield();
    count++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Blynk.config(auth);
    Blynk.connect(5000);
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
  }

  // ĐỌC DỮ LIỆU THỰC TẾ TỪ ARDUINO MEGA GỬI SANG
  if (Serial.available() > 0) {
    String jsonString = Serial.readStringUntil('\n');

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, jsonString);

    if (!error) {
      bool hasFinger = doc["finger"];

      if (hasFinger) {
        // Đọc giá trị thực tế từ chuỗi JSON
        if (!doc["bpm"].isNull()) {
          int realBpm = doc["bpm"];
          Blynk.virtualWrite(V0, realBpm);  // Đẩy BPM THỰC TẾ lên V0
        }
        if (!doc["spo2"].isNull()) {
          int realSpo2 = doc["spo2"];
          Blynk.virtualWrite(V1, realSpo2); // Đẩy SpO2 THỰC TẾ lên V1
        }
      } else {
        // Không có ngón tay -> Trả về 0
        Blynk.virtualWrite(V0, 0);
        Blynk.virtualWrite(V1, 0);
      }
    }
  }
}