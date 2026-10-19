// Pool temperature monitor firmware for the ESP32-C3 SuperMini.
// Each cycle: power + read the probes (conversion overlaps the WiFi connect),
// publish one JSON message over MQTT (retained), then sleep.
// Probe pins and calibration live in include/probes.h; WiFi + MQTT settings
// in include/secrets.h.
//
// Payload on MQTT_TOPIC, temperatures in F after calibration, null if a probe
// is missing (calibration itself is done in C, the sensor's native unit):
//   {"surface_f":77.56,"mid_f":77.22,"bottom_f":76.89,"uncal":0,"rssi":-61,
//    "boot":42,"awake_ms":2130}
// "uncal" counts probes found but not in KNOWN_PROBES (no offset applied).

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include "secrets.h"
#include "probes.h"

// Local test settings: override either of these in include/secrets.h (which
// isn't committed) instead of editing this file, so pulls never conflict.
//
// 1 = stay awake between cycles so the USB serial monitor stays connected.
// 0 = real deep sleep (for battery use; USB serial drops every cycle).
#ifndef DEBUG_NO_DEEP_SLEEP
#define DEBUG_NO_DEEP_SLEEP 1
#endif
// Seconds between readings.
#ifndef REPORT_INTERVAL_S
#define REPORT_INTERVAL_S 600
#endif
static const uint32_t WIFI_ATTEMPT_MS   = 8000;   // per attempt, 2 attempts
static const char    *MQTT_TOPIC        = "pool/temp";
static const char    *MQTT_CLIENT_ID    = "pool-monitor";

RTC_DATA_ATTR uint32_t bootCount = 0;

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

// Occasional connect attempts fail outright (seen on the first connect after
// boot and on a reconnect in debug mode; cause unknown), so if the first
// attempt times out, reset the radio and try once more.
static bool connectWiFi() {
  for (uint8_t attempt = 1; attempt <= 2; attempt++) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_ATTEMPT_MS) {
      delay(10);
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("WiFi connected after %lu ms (attempt %u)\n",
                    (unsigned long)(millis() - start), attempt);
      return true;
    }
    Serial.printf("WiFi attempt %u FAILED after %lu ms (status %d)\n", attempt,
                  (unsigned long)(millis() - start), (int)WiFi.status());
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    delay(100);
  }
  return false;
}

// Appends ,"<key>_f":value (or null) to the JSON buffer, converting C to F.
static size_t appendTemp(char *buf, size_t len, size_t pos, const char *key, float c) {
  const char *sep = pos > 1 ? "," : "";
  if (c == DEVICE_DISCONNECTED_C) {
    return pos + snprintf(buf + pos, len - pos, "%s\"%s_f\":null", sep, key);
  }
  return pos + snprintf(buf + pos, len - pos, "%s\"%s_f\":%.2f", sep, key,
                        c * 9.0f / 5.0f + 32.0f);
}

static void runCycle() {
  uint32_t cycleStart = millis();
  bootCount++;
  Serial.printf("\nCycle %lu\n", (unsigned long)bootCount);

  // Probes: power up, detect, start conversions
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);
  uint8_t uncal = 0;
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (detectProbe(channels[i]) && !channels[i].calibrated) uncal++;
  }
  requestAllProbes();
  uint32_t convStart = millis();

  // WiFi connects while the probes convert
  bool wifiOk = connectWiFi();

  while (millis() - convStart < PROBE_CONVERSION_MS) delay(5);

  char payload[200];
  size_t pos = snprintf(payload, sizeof(payload), "{");
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    Channel &ch = channels[i];
    float raw = readRaw(ch);
    float c = (raw == DEVICE_DISCONNECTED_C) ? raw : raw + ch.offsetC;
    pos = appendTemp(payload, sizeof(payload), pos, ch.name, c);
  }
  digitalWrite(PROBE_POWER_PIN, LOW);

  snprintf(payload + pos, sizeof(payload) - pos,
           ",\"uncal\":%u,\"rssi\":%d,\"boot\":%lu,\"awake_ms\":%lu}",
           uncal, wifiOk ? WiFi.RSSI() : 0, (unsigned long)bootCount,
           (unsigned long)(millis() - cycleStart));
  Serial.println(payload);

  if (wifiOk) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      bool sent = mqtt.publish(MQTT_TOPIC, payload, true);  // retained
      mqtt.loop();
      delay(50);  // let the TCP stack send before disconnecting
      mqtt.disconnect();
      Serial.printf("MQTT publish %s\n", sent ? "OK" : "FAILED");
    } else {
      Serial.printf("MQTT connect FAILED (state %d)\n", mqtt.state());
    }
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void setup() {
  Serial.begin(115200);
#if DEBUG_NO_DEEP_SLEEP
  delay(3000);  // time to open the serial monitor
#endif
  pinMode(PROBE_POWER_PIN, OUTPUT);

#if !DEBUG_NO_DEEP_SLEEP
  runCycle();
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)REPORT_INTERVAL_S * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
  // Only reached with DEBUG_NO_DEEP_SLEEP; otherwise setup() ends in deep sleep.
  runCycle();
  Serial.printf("Waiting %lu s\n", (unsigned long)REPORT_INTERVAL_S);
  delay((uint32_t)REPORT_INTERVAL_S * 1000UL);
}
