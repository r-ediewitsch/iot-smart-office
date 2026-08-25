#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/timers.h"

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include "secrets.h"

#define BUZZER_PIN 2                // Modul Active Buzzer
#define PN532_IRQ   -1              // Mode I2C Standar tanpa IRQ fisik
#define PN532_RESET -1

enum BuzzerEvent {
  BEEP_TAP_IN,
  BEEP_TAP_OUT,
  BEEP_ALARM_DOOR
};

struct SharedDoorData {
  int headcount;
  bool doorOpen;
  unsigned long doorOpenSec;
  uint32_t lastUid;
  char lastAction;
} sharedDoorData;

QueueHandle_t buzzerQueue;
SemaphoreHandle_t doorMutex;

Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// ================= TRACKING KARTU =================
#define MAX_ACTIVE_CARDS 10
uint32_t activeCards[MAX_ACTIVE_CARDS];
int activeCardCount = 0;

bool isCardInside(uint32_t cardId) {
  for (int i = 0; i < activeCardCount; i++) {
    if (activeCards[i] == cardId) return true;
  }
  return false;
}

void registerCardEntry(uint32_t cardId) {
  if (activeCardCount < MAX_ACTIVE_CARDS) {
    activeCards[activeCardCount++] = cardId;
  }
}

void registerCardExit(uint32_t cardId) {
  for (int i = 0; i < activeCardCount; i++) {
    if (activeCards[i] == cardId) {
      for (int j = i; j < activeCardCount - 1; j++) {
        activeCards[j] = activeCards[j + 1];
      }
      activeCardCount--;
      break;
    }
  }
}

// ================= TASK 1: BUZZER AUDIO FEEDBACK =================
void Task_Buzzer(void *pvParameters) {
  BuzzerEvent evt;
  for (;;) {
    if (xQueueReceive(buzzerQueue, &evt, portMAX_DELAY) == pdPASS) {
      if (evt == BEEP_TAP_IN) {
        digitalWrite(BUZZER_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(100));
        digitalWrite(BUZZER_PIN, LOW);
      } 
      else if (evt == BEEP_TAP_OUT) {
        digitalWrite(BUZZER_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(80));
        digitalWrite(BUZZER_PIN, LOW);
        vTaskDelay(pdMS_TO_TICKS(80));
        digitalWrite(BUZZER_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(80));
        digitalWrite(BUZZER_PIN, LOW);
      } 
      else if (evt == BEEP_ALARM_DOOR) {
        digitalWrite(BUZZER_PIN, HIGH);
        vTaskDelay(pdMS_TO_TICKS(200));
        digitalWrite(BUZZER_PIN, LOW);
      }
    }
  }
}

// ================= TASK 2: NFC TAP-IN / TAP-OUT =================
void Task_NFC_Read(void *pvParameters) {
  uint8_t uid[7];
  uint8_t uidLength;
  uint32_t lastProcessedUID = 0;
  unsigned long lastCardTapTime = 0;

  for (;;) {
    bool success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 50);

    if (success) {
      uint32_t currentCardId = 0;
      for (uint8_t i = 0; i < uidLength; i++) {
        currentCardId = (currentCardId << 8) | uid[i];
      }

      // Debounce card tap (2 detik jeda antar tap)
      if (currentCardId != lastProcessedUID || (millis() - lastCardTapTime > 2000)) {
        lastProcessedUID = currentCardId;
        lastCardTapTime = millis();

        bool inside = isCardInside(currentCardId);
        BuzzerEvent bEvt;

        if (xSemaphoreTake(doorMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          sharedDoorData.lastUid = currentCardId;

          if (!inside) {
            // Skenario Masuk (Tap-In)
            registerCardEntry(currentCardId);
            sharedDoorData.headcount++;
            sharedDoorData.lastAction = 'I';
            bEvt = BEEP_TAP_IN;
            Serial.printf("[NFC] Staf Masuk (UID: 0x%08X) | Headcount: %d\n", currentCardId, sharedDoorData.headcount);
          } else {
            // Skenario Keluar (Tap-Out)
            registerCardExit(currentCardId);
            if (sharedDoorData.headcount > 0) sharedDoorData.headcount--;
            sharedDoorData.lastAction = 'O';
            bEvt = BEEP_TAP_OUT;
            Serial.printf("[NFC] Staf Keluar (UID: 0x%08X) | Headcount: %d\n", currentCardId, sharedDoorData.headcount);
          }
          xSemaphoreGive(doorMutex);
        }

        xQueueSend(buzzerQueue, &bEvt, 0);
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}
  

void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.println("[ERROR] PN532 tidak terdeteksi via I2C!");
  } else {
    nfc.SAMConfig();
    Serial.println("[NFC] PN532 Terhubung & Siap Membaca Kartu.");
  }

  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[Wi-Fi] Menghubungkan");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\n[Wi-Fi] Terhubung!");

  doorMutex   = xSemaphoreCreateMutex();
  buzzerQueue = xQueueCreate(10, sizeof(BuzzerEvent));

  sharedDoorData.headcount   = 0;
  sharedDoorData.doorOpen    = false;
  sharedDoorData.doorOpenSec = 0;
  sharedDoorData.lastUid     = 0;
  sharedDoorData.lastAction  = '-';

  xTaskCreatePinnedToCore(Task_Buzzer,      "Buzzer_Task",   2048, NULL, 4, NULL, 1);
  xTaskCreatePinnedToCore(Task_NFC_Read,    "NFC_Task",      3072, NULL, 3, NULL, 1);

  Serial.println("Node 2 Running");
}

void loop() {
  // Aku nak sedikit aksi
}