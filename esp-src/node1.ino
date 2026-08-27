#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <DHT.h>
#include <WiFi.h>
#include <esp_now.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <IRremoteESP8266.h>
#include <ir_Daikin.h>
#include "secrets.h"

// ================= PIN & KONFIGURASI =================
#define DHTPIN 26
#define DHTTYPE DHT22
#define IR_LED_PIN 25               // Pin Data LED Transmitter IR
#define STATUS_LED 2                // LED indikator onboard

const unsigned long DOOR_TIMEOUT_SEC   = 180;
const unsigned long AC_HEALTH_CHECK_MS = 1800000; // 30 menit

// ================= STRUKTUR DATA =================
typedef struct __attribute__((packed)) struct_message {
  int headcount;
  bool doorOpen;
  unsigned long doorOpenSec;
  uint32_t lastUid;
  char lastAction;
} struct_message;

struct_message incomingDoorData;

struct IRCommand {
  char action;                      // 'O' = ON, 'F' = OFF, 'S' = SET_TEMP
  int targetTemp;
};

struct SharedData {
  float temp;
  float humidity;
  int headcount;
  bool doorOpen;
  unsigned long doorOpenSec;
  uint32_t lastUid;
  char lastAction;
  bool acStatus;
  int acTargetTemp;
  bool acHealthAlert;
  bool manualOverride;
} sharedData;

// Handle FreeRTOS & Objek Hardware
SemaphoreHandle_t dataMutex;
QueueHandle_t irQueue;

DHT dht(DHTPIN, DHTTYPE);
IRDaikinESP ac(IR_LED_PIN);
WiFiClient espClient;
PubSubClient mqttClient(espClient);

// Variabel Waktu Diagnostik
unsigned long acTurnOnTime = 0;
float tempWhenTurnedOn = 0.0;

// ================= CALLBACK ESP-NOW (DATA MASUK DARI NODE 1) =================
void OnDataRecv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  if (len != sizeof(struct_message)) {
    Serial.printf("[ESP-NOW RX] Ukuran paket tidak sesuai! Diterima: %d bytes\n", len);
    return;
  }

  memcpy(&incomingDoorData, incomingData, sizeof(incomingDoorData));

  if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    sharedData.headcount   = incomingDoorData.headcount;
    sharedData.doorOpen    = incomingDoorData.doorOpen;
    sharedData.doorOpenSec = incomingDoorData.doorOpenSec;
    sharedData.lastUid     = incomingDoorData.lastUid;
    sharedData.lastAction  = incomingDoorData.lastAction;
    xSemaphoreGive(dataMutex);
  }

  Serial.printf("[ESP-NOW RX] Headcount: %d | Pintu: %s (%lus) | UID: 0x%08X | Aksi: %c\n",
                incomingDoorData.headcount,
                incomingDoorData.doorOpen ? "TERBUKA" : "RAPAT",
                incomingDoorData.doorOpenSec,
                incomingDoorData.lastUid,
                incomingDoorData.lastAction);
}

// ================= TASK 1: IR TRANSMITTER =================
void Task_IR_Transmit(void *pvParameters) {
  IRCommand cmd;
  for (;;) {
    if (xQueueReceive(irQueue, &cmd, portMAX_DELAY) == pdPASS) {
      digitalWrite(STATUS_LED, HIGH);

      if (cmd.action == 'O') {
        ac.on();
        ac.setMode(kDaikinCool);
        ac.setFan(kDaikinFanAuto);
        ac.setTemp(cmd.targetTemp);
        ac.send();
        Serial.printf("[IR EXEC] AC ON | Mode: COOL | Target: %d°C\n", cmd.targetTemp);
      } 
      else if (cmd.action == 'F') {
        ac.off();
        ac.send();
        Serial.println("[IR EXEC] AC OFF");
      } 
      else if (cmd.action == 'S') {
        ac.setTemp(cmd.targetTemp);
        ac.send();
        Serial.printf("[IR EXEC] SET TEMPERATURE: %d°C\n", cmd.targetTemp);
      }

      vTaskDelay(pdMS_TO_TICKS(150));
      digitalWrite(STATUS_LED, LOW);
    }
  }
}

