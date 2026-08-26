#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <DHT.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
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
  bool manualOverride; 
} sharedData;

SemaphoreHandle_t dataMutex;

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

unsigned long acTurnOnTime = 0;
float tempWhenTurnedOn = 0.0;

// ================= HANDLER PESAN MQTT MASUK (MANUAL OVERRIDE) =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  Serial.printf("[MQTT RX] Topik: %s | Pesan: %s\n", topic, message);

  if (String(topic) == "office/ac/command") {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error) {
      const char* action = doc["action"]; // "SET_POWER", "SET_TEMP", "SET_AUTO"

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(action, "SET_POWER") == 0) {
          sharedData.acStatus = doc["value"].as<bool>();
          sharedData.manualOverride = true; // Kunci kontrol manual
          if (sharedData.acStatus) {
            acTurnOnTime = xTaskGetTickCount();
            tempWhenTurnedOn = sharedData.temp;
          }
          Serial.printf("[OVERRIDE] Status AC diubah manual ke: %s\n", sharedData.acStatus ? "ON" : "OFF");
        } 
        else if (strcmp(action, "SET_TEMP") == 0) {
          sharedData.acTargetTemp = doc["target"].as<int>();
          sharedData.manualOverride = true; // Kunci kontrol manual agar tidak ditimpa Dynamic Thermostat
          Serial.printf("[OVERRIDE] Target Suhu AC diubah manual ke: %d°C\n", sharedData.acTargetTemp);
        }
        else if (strcmp(action, "SET_AUTO") == 0) {
          sharedData.manualOverride = false; // Kembalikan ke mode otomatis penuh
          Serial.println("[OVERRIDE] Mode Otomatis Dynamic Thermostat Diaktifkan Kembali.");
        }
        xSemaphoreGive(dataMutex);
      }
    } else {
      Serial.println("[MQTT RX] Gagal parse format JSON command!");
    }
  }
}

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

// ================= TASK 2: MQTT TELEMETRY & COMMAND =================
void Task_MQTT_Telemetry(void *pvParameters) {
  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqttClient.connected()) {
        Serial.println("[MQTT] Menghubungkan ke broker...");
        String clientId = "ESP32_Node2_" + String(random(0xffff), HEX);
        
        #if defined(MQTT_USER) && defined(MQTT_PASSWORD)
          bool connected = mqttClient.connect(clientId.c_str(), MQTT_USER, MQTT_PASSWORD);
        #else
          bool connected = mqttClient.connect(clientId.c_str());
        #endif

        if (connected) {
          Serial.println(" Terhubung!");
          // Subscribe ke topik kendali manual dari dashboard
          mqttClient.subscribe("office/ac/command");
          Serial.println("[MQTT] Berhasil subscribe ke office/ac/command");
        } else {
          Serial.printf(" Gagal, rc=%d. Coba lagi nanti.\n", mqttClient.state());
        }
      }

      if (mqttClient.connected()) {
        mqttClient.loop(); // Memproses antrean pesan masuk / callback

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
        jsonPayload += "\"ac_health_alert\":" + String(localData.acHealthAlert ? "true" : "false") + ",";
        jsonPayload += "\"manual_override\":" + String(localData.manualOverride ? "true" : "false");
        jsonPayload += "}";

        mqttClient.publish("office/telemetry", jsonPayload.c_str());
      }
    } else {
      Serial.println("[Wi-Fi] Terputus, mencoba rekoneksi...");
      WiFi.reconnect();
    }

    vTaskDelay(pdMS_TO_TICKS(2500));
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

    // 1. AUTO-OFF: Tetap aktif sebagai proteksi jika ruangan kosong / pintu bocor
    if (localData.acStatus) {
      if (localData.headcount <= 0 || (localData.doorOpen && localData.doorOpenSec > DOOR_TIMEOUT_SEC)) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acStatus = false;
          sharedData.manualOverride = false; // Reset override jika mati otomatis
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] AC dimatikan otomatis (Pintu Terbuka/Ruangan Kosong).");
      }
    }

    // 2. AUTO-ON: Hanya berjalan jika TIDAK dalam mode manual override
    if (!localData.manualOverride && !localData.acStatus && localData.headcount > 0 && !(localData.doorOpen && localData.doorOpenSec > DOOR_TIMEOUT_SEC)) {
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

    // 3. DYNAMIC THERMOSTAT: Hanya menyesuaikan suhu otomatis jika TIDAK dalam manual override
    if (!localData.manualOverride && localData.acStatus && !isnan(localData.temp)) {
      if (localData.temp < 21.0 && localData.acTargetTemp != 25) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 25;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Ruangan terlalu dingin. Target AC dinaikkan ke 25°C.");
      }
      else if (localData.temp > 25.5 && localData.acTargetTemp != 23) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 23;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Ruangan panas. Target AC diturunkan ke 23°C.");
      }
      else if (localData.temp >= 21.0 && localData.temp <= 25.5 && localData.acTargetTemp != 24) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = 24;
          xSemaphoreGive(dataMutex);
        }
        Serial.println("[LOGIKA] Suhu ruangan normal. Target AC diatur ke 24°C.");
      }

      // 4. DIAGNOSTIK KESEHATAN AC
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

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  dataMutex = xSemaphoreCreateMutex();

  sharedData.temp = 25.0;
  sharedData.humidity = 60.0;
  sharedData.headcount = 1;
  sharedData.doorOpen = false;
  sharedData.doorOpenSec = 0;
  sharedData.acStatus = true;
  sharedData.acTargetTemp = 24;
  sharedData.acHealthAlert = false;
  sharedData.manualOverride = false;

  xTaskCreatePinnedToCore(Task_DHT22_Read,      "DHT_Task",   2048, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_MQTT_Telemetry,  "MQTT_Task",  4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_AC_Logic,        "Logic_Task", 3072, NULL, 2, NULL, 1);

  Serial.println("Node 2 Running");
}

void loop() {
  // FreeRTOS mengelola task
}