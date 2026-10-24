// Probe wiring, calibration table and helpers shared by probe_test and the
// monitor firmware.
//
// All DS18B20 probes share one 1-Wire bus. Each probe's depth comes from its
// ROM address in KNOWN_PROBES[], which also holds its calibration offset. A
// probe that isn't in the table still reads, but is reported as unknown (no
// depth, no offset) until its address is added.
//
// Wiring (one adapter board, whose 4.7k pull-up serves the whole bus):
//   all probe VCC  -> GPIO4 (3.3 V when on -- never 5 V, see README)
//   all probe GND  -> GND
//   all probe DATA -> ONEWIRE_PIN
// Battery: TP4056 OUT+ -- 100k -- GPIO3 -- 100k -- GND (100 nF GPIO3-GND)
#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PROBE_POWER_PIN 4
#define ONEWIRE_PIN     7
#define BATTERY_PIN     3
#define BATTERY_DIVIDER 2.0f  // two equal resistors halve the battery voltage
// Correction for resistor tolerance + ADC error on this build: firmware read
// 4.11 V where a multimeter read 3.95 V (one point, near full charge).
// Re-check at a lower battery voltage; if the error changes with voltage, a
// single factor isn't enough and this needs a two-point correction.
#define BATTERY_CAL     0.961f

static const uint32_t PROBE_CONVERSION_MS = 750;  // DS18B20 max at 12-bit

// Depth assignment + calibration, keyed by probe ROM address. Order here is
// the order in the payload and on the web page.
// Offsets from a packed crushed-ice bath (0 C), averaged over 16 readings at
// 12-bit: offset = 0.0 - average. Resolution is 0.0625 C, so each offset is
// only good to roughly +/-0.03-0.06 C.
struct KnownProbe {
  const char *depth;
  DeviceAddress addr;
  float offsetC;
};
static const KnownProbe KNOWN_PROBES[] = {
  { "surface", { 0x28, 0x04, 0x66, 0x6F, 0x00, 0x00, 0x00, 0x45 },  0.000f },  // avg  0.000
  { "mid",     { 0x28, 0xA0, 0x2E, 0x6E, 0x00, 0x00, 0x00, 0x9D }, +0.086f },  // avg -0.086
  { "bottom",  { 0x28, 0x39, 0x46, 0x6E, 0x00, 0x00, 0x00, 0x95 }, +0.102f },  // avg -0.102
  // Solar heater outlet. Not ice-bath calibrated yet (offset 0).
  { "solar",   { 0x28, 0x75, 0x25, 0x67, 0x00, 0x00, 0x00, 0x51 },  0.000f },
};
static const uint8_t KNOWN_COUNT = sizeof(KNOWN_PROBES) / sizeof(KNOWN_PROBES[0]);

static OneWire oneWire(ONEWIRE_PIN);
static DallasTemperature sensors(&oneWire);

// Which known probes were found on the last scan, and how many unknown ones.
static bool probePresent[KNOWN_COUNT];
static uint8_t unknownProbes = 0;

inline void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

// Scans the bus, matches each probe to KNOWN_PROBES and prints what it found.
// Returns the number of devices on the bus.
inline uint8_t scanProbes() {
  sensors.begin();
  sensors.setWaitForConversion(false);  // start conversion, then wait once
  sensors.setResolution(12);
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) probePresent[k] = false;
  unknownProbes = 0;

  uint8_t count = sensors.getDeviceCount();
  for (uint8_t i = 0; i < count; i++) {
    DeviceAddress a;
    if (!sensors.getAddress(a, i)) continue;
    int match = -1;
    for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
      if (memcmp(a, KNOWN_PROBES[k].addr, sizeof(DeviceAddress)) == 0) match = k;
    }
    Serial.print("Probe { ");
    printAddress(a);
    if (match >= 0) {
      probePresent[match] = true;
      Serial.printf(" }  %s, offset %+.3f C\n", KNOWN_PROBES[match].depth,
                    KNOWN_PROBES[match].offsetC);
    } else {
      unknownProbes++;
      Serial.println(" }  UNKNOWN (add to KNOWN_PROBES with its depth)");
    }
  }
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
    if (!probePresent[k]) Serial.printf("Probe %s NOT FOUND\n", KNOWN_PROBES[k].depth);
  }
  return count;
}

// Starts a conversion on every probe on the bus (non-blocking).
inline void requestAllProbes() {
  sensors.requestTemperatures();
}

// Raw reading of known probe k in C, or DEVICE_DISCONNECTED_C.
inline float readRawC(uint8_t k) {
  if (!probePresent[k]) return DEVICE_DISCONNECTED_C;
  return sensors.getTempC(KNOWN_PROBES[k].addr);
}

// Calibrated reading of known probe k in C, or DEVICE_DISCONNECTED_C.
inline float readCalibratedC(uint8_t k) {
  float raw = readRawC(k);
  return raw == DEVICE_DISCONNECTED_C ? raw : raw + KNOWN_PROBES[k].offsetC;
}

// Battery voltage via the divider on BATTERY_PIN, averaged over 16 samples.
// analogReadMilliVolts() applies the chip's factory ADC calibration.
inline float readBatteryV() {
  uint32_t mv = 0;
  for (uint8_t i = 0; i < 16; i++) mv += analogReadMilliVolts(BATTERY_PIN);
  return (mv / 16.0f) * BATTERY_DIVIDER * BATTERY_CAL / 1000.0f;
}
