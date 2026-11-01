#define BLYNK_TEMPLATE_ID "TMPL68xbXFpBY"
#define BLYNK_TEMPLATE_NAME "Health Monitoring"
#define BLYNK_AUTH_TOKEN "jLTGUEGT3ONiskg5kwqzPlSX4eLd9qI4"

#include <ESP8266WiFi.h>
#include <BlynkSimpleEsp8266.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h> 

char auth[] = BLYNK_AUTH_TOKEN;
char ssid[] = "TRAPH3CHAY";  
char pass[] = "TRAPH3CHAY";     

// --- CẤU HÌNH TELEGRAM BOT ---
const String BOT_TOKEN = "8899307874:AAGjB9jA3fKIZiZbfWkvYpabywVc6AlsOuY";
const String CHAT_ID   = "-5445817951";

WiFiClientSecure secureClient;

// --- CẤU HÌNH THỜI GIAN THỰC (NTP) ---
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 25200, 60000);

// Biến chống spam cảnh báo chỉ số bất thường (10 giây gửi 1 lần)
unsigned long lastAlertTime = 0;
const unsigned long alertInterval = 10000; 

// Hàm chuyển đổi nhóm tuổi hiển thị
String getAgeGroupName(int group) {
  if (group == 0) return "CHILD";
  if (group == 1) return "ADULT";
  if (group == 2) return "ELDERLY";
  return "ADULT";
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.print("Đang kết nối tới Wi-Fi: ");
  Serial.println(ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);

  int count = 0;
  while (WiFi.status() != WL_CONNECTED && count < 20) {
    delay(500);
    Serial.print(".");
    yield();
    count++;
  }

  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println(">>> ĐÃ KẾT NỐI WI-FI THÀNH CÔNG! <<<");
    Serial.print("Địa chỉ IP: ");
    Serial.println(WiFi.localIP());

    Blynk.config(auth);
    Blynk.connect(5000);

    timeClient.begin();
    configTime(25200, 0, "pool.ntp.org", "time.nist.gov");
  } else {
    Serial.println(">>> KẾT NỐI WI-FI THẤT BẠI! <<<");
  }
}

// Hàm gửi tin nhắn qua Telegram
void sendTelegramAlert(String message) {
  secureClient.setInsecure(); 
  Serial.println("Đang gửi cảnh báo qua Telegram...");
  
  if (secureClient.connect("api.telegram.org", 443)) {
    message.replace(" ", "%20");
    message.replace("\n", "%0A");
    
    String url = "/bot" + BOT_TOKEN + "/sendMessage?chat_id=" + CHAT_ID + "&text=" + message;
    
    secureClient.print(String("GET ") + url + " HTTP/1.1\r\n" +
                       "Host: api.telegram.org\r\n" +
                       "Connection: close\r\n\r\n");
    delay(10);
    secureClient.stop();
    Serial.println(">>> ĐÃ GỬI TELEGRAM THÀNH CÔNG! <<<");
  } else {
    Serial.println(">>> KẾT NỐI TELEGRAM THẤT BẠI! <<<");
  }
}

void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    Blynk.run();
    timeClient.update(); 
  }

  // Nhận JSON từ Arduino Mega qua Serial (UART)
  if (Serial.available() > 0) {
    String jsonString = Serial.readStringUntil('\n');

    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, jsonString);

    if (!error) {
      // --- 1. XỬ LÝ SỰ KIỆN NÚT BẤM SOS KHẨN CẤP ---
      bool sosTriggered = doc["sos"] | false;
      if (sosTriggered) {
        time_t now = time(nullptr);
        struct tm* ptm = localtime(&now);
        char timeBuffer[64];
        sprintf(timeBuffer, "%02d:%02d:%02d - %02d/%02d/%04d", 
                ptm->tm_hour, ptm->tm_min, ptm->tm_sec, 
                ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900);
        
        String sosMsg = "🚨 EMERGENCY: SOS BUTTON PRESSED! 🚨\n\n"
                "⚠️ Immediate medical assistance required!\n"
                "⏱ Time: " + String(timeBuffer);
        
        sendTelegramAlert(sosMsg);
        return; 
      }

      // --- 2. XỬ LÝ DỮ LIỆU SỨC KHỎE BÌNH THƯỜNG / BẤT THƯỜNG ---
      bool hasFinger = doc["finger"] | false;

      if (hasFinger) {
        int avgBpm = doc["avgBpm"] | 0;
        int avgSpo2 = doc["avgSpo2"] | 0;
        int ageGroupCode = doc["ageGroup"] | 1;
        String modeStr = getAgeGroupName(ageGroupCode);

        // Đẩy lên Blynk (V0: Nhịp tim, V1: SpO2)
        Blynk.virtualWrite(V0, avgBpm); 
        Blynk.virtualWrite(V1, avgSpo2); 

        // Kiểm tra ngưỡng cảnh báo y tế tự động (Chỉ dựa trên Nhịp tim và SpO2)
        bool isAbnormal = (avgBpm > 100 || (avgBpm > 0 && avgBpm < 60) || 
                           (avgSpo2 > 0 && avgSpo2 < 95));

        if (isAbnormal) {
          if (millis() - lastAlertTime > alertInterval) {
            time_t now = time(nullptr);
            struct tm* ptm = localtime(&now);
            
            char timeBuffer[64];
            sprintf(timeBuffer, "%02d:%02d:%02d - %02d/%02d/%04d", 
                    ptm->tm_hour, ptm->tm_min, ptm->tm_sec, 
                    ptm->tm_mday, ptm->tm_mon + 1, ptm->tm_year + 1900);
            
            String alertMsg = "🚨 ABNORMAL HEALTH ALERT! 🚨\n\n"
                  "👤 Age Group: " + modeStr + "\n"
                  "❤️ Heart Rate: " + String(avgBpm) + " BPM\n"
                  "🩸 SpO2: " + String(avgSpo2) + "%\n\n"
                  "⏱ Time: " + String(timeBuffer);
            
            sendTelegramAlert(alertMsg);
            lastAlertTime = millis(); 
          }
        }

      } else {
        Blynk.virtualWrite(V0, 0);
        Blynk.virtualWrite(V1, 0);
      }
    }
  }
}