// ================= HANDLER PESAN MQTT MASUK =================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  char message[length + 1];
  memcpy(message, payload, length);
  message[length] = '\0';

  Serial.printf("[MQTT RX] Topik: %s | Pesan: %s\n", topic, message);

  if (String(topic) == "office/ac/command") {
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, message);

    if (!error) {
      const char* action = doc["action"];

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // 1. OVERRIDE POWER AC (ON / OFF)
        if (strcmp(action, "SET_POWER") == 0) {
          bool targetState = false;
          auto val = doc.containsKey("value") ? doc["value"] : doc["state"];

          if (val.is<bool>()) {
            targetState = val.as<bool>();
          } else if (val.is<int>()) {
            targetState = (val.as<int>() == 1);
          } else if (val.is<const char*>()) {
            const char* s = val;
            targetState = (strcasecmp(s, "true") == 0 || strcasecmp(s, "on") == 0 || strcmp(s, "1") == 0);
          }

          sharedData.acStatus = targetState;
          sharedData.manualOverride = true;

          if (sharedData.acStatus) {
            acTurnOnTime = xTaskGetTickCount();
            tempWhenTurnedOn = sharedData.temp;
          }

          // Kirim perintah IR ke antrean
          IRCommand cmd;
          cmd.action = targetState ? 'O' : 'F';
          cmd.targetTemp = sharedData.acTargetTemp;
          xQueueSend(irQueue, &cmd, 0);

          Serial.printf("[OVERRIDE] Status AC diubah manual ke: %s\n", sharedData.acStatus ? "ON" : "OFF");
        } 
        // 2. OVERRIDE TARGET SUHU
        else if (strcmp(action, "SET_TEMP") == 0) {
          int target = doc.containsKey("value") ? doc["value"].as<int>() : doc["target"].as<int>();
          if (target >= 16 && target <= 30) {
            sharedData.acTargetTemp = target;
            sharedData.manualOverride = true;

            // Jika AC sedang menyala, langsung kirim update sinyal IR
            if (sharedData.acStatus) {
              IRCommand cmd;
              cmd.action = 'S';
              cmd.targetTemp = target;
              xQueueSend(irQueue, &cmd, 0);
            }
            Serial.printf("[OVERRIDE] Target Suhu AC diubah manual ke: %d°C\n", sharedData.acTargetTemp);
          }
        }
        // 3. KEMBALI KE MODE OTOMATIS
        else if (strcmp(action, "SET_AUTO") == 0 || (strcmp(action, "SET_MODE") == 0 && (doc["value"] == "auto" || doc["mode"] == "auto"))) {
          sharedData.manualOverride = false;
          Serial.println("[OVERRIDE] Mode Otomatis Dynamic Thermostat Diaktifkan Kembali.");
        }
        xSemaphoreGive(dataMutex);
      }
    } else {
      Serial.println("[MQTT RX] Gagal parse format JSON command!");
    }
  }
}

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

// ================= TASK 3: MQTT TELEMETRY & GATEWAY =================
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
          mqttClient.subscribe("office/ac/command");
          Serial.println("[MQTT] Berhasil subscribe ke office/ac/command");
        } else {
          Serial.printf(" Gagal, rc=%d. Coba lagi nanti.\n", mqttClient.state());
        }
      }

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
        jsonPayload += "\"last_uid\":\"0x" + String(localData.lastUid, HEX) + "\",";
        jsonPayload += "\"last_action\":\"" + String(localData.lastAction) + "\",";
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

