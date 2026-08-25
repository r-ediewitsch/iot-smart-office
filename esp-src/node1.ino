#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <DHT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"

#define DHTPIN 4
#define DHTTYPE DHT22

const unsigned long DOOR_TIMEOUT_SEC   = 180;
const unsigned long AC_HEALTH_CHECK_MS = 1800000; // 30 menit

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

// Inisialisasi Objek Hardware & Klien Jaringan
DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Variabel Waktu Diagnostik
unsigned long acTurnOnTime = 0;
float tempWhenTurnedOn = 0.0;

// ================= TASK 1: READ SENSOR DHT 22 =================
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

// ================= TASK 2: MQTT TELEMETRY =================
void Task_MQTT_Telemetry(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      // Reconnect MQTT jika terputus
      if (!mqttClient.connected()) {
        Serial.print("[MQTT] Menghubungkan ke broker...");
        String clientId = "ESP32_Node2_" + String(random(0xffff), HEX);
        
        #if defined(MQTT_USER) && defined(MQTT_PASSWORD)
          bool connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
        #else
          bool connected = mqttClient.connect(clientId.c_str());
        #endif

        if (connected) {
          Serial.println(" Terhubung!");
        } else {
          Serial.printf(" Gagal, rc=%d. Coba lagi nanti.\n", mqttClient.state());
        }
      }

      // Jika MQTT aktif, kirim data telemetri
      if (mqttClient.connected()) {
        mqttClient.loop();

        SharedData localData;
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          localData = sharedData;
          xSemaphoreGive(dataMutex);
        }

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

        bool published = mqttClient.publish("office/telemetry", jsonPayload.c_str());
        if (published) {
          Serial.println("[MQTT] Data telemetri berhasil di-publish ke office/telemetry");
        } else {
          Serial.println("[MQTT] Gagal publish data!");
        }
      }
    } else {
      Serial.println("[Wi-Fi] Terputus, mencoba rekoneksi...");
      WiFi.reconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(2500)); // Kirim telemetri setiap 2.5 detik
  }
}

// ================= TASK 3: AC CLIMATE LOGIC =================
void Task_AC_Logic(void *pvParameters) {
  for (;;) {
    SharedData localData;
    if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localData = sharedData;
      xSemaphoreGive(dataMutex);
    }

    // 1. AUTO-OFF: Ruangan Kosong atau Pintu Terbuka Melebihi Batas
    if (localData.acStatus) {
      if (localData.headcount <= 0 || (localData.doorOpen && localData.doorOpenSec > DOOR_TIMEOUT_SEC)) {
        // IR Command to turn off the AC

        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acStatus = false;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] AC dimatikan otomatis (Pintu Terbuka/Ruangan Kosong).");
      }
    }

    // 2. AUTO-ON: Ada Orang & Pintu Tidak Bocor
    if (!localData.acStatus && localData.headcount > 0 && !(localData.doorOpen && localData.doorOpenSec > DOOR_TIMEOUT_SEC)) {
      // IR Command to turn on the AC

      acTurnOnTime = xTaskGetTickCount();
      tempWhenTurnedOn = localData.temp;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sharedData.acStatus = true;
        sharedData.acTargetTemp = 24;
        sharedData.acHealthAlert = false;
        xSemaphoreGive(dataMutex);
      }
      Serial.println("[LOGIKA] Orang terdeteksi di ruangan. Menyalakan AC default 24°C.");
    }

    // 3. DYNAMIC THERMOSTAT (Penyesuaian Suhu Otomatis saat AC Aktif)
    if (localData.acStatus && !isnan(localData.temp)) {
      if (localData.temp < 21.0 && localData.acTargetTemp != 25) {
        // Terlalu Dingin -> Naikkan ke 25°C
        
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 25;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Ruangan terlalu dingin. Target AC dinaikkan ke 25°C.");
      }
      else if (localData.temp > 25.5 && localData.acTargetTemp != 23) {
        // Terlalu Gerah -> Turunkan ke 23°C
        
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 23;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Ruangan panas. Target AC diturunkan ke 23°C.");
      }
      else if (localData.temp >= 21.0 && localData.temp <= 25.5 && localData.acTargetTemp != 24) {
        // Kembali Normal -> Set 24°C
       
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 24;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Suhu ruangan normal. Target AC diatur ke 24°C.");
      }

      // 4. DIAGNOSTIK KESEHATAN AC (Kompresor / Freon Bocor)
      if (!localData.acHealthAlert && (xTaskGetTickCount() - acTurnOnTime > pdMS_TO_TICKS(AC_HEALTH_CHECK_MS))) {
        if ((tempWhenTurnedOn - localData.temp) < 1.5 && localData.temp > 25.5) {
          if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            sharedData.acHealthAlert = true;
            xSemaphoreGive(dataMutex);
          }
          Serial.println("[PERINGATAN] Anomali AC terdeteksi: Suhu tidak turun setelah evaluasi!");
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
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

  // Konfigurasi MQTT Broker
  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setBufferSize(512); // Menjamin ukuran buffer muat seluruh payload JSON

  dataMutex = xSemaphoreCreateMutex();

  // Inisialisasi Data Global Awal
  sharedData.temp = 25.0;
  sharedData.humidity = 60.0;
  sharedData.headcount = 1;
  sharedData.doorOpen = false;
  sharedData.doorOpenSec = 0;
  sharedData.acStatus = true;
  sharedData.acTargetTemp = 24;
  sharedData.acHealthAlert = false;

  xTaskCreatePinnedToCore(Task_DHT22_Read,      "DHT_Task",   2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_MQTT_Telemetry,  "MQTT_Task",  4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_AC_Logic,        "Logic_Task", 3072, NULL, 2, NULL, 1);

  Serial.println("Node 2 Running");
}

void loop() {
  // FreeRTOS mengelola seluruh eksekusi task
}