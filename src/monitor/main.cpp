// Pool temperature monitor firmware for the ESP32-C3 SuperMini.
// Each cycle: power + read the probes (conversion overlaps the WiFi connect),
// publish one JSON message over MQTT (retained), then sleep.
// Probe pins and calibration live in include/probes.h; WiFi + MQTT settings
// in include/secrets.h.
//
// Payload on MQTT_TOPIC, temperatures in F after calibration, null if a probe
// is missing (calibration itself is done in C, the sensor's native unit):
//   {"surface_f":77.56,"mid_f":77.22,"bottom_f":76.89,"uncal":0,
//    "battery_v":3.98,"rssi":-61,"bssid":"AA:BB:CC:DD:EE:FF","boot":42,
//    "awake_ms":2130}
// bssid identifies which access point (e.g. mesh router vs satellite) this
// wake connected to; null if WiFi failed. reset is why the chip last started:
// "deepsleep" is a normal wake; anything else (e.g. "brownout", "poweron")
// means a real restart, which also resets the boot counter. fw is the git
// commit this firmware was built from (see scripts/version.py). missed is how
// many cycles since the last successful publish failed to send; interval is
// the sleep time in effect (set by the Pi via pool/interval).
// "uncal" counts probes on the bus that aren't in KNOWN_PROBES (so have no
// depth or offset). battery_v is read before WiFi starts, so it isn't
// pulled down by transmit current.

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
// Seconds between readings until the Pi sends one (fallback only: the Pi's
// retained message on MQTT_INTERVAL_TOPIC sets the day/night schedule).
#ifndef REPORT_INTERVAL_S
#define REPORT_INTERVAL_S 600
#endif
static const char    *MQTT_INTERVAL_TOPIC = "pool/interval";
static const uint32_t MIN_INTERVAL_S      = 15;    // ignore values outside
static const uint32_t MAX_INTERVAL_S      = 3600;  // this range
static const uint32_t INTERVAL_WAIT_MS    = 500;   // wait for the retained msg
static const uint32_t WIFI_ATTEMPT_MS   = 8000;   // per attempt, 2 attempts
static const char    *MQTT_TOPIC        = "pool/temp";
static const char    *MQTT_CLIENT_ID    = "pool-monitor";

#ifndef FW_VERSION
#define FW_VERSION "unknown"
#endif

RTC_DATA_ATTR uint32_t bootCount = 0;
// Boot count of the last cycle that published, to report how many cycles in
// between failed to (WiFi or MQTT); 0 = none published since power-up.
RTC_DATA_ATTR uint32_t lastPublishedBoot = 0;
// Current interval; kept across deep sleep, updated from the Pi.
RTC_DATA_ATTR uint32_t intervalS = REPORT_INTERVAL_S;
static uint32_t receivedIntervalS = 0;

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  char buf[12];
  len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  receivedIntervalS = strtoul(buf, nullptr, 10);
}

static const char *resetReason() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "external";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
  }
}

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
  Serial.printf("\nCycle %lu, firmware %s\n", (unsigned long)bootCount, FW_VERSION);

  float batteryV = readBatteryV();
  Serial.printf("Battery %.2f V\n", batteryV);

  // Probes: power up, scan the bus, start conversions
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);
  scanProbes();
  requestAllProbes();
  uint32_t convStart = millis();

  // WiFi connects while the probes convert
  bool wifiOk = connectWiFi();

  while (millis() - convStart < PROBE_CONVERSION_MS) delay(5);

  char payload[384];
  size_t pos = snprintf(payload, sizeof(payload), "{");
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
    pos = appendTemp(payload, sizeof(payload), pos, KNOWN_PROBES[k].depth,
                     readCalibratedC(k));
  }
  digitalWrite(PROBE_POWER_PIN, LOW);

  // Cycles since the last successful publish that didn't get through.
  uint32_t missed = (lastPublishedBoot > 0 && bootCount > lastPublishedBoot)
                        ? bootCount - lastPublishedBoot - 1 : 0;

  char bssid[24] = "null";
  if (wifiOk) snprintf(bssid, sizeof(bssid), "\"%s\"", WiFi.BSSIDstr().c_str());
  snprintf(payload + pos, sizeof(payload) - pos,
           ",\"uncal\":%u,\"battery_v\":%.2f,\"rssi\":%d,\"bssid\":%s,\"reset\":\"%s\",\"fw\":\"%s\","
           "\"boot\":%lu,\"missed\":%lu,\"interval\":%lu,\"awake_ms\":%lu}",
           unknownProbes, batteryV, wifiOk ? WiFi.RSSI() : 0, bssid, resetReason(), FW_VERSION,
           (unsigned long)bootCount, (unsigned long)missed, (unsigned long)intervalS,
           (unsigned long)(millis() - cycleStart));
  Serial.println(payload);

  if (wifiOk) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    // Default 256-byte packet limit is close to the worst-case payload (~234
    // bytes with 4 probes); publish() fails silently past it.
    mqtt.setBufferSize(512);
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      bool sent = mqtt.publish(MQTT_TOPIC, payload, true);  // retained
      if (sent) lastPublishedBoot = bootCount;
      Serial.printf("MQTT publish %s\n", sent ? "OK" : "FAILED");

      // The Pi keeps the interval as a retained message, so the broker sends
      // it straight after subscribing.
      receivedIntervalS = 0;
      mqtt.setCallback(onMqttMessage);
      mqtt.subscribe(MQTT_INTERVAL_TOPIC);
      uint32_t waitStart = millis();
      while (!receivedIntervalS && millis() - waitStart < INTERVAL_WAIT_MS) {
        mqtt.loop();
        delay(10);
      }
      if (receivedIntervalS >= MIN_INTERVAL_S && receivedIntervalS <= MAX_INTERVAL_S) {
        intervalS = receivedIntervalS;
        Serial.printf("Interval from Pi: %lu s\n", (unsigned long)intervalS);
      } else {
        Serial.printf("No valid interval from Pi; keeping %lu s\n", (unsigned long)intervalS);
      }

      mqtt.loop();
      delay(50);  // let the TCP stack send before disconnecting
      mqtt.disconnect();
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
  esp_sleep_enable_timer_wakeup((uint64_t)intervalS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
  // Only reached with DEBUG_NO_DEEP_SLEEP; otherwise setup() ends in deep sleep.
  runCycle();
  Serial.printf("Waiting %lu s\n", (unsigned long)intervalS);
  delay(intervalS * 1000UL);
}
