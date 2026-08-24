#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <DHT.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

#define DHTPIN 4
#define DHTTYPE DHT22

struct SharedData {
  float temp;
  float humidity;
  int headcount;
  bool doorOpen;
  unsigned long doorOpenSec;
  bool acStatus;
  int acTargetTemp;
  bool acHealthAlert;
} sharedData;

// Handle FreeRTOS
SemaphoreHandle_t dataMutex;

// Inisialisasi Objek Hardware
DHT dht(DHTPIN, DHTTYPE);

// Variabel Waktu Diagnostik
unsigned long acTurnOnTime = 0;
float tempWhenTurnedOn = 0.0;

// ================= TASK 2: READ SENSOR DHT 22 =================
void Task_DHT22_Read(void *pvParameters) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  for (;;) {
    float h = dht.readHumidity();
    float t = dht.readTemperature();

    if (!isnan(h) && !isnan(t)) {
      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sharedData.temp = t;
        sharedData.humidity = h;
        xSemaphoreGive(dataMutex);
      }
      Serial.printf("[DHT] Temp: %.2f, Humidity: %.2f\n", t, h);
    } else {
      Serial.println("[DHT] Gagal membaca sensor!");
    }
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(1000));
  }
}

// ================= TASK 2: HTTP TELEMETRY =================
void Task_HTTP_Telemetry(void *pvParameters) {
  for (;;) {
    SharedData localData;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localData = sharedData;
      xSemaphoreGive(dataMutex);
    }

    if (WiFi.status() == WL_CONNECTED) {
      HTTPClient http;

      http.begin(FLASK_URL);
      http.setTimeout(3000);

      
      http.addHeader("Content-Type", "application/json");
      http.addHeader("Bypass-Tunnel-Reminder", "true");
      http.addHeader("User-Agent", "ESP32-SmartOffice");

      String jsonPayload = "{";
      jsonPayload += "\"temp\":" + String(localData.temp, 1) + ",";
      jsonPayload += "\"humidity\":" + String(localData.humidity, 1) + ",";
      jsonPayload += "\"headcount\":" + String(localData.headcount) + ",";
      jsonPayload += "\"door_open\":" + String(localData.doorOpen ? "true" : "false") + ",";
      jsonPayload += "\"door_open_sec\":" + String(localData.doorOpenSec) + ",";
      jsonPayload += "\"ac_status\":" + String(localData.acStatus ? "true" : "false") + ",";
      jsonPayload += "\"ac_target_temp\":" + String(localData.acTargetTemp) + ",";
      jsonPayload += "\"ac_health_alert\":" + String(localData.acHealthAlert ? "true" : "false");
      jsonPayload += "}";

      int httpResponseCode = http.POST(jsonPayload);
      if (httpResponseCode > 0) {
        Serial.printf("[HTTP] Data terkirim ke Flask | Status Code: %d\n", httpResponseCode);
      } else {
        Serial.printf("[HTTP] Gagal mengirim data | Error: %s\n", http.errorToString(httpResponseCode).c_str());
      }
      http.end();
    } else {
      Serial.println("[HTTP] Wi-Fi terputus, mencoba rekoneksi...");
      WiFi.reconnect();
    }

    // Serial.println("[JSON PAYLOAD]");
    // Serial.print(jsonPayload);

    vTaskDelay(pdMS_TO_TICKS(2500)); // Kirim telemetri setiap 2.5 detik
  }
}


void setup() {
  Serial.begin(115200);
  dht.begin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Wi-Fi] Menghubungkan");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[Wi-Fi] Terhubung!");

  dataMutex = xSemaphoreCreateMutex();

  // Inisialisasi Data Global Awal
  sharedData.temp = 25.0;
  sharedData.humidity = 60.0;
  sharedData.headcount = 0;
  sharedData.doorOpen = false;
  sharedData.doorOpenSec = 0;
  sharedData.acStatus = false;
  sharedData.acTargetTemp = 24;
  sharedData.acHealthAlert = false;

  xTaskCreatePinnedToCore(Task_DHT22_Read,      "DHT_Task",     2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_HTTP_Telemetry, "HTTP_Task",     8192, NULL, 1, NULL, 1);

  Serial.println("Node 1 Running");
}

void loop() {
  // Aku nak sedikit aksi
}
