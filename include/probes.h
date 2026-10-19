// Probe wiring, calibration table and helpers shared by probe_test and the
// monitor firmware. One DS18B20 per pin: the pin decides the depth. Each
// probe's ROM address is read on detection and looked up in KNOWN_PROBES[]
// so its calibration offset follows the physical probe.
//
// Wiring per probe adapter board (each has its own 4.7k pull-up):
//   VCC  -> GPIO4 (3.3 V when on -- never 5 V, see README)
//   GND  -> GND
//   DATA -> SURFACE_PIN, MID_PIN or BOTTOM_PIN below
//
// GPIO5 and GPIO6 found no probes on the first board (all three probes work
// on GPIO7; cause not identified), so surface/mid moved to GPIO10/GPIO1.
#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PROBE_POWER_PIN 4
#define SURFACE_PIN     10
#define MID_PIN         1
#define BOTTOM_PIN      7

static const uint32_t PROBE_CONVERSION_MS = 750;  // DS18B20 max at 12-bit

// Calibration table, keyed by probe ROM address.
// Offsets from a packed crushed-ice bath (0 C), averaged over 16 readings at
// 12-bit: offset = 0.0 - average. Resolution is 0.0625 C, so each offset is
// only good to roughly +/-0.03-0.06 C.
struct KnownProbe {
  DeviceAddress addr;
  float offsetC;
};
static const KnownProbe KNOWN_PROBES[] = {
  { { 0x28, 0x04, 0x66, 0x6F, 0x00, 0x00, 0x00, 0x45 },  0.000f },  // avg  0.000
  { { 0x28, 0xA0, 0x2E, 0x6E, 0x00, 0x00, 0x00, 0x9D }, +0.086f },  // avg -0.086
  { { 0x28, 0x39, 0x46, 0x6E, 0x00, 0x00, 0x00, 0x95 }, +0.102f },  // avg -0.102
};
static const uint8_t KNOWN_COUNT = sizeof(KNOWN_PROBES) / sizeof(KNOWN_PROBES[0]);

static OneWire wireSurface(SURFACE_PIN);
static OneWire wireMid(MID_PIN);
static OneWire wireBottom(BOTTOM_PIN);

static DallasTemperature sensorSurface(&wireSurface);
static DallasTemperature sensorMid(&wireMid);
static DallasTemperature sensorBottom(&wireBottom);

struct Channel {
  const char *name;
  DallasTemperature *sensor;
  DeviceAddress addr;
  bool found;
  bool calibrated;
  float offsetC;
};

static Channel channels[] = {
  { "surface", &sensorSurface },
  { "mid",     &sensorMid },
  { "bottom",  &sensorBottom },
};
static const uint8_t CHANNEL_COUNT = sizeof(channels) / sizeof(channels[0]);

inline void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

// Detects the probe on a channel, reads its address and looks up its offset.
inline bool detectProbe(Channel &ch) {
  ch.sensor->begin();
  ch.sensor->setWaitForConversion(false);  // start all, then wait once
  ch.found = ch.sensor->getAddress(ch.addr, 0);
  ch.calibrated = false;
  ch.offsetC = 0.0f;
  if (!ch.found) {
    Serial.printf("Probe %-7s not found\n", ch.name);
    return false;
  }

  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    if (memcmp(ch.addr, KNOWN_PROBES[i].addr, sizeof(DeviceAddress)) == 0) {
      ch.calibrated = true;
      ch.offsetC = KNOWN_PROBES[i].offsetC;
      break;
    }
  }

  Serial.printf("Probe %-7s { ", ch.name);
  printAddress(ch.addr);
  if (ch.calibrated) {
    Serial.printf(" }  known, offset %+.3f C\n", ch.offsetC);
  } else {
    Serial.println(" }  NOT in KNOWN_PROBES (uncalibrated)");
  }
  return true;
}

// Starts a conversion on every detected probe (non-blocking).
inline void requestAllProbes() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (channels[i].found) channels[i].sensor->requestTemperatures();
  }
}

// Returns the raw reading, or DEVICE_DISCONNECTED_C. Marks the channel as not
// found on failure so it gets detected again.
inline float readRaw(Channel &ch) {
  if (!ch.found) return DEVICE_DISCONNECTED_C;
  float raw = ch.sensor->getTempC(ch.addr);
  if (raw == DEVICE_DISCONNECTED_C) ch.found = false;
  return raw;
}
