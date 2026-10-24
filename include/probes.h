// Probe wiring, calibration table and helpers shared by probe_test and the
// monitor firmware.
//
// DS18B20 probes sit on one or more 1-Wire buses (BUS_PINS). Each probe's
// name comes from its ROM address in KNOWN_PROBES[], which also holds its
// calibration offset, so it doesn't matter which bus a probe is on. A probe
// that isn't in the table still reads, but is reported as unknown (no name,
// no offset) until its address is added.
//
// Wiring (each bus has one adapter board, whose 4.7k pull-up serves it):
//   all probe VCC  -> GPIO4 (3.3 V when on -- never 5 V, see README)
//   all probe GND  -> GND
//   GPIO7  bus: surface, mid, bottom (pool probes)
//   GPIO10 bus: solar heater outlet (all four on one bus failed; long
//               star-wired cables are the suspected cause)
// Battery: TP4056 OUT+ -- 100k -- GPIO3 -- 100k -- GND (100 nF GPIO3-GND)
#pragma once

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PROBE_POWER_PIN 4
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

static OneWire wire7(7);
static OneWire wire10(10);
static DallasTemperature bus7(&wire7);
static DallasTemperature bus10(&wire10);
static DallasTemperature *const BUSES[] = { &bus7, &bus10 };
static const uint8_t BUS_PINS[] = { 7, 10 };
static const uint8_t BUS_COUNT = sizeof(BUSES) / sizeof(BUSES[0]);

// Which bus each known probe was found on in the last scan (-1 = not found),
// and how many unknown probes were seen.
static int8_t probeBus[KNOWN_COUNT];
static uint8_t unknownProbes = 0;

inline void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

// Scans every bus, matches each probe to KNOWN_PROBES and prints what it
// found. Returns the total number of devices across all buses.
inline uint8_t scanProbes() {
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) probeBus[k] = -1;
  unknownProbes = 0;
  uint8_t total = 0;

  for (uint8_t b = 0; b < BUS_COUNT; b++) {
    DallasTemperature &bus = *BUSES[b];
    bus.begin();
    bus.setWaitForConversion(false);  // start conversions, then wait once
    bus.setResolution(12);
    uint8_t count = bus.getDeviceCount();
    total += count;
    for (uint8_t i = 0; i < count; i++) {
      DeviceAddress a;
      if (!bus.getAddress(a, i)) continue;
      int match = -1;
      for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
        if (memcmp(a, KNOWN_PROBES[k].addr, sizeof(DeviceAddress)) == 0) match = k;
      }
      Serial.printf("GPIO%u probe { ", BUS_PINS[b]);
      printAddress(a);
      if (match >= 0) {
        probeBus[match] = b;
        Serial.printf(" }  %s, offset %+.3f C\n", KNOWN_PROBES[match].depth,
                      KNOWN_PROBES[match].offsetC);
      } else {
        unknownProbes++;
        Serial.println(" }  UNKNOWN (add to KNOWN_PROBES with its name)");
      }
    }
  }
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
    if (probeBus[k] < 0) Serial.printf("Probe %s NOT FOUND\n", KNOWN_PROBES[k].depth);
  }
  return total;
}

// Starts a conversion on every probe on every bus (non-blocking).
inline void requestAllProbes() {
  for (uint8_t b = 0; b < BUS_COUNT; b++) BUSES[b]->requestTemperatures();
}

// Raw reading of known probe k in C, or DEVICE_DISCONNECTED_C.
inline float readRawC(uint8_t k) {
  if (probeBus[k] < 0) return DEVICE_DISCONNECTED_C;
  return BUSES[probeBus[k]]->getTempC(KNOWN_PROBES[k].addr);
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
