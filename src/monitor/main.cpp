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
// the sleep time in effect (set by the Pi via pool/interval). last_fail is
// why the most recent missed cycle failed: "wifi<status>" (WiFi never
// connected; Arduino wl_status_t code), "mqtt<state>" (broker connect failed;
// PubSubClient state code), "pub" (publish failed) or "hang" (the awake-time
// limit forced it back to sleep); "" if none. probes is
// how many devices the scan found on all 1-Wire buses.
// "uncal" counts probes on the bus that aren't in KNOWN_PROBES (so have no
// depth or offset). battery_v is read before WiFi starts, so it isn't
// pulled down by transmit current. ota_fail counts failed attempts to install
// the firmware the Pi is offering.
//
// OTA: the Pi keeps a retained "<version> <url>" on pool/ota (or "none").
// After publishing, if that version differs from FW_VERSION and the battery
// is high enough, the firmware is downloaded into the other app slot and the
// chip restarts into it. A failed download leaves the running firmware in
// place; after OTA_MAX_FAILS failures the same version isn't retried.

#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <HTTPUpdate.h>
#include <esp_timer.h>
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
static const char    *MQTT_OTA_TOPIC      = "pool/ota";
static const float    OTA_MIN_BATTERY_V   = 3.6f;
static const uint8_t  OTA_MAX_FAILS       = 3;
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
// Why the most recent failed cycle didn't publish ("wifi<status>",
// "mqtt<state>" or "pub"); reported in the next successful reading, then
// cleared. Empty = no failure since the last report.
RTC_DATA_ATTR char lastFail[16] = "";
// Current interval; kept across deep sleep, updated from the Pi.
RTC_DATA_ATTR uint32_t intervalS = REPORT_INTERVAL_S;
static uint32_t receivedIntervalS = 0;
// Last OTA offer from the Pi this wake: "<version> <url>" or "none".
static char otaOffer[200];
static bool otaReceived = false;
// Version that failed to install, and how many times, kept across sleep.
RTC_DATA_ATTR char otaFailedVersion[48] = "";
RTC_DATA_ATTR uint8_t otaFailCount = 0;

// Awake-time limit. A wake normally takes 2-3 s; if one is still awake after
// AWAKE_LIMIT_MS (e.g. hung in WiFi/MQTT), the timer records "hang" in
// lastFail and forces deep sleep, so the board recovers on its own at the
// next wake instead of staying awake (and silent) until power-cycled.
static const uint32_t AWAKE_LIMIT_MS     = 90000;
static const uint32_t OTA_AWAKE_LIMIT_MS = 300000;  // firmware download
static esp_timer_handle_t awakeTimer = nullptr;