// ================= TASK 4: AC CLIMATE LOGIC (OTOMASI IR) =================
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
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acStatus = false;
          sharedData.manualOverride = false; // Reset override jika mati otomatis
          xSemaphoreGive(dataMutex);
        }

        // Matikan AC lewat sinyal IR
        IRCommand cmd = {'F', 0};
        xQueueSend(irQueue, &cmd, 0);

        Serial.println("[LOGIKA] AC dimatikan otomatis (Pintu Terbuka/Ruangan Kosong).");
      }
    }

    // 2. AUTO-ON: Ada Orang & Pintu Tidak Bocor
    if (!localData.manualOverride && !localData.acStatus && localData.headcount > 0 && !(localData.doorOpen && localData.doorOpenSec > DOOR_TIMEOUT_SEC)) {
      acTurnOnTime = xTaskGetTickCount();
      tempWhenTurnedOn = localData.temp;

      if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sharedData.acStatus = true;
        sharedData.acTargetTemp = 24;
        sharedData.acHealthAlert = false;
        xSemaphoreGive(dataMutex);
      }

      // Nyalakan AC lewat sinyal IR dengan target default 24°C
      IRCommand cmd = {'O', 24};
      xQueueSend(irQueue, &cmd, 0);

      Serial.println("[LOGIKA] Orang terdeteksi di ruangan. Menyalakan AC default 24°C.");
    }

    // 3. DYNAMIC THERMOSTAT (Penyesuaian Suhu Otomatis saat AC Aktif)
    if (!localData.manualOverride && localData.acStatus && !isnan(localData.temp)) {
      int newTarget = localData.acTargetTemp;

      if (localData.temp < 21.0 && localData.acTargetTemp != 25) {
        newTarget = 25;
        Serial.println("[LOGIKA] Ruangan terlalu dingin. Target AC dinaikkan ke 25°C.");
      }
      else if (localData.temp > 25.5 && localData.acTargetTemp != 23) {
        newTarget = 23;
        Serial.println("[LOGIKA] Ruangan panas. Target AC diturunkan ke 23°C.");
      }
      else if (localData.temp >= 21.0 && localData.temp <= 25.5 && localData.acTargetTemp != 24) {
        newTarget = 24;
        Serial.println("[LOGIKA] Suhu ruangan normal. Target AC diatur ke 24°C.");
      }

      // Jika ada penyesuaian suhu target, update shared data & kirim sinyal IR
      if (newTarget != localData.acTargetTemp) {
        if (xSemaphoreTake(dataMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedData.acTargetTemp = newTarget;
          xSemaphoreGive(dataMutex);
        }

        IRCommand cmd = {'S', newTarget};
        xQueueSend(irQueue, &cmd, 0);
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

// ================= SETUP =================
void setup() {
  Serial.begin(115200);
  
  pinMode(STATUS_LED, OUTPUT);
  digitalWrite(STATUS_LED, LOW);

  dht.begin();
  ac.begin();

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Wi-Fi] Menghubungkan");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[Wi-Fi] Terhubung!");

  // Inisialisasi ESP-NOW Receiver
  if (esp_now_init() != ESP_OK) {
    Serial.println("[ERROR] Inisialisasi ESP-NOW Gagal!");
  } else {
    esp_now_register_recv_cb(OnDataRecv);
    Serial.println("[ESP-NOW] Terhubung & Siap Menerima Data dari Node 2.");
  }

  mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  mqttClient.setBufferSize(512);

  dataMutex = xSemaphoreCreateMutex();
  irQueue   = xQueueCreate(10, sizeof(IRCommand));

  sharedData.temp            = 25.0;
  sharedData.humidity        = 60.0;
  sharedData.headcount       = 0;
  sharedData.doorOpen        = false;
  sharedData.doorOpenSec     = 0;
  sharedData.acStatus        = false;
  sharedData.acTargetTemp    = 24;
  sharedData.acHealthAlert   = false;
  sharedData.manualOverride  = false;

  // Task Pinning ke Dual-Core
  xTaskCreatePinnedToCore(Task_IR_Transmit,     "IR_Task",    2048, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(Task_DHT22_Read,      "DHT_Task",   3584, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(Task_MQTT_Telemetry,  "MQTT_Task",  4096, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(Task_AC_Logic,        "Logic_Task", 3072, NULL, 2, NULL, 1);

  Serial.println("Node 1 Ready.");
}

void loop() {
  // Aku nak sedikit aksi
}