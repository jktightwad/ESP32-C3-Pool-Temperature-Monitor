// WiFi connect-time / RSSI test for the ESP32-C3 SuperMini.
// Each cycle: connect to WiFi, print timing + link info, then sleep 20 s.
// Use it to measure connect time at the pool edge and deep-sleep current.

#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"

// 1 = stay awake between cycles (WiFi fully off, then delay) so the USB serial
//     monitor stays connected. Use this at the desk to read the output.
// 0 = real deep sleep. Use this for battery / sleep-current tests. USB serial
//     drops every cycle in this mode, so output is mostly unreadable.
#define DEBUG_NO_DEEP_SLEEP 1

// Onboard LED: assumed GPIO8, active-low -- verify on your board.
// GPIO8 is a strapping pin; driving it after boot is fine.
#define LED_PIN 8
#define LED_ON  LOW
#define LED_OFF HIGH

// Set to 0 for current measurements so the LED doesn't skew readings.
#define USE_LED 1

static const uint32_t SLEEP_SECONDS   = 20;
static const uint32_t WIFI_TIMEOUT_MS = 10000;
// Native USB serial drops during deep sleep; give the host time to reconnect.
// Has no effect on the WiFi timing figures.
static const uint32_t SERIAL_WAIT_MS  = 2000;

RTC_DATA_ATTR uint32_t cycleCount = 0;
RTC_DATA_ATTR uint32_t failCount = 0;

static void runCycle() {
  cycleCount++;
#if USE_LED
  digitalWrite(LED_PIN, LED_ON);
#endif

  Serial.printf("Cycle #%lu, wake cause %d, failures so far %lu\n",
                (unsigned long)cycleCount, (int)esp_sleep_get_wakeup_cause(),
                (unsigned long)failCount);

  WiFi.mode(WIFI_STA);
  uint32_t wifiStart = millis();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < WIFI_TIMEOUT_MS) {
    delay(10);
  }
  uint32_t connectMs = millis() - wifiStart;

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("Connected in %lu ms\n", (unsigned long)connectMs);
    Serial.printf("RSSI %d dBm, channel %d, BSSID %s, IP %s\n",
                  WiFi.RSSI(), WiFi.channel(), WiFi.BSSIDstr().c_str(),
                  WiFi.localIP().toString().c_str());
  } else {
    failCount++;
    Serial.printf("WiFi connect FAILED after %lu ms (status %d)\n",
                  (unsigned long)connectMs, (int)WiFi.status());
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
#if USE_LED
  digitalWrite(LED_PIN, LED_OFF);
#endif
}

void setup() {
#if USE_LED
  pinMode(LED_PIN, OUTPUT);
#endif

  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < SERIAL_WAIT_MS) {
    delay(10);
  }

#if DEBUG_NO_DEEP_SLEEP
  Serial.println("DEBUG_NO_DEEP_SLEEP: staying awake between cycles");
#else
  runCycle();
  Serial.printf("Awake %lu ms total, deep sleeping %lu s\n\n",
                (unsigned long)millis(), (unsigned long)SLEEP_SECONDS);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
  // Only reached with DEBUG_NO_DEEP_SLEEP; otherwise setup() ends in deep sleep.
  runCycle();
  Serial.printf("Waiting %lu s\n\n", (unsigned long)SLEEP_SECONDS);
  delay(SLEEP_SECONDS * 1000UL);
}