static void onAwakeTimeout(void *) {
  strcpy(lastFail, "hang");
#if DEBUG_NO_DEEP_SLEEP
  esp_restart();
#else
  esp_sleep_enable_timer_wakeup((uint64_t)intervalS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

// (Re)starts the awake-time limit from now.
static void startAwakeLimit(uint32_t ms) {
  if (!awakeTimer) {
    esp_timer_create_args_t args = {};
    args.callback = &onAwakeTimeout;
    args.name = "awake_limit";
    esp_timer_create(&args, &awakeTimer);
  }
  esp_timer_stop(awakeTimer);  // harmless if not running
  esp_timer_start_once(awakeTimer, (uint64_t)ms * 1000ULL);
}

static void onMqttMessage(char *topic, byte *payload, unsigned int len) {
  if (strcmp(topic, MQTT_OTA_TOPIC) == 0) {
    len = len < sizeof(otaOffer) - 1 ? len : sizeof(otaOffer) - 1;
    memcpy(otaOffer, payload, len);
    otaOffer[len] = '\0';
    otaReceived = true;
    return;
  }
  char buf[12];
  len = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
  memcpy(buf, payload, len);
  buf[len] = '\0';
  receivedIntervalS = strtoul(buf, nullptr, 10);
}

// Installs the firmware the Pi offers, if it's a different version. Only
// returns if there's nothing to do or the update failed; on success the chip
// restarts into the new firmware.
static void maybeUpdateFirmware(float batteryV) {
  if (!otaReceived) return;
  char *space = strchr(otaOffer, ' ');
  if (!space) return;  // "none" or malformed
  *space = '\0';
  const char *version = otaOffer;
  const char *url = space + 1;
  if (strcmp(version, FW_VERSION) == 0) return;
  if (strcmp(version, otaFailedVersion) == 0 && otaFailCount >= OTA_MAX_FAILS) {
    Serial.printf("OTA %s failed %u times; not retrying\n", version, otaFailCount);
    return;
  }
  if (batteryV < OTA_MIN_BATTERY_V) {
    Serial.printf("OTA %s waiting: battery %.2f V < %.2f V\n", version, batteryV, OTA_MIN_BATTERY_V);
    return;
  }

  Serial.printf("OTA: updating %s -> %s from %s\n", FW_VERSION, version, url);
  startAwakeLimit(OTA_AWAKE_LIMIT_MS);
  WiFiClient client;
  httpUpdate.rebootOnUpdate(false);
  t_httpUpdate_return result = httpUpdate.update(client, url);
  if (result == HTTP_UPDATE_OK) {
    Serial.println("OTA OK, restarting");
    Serial.flush();
    ESP.restart();
  }
  Serial.printf("OTA FAILED: %s\n", httpUpdate.getLastErrorString().c_str());
  if (strcmp(version, otaFailedVersion) != 0) {
    strncpy(otaFailedVersion, version, sizeof(otaFailedVersion) - 1);
    otaFailedVersion[sizeof(otaFailedVersion) - 1] = '\0';
    otaFailCount = 0;
  }
  otaFailCount++;
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
  startAwakeLimit(AWAKE_LIMIT_MS);
  uint32_t cycleStart = millis();
  bootCount++;
  Serial.printf("\nCycle %lu, firmware %s\n", (unsigned long)bootCount, FW_VERSION);

  float batteryV = readBatteryV();
  Serial.printf("Battery %.2f V\n", batteryV);

  // Probes: power up, scan the bus, start conversions
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);
  uint8_t probesFound = scanProbes();
  requestAllProbes();
  uint32_t convStart = millis();

  // WiFi connects while the probes convert
  bool wifiOk = connectWiFi();

  while (millis() - convStart < PROBE_CONVERSION_MS) delay(5);

  char payload[448];
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
           "\"boot\":%lu,\"missed\":%lu,\"last_fail\":\"%s\",\"probes\":%u,\"interval\":%lu,"
           "\"ota_fail\":%u,\"awake_ms\":%lu}",
           unknownProbes, batteryV, wifiOk ? WiFi.RSSI() : 0, bssid, resetReason(), FW_VERSION,
           (unsigned long)bootCount, (unsigned long)missed, lastFail, probesFound,
           (unsigned long)intervalS, otaFailCount,
           (unsigned long)(millis() - cycleStart));
  Serial.println(payload);

  if (wifiOk) {
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    // Default 256-byte packet limit is close to the worst-case payload (~234
    // bytes with 4 probes); publish() fails silently past it.
    mqtt.setBufferSize(512);
    if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      bool sent = mqtt.publish(MQTT_TOPIC, payload, true);  // retained
      if (sent) {
        lastPublishedBoot = bootCount;
        lastFail[0] = '\0';  // reported in this payload
      } else {
        strcpy(lastFail, "pub");
      }
      Serial.printf("MQTT publish %s\n", sent ? "OK" : "FAILED");

      // The Pi keeps the interval and the OTA offer as retained messages, so
      // the broker sends both straight after subscribing.
      receivedIntervalS = 0;
      otaReceived = false;
      mqtt.setCallback(onMqttMessage);
      mqtt.subscribe(MQTT_INTERVAL_TOPIC);
      mqtt.subscribe(MQTT_OTA_TOPIC);
      uint32_t waitStart = millis();
      while ((!receivedIntervalS || !otaReceived) && millis() - waitStart < INTERVAL_WAIT_MS) {
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

      maybeUpdateFirmware(batteryV);
    } else {
      Serial.printf("MQTT connect FAILED (state %d)\n", mqtt.state());
      snprintf(lastFail, sizeof(lastFail), "mqtt%d", mqtt.state());
    }
  } else {
    snprintf(lastFail, sizeof(lastFail), "wifi%d", (int)WiFi.status());
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
  esp_timer_stop(awakeTimer);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)intervalS * 1000000ULL);
  esp_deep_sleep_start();
#endif
}

void loop() {
  // Only reached with DEBUG_NO_DEEP_SLEEP; otherwise setup() ends in deep sleep.
  runCycle();
  esp_timer_stop(awakeTimer);
  Serial.printf("Waiting %lu s\n", (unsigned long)intervalS);
  delay(intervalS * 1000UL);
}
