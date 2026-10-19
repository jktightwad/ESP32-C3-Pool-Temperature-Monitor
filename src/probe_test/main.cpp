// DS18B20 probe test / calibration sketch for the ESP32-C3 SuperMini.
// One probe per GPIO (one 1-Wire bus each), so a failed probe can't take
// down the others and the pin identifies the depth.
// - Powers all probes from PROBE_POWER_PIN.
// - Prints each probe's ROM address (record it with the calibration offset).
// - Reads all probes every few seconds at 12-bit resolution and prints each
//   temperature plus the spread between highest and lowest, for ice-bath
//   calibration (all probes in the same bath should read the same).
// Missing probes are rescanned automatically on every read, so probes can be
// moved between pins while it runs. Type 'r' to print a full scan.
//
// Wiring per probe adapter board (each has its own 4.7k pull-up):
//   VCC  -> PROBE_POWER_PIN (3.3 V when on -- never 5 V, see README)
//   GND  -> GND
//   DATA -> that probe's pin in PROBES[] below

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

// Switched 3.3 V supply for all probe VCC terminals.
#define PROBE_POWER_PIN 4

struct Probe {
  const char *name;
  uint8_t pin;
  OneWire bus;
  DallasTemperature sensor;
  DeviceAddress addr;
  bool found;

  Probe(const char *n, uint8_t p) : name(n), pin(p), bus(p), sensor(&bus), found(false) {}
};

// Depth assignment by pin -- edit if you wire them differently.
Probe probes[] = {
  {"surface", 5},
  {"mid",     6},
  {"bottom",  7},
};
static const uint8_t PROBE_COUNT = sizeof(probes) / sizeof(probes[0]);

static const uint32_t READ_INTERVAL_MS = 3000;
static const uint32_t CONVERSION_MS    = 750;   // DS18B20 max at 12-bit
static const uint32_t SERIAL_WAIT_MS   = 3000;

static void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

// Looks for the probe on one pin. Prints the result if verbose, or if the
// probe was newly found (so a reconnected probe is announced).
static void scanProbe(Probe &p, bool verbose) {
  bool wasFound = p.found;
  p.sensor.begin();
  p.sensor.setWaitForConversion(false);  // start all, then wait once
  p.found = p.sensor.getAddress(p.addr, 0) && p.sensor.validFamily(p.addr);
  if (p.found) p.sensor.setResolution(p.addr, 12);

  if (!verbose && (wasFound || !p.found)) return;

  Serial.printf("  %-7s GPIO%u: ", p.name, p.pin);
  if (!p.found) {
    Serial.println("NOT FOUND (check DATA wire and adapter VCC/GND)");
    return;
  }
  Serial.print("{ ");
  printAddress(p.addr);
  Serial.print(" }");
  if (p.sensor.getDeviceCount() > 1) {
    Serial.printf("  (%u devices on this pin, using the first)", p.sensor.getDeviceCount());
  }
  Serial.println(wasFound ? "" : (verbose ? "" : "  <- found"));
}

static void scanProbes() {
  Serial.println("\nScan:");
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    scanProbe(probes[i], true);
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  uint32_t serialStart = millis();
  while (!Serial && millis() - serialStart < SERIAL_WAIT_MS) {
    delay(10);
  }

  pinMode(PROBE_POWER_PIN, OUTPUT);
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);  // let probes power up before talking to them

  Serial.println("DS18B20 probe test (one probe per pin)");
  scanProbes();
}

void loop() {
  while (Serial.available()) {
    if (Serial.read() == 'r') scanProbes();
  }

  static uint32_t lastRead = 0;
  if (millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  // Retry any missing probe, then start conversions on every bus and wait
  // once for all of them.
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    if (!probes[i].found) scanProbe(probes[i], false);
  }
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    if (probes[i].found) probes[i].sensor.requestTemperatures();
  }
  delay(CONVERSION_MS);

  float lo = 1000, hi = -1000;
  uint8_t good = 0;
  Serial.printf("[%7lu s]", (unsigned long)(millis() / 1000));
  for (uint8_t i = 0; i < PROBE_COUNT; i++) {
    Probe &p = probes[i];
    if (!p.found) {
      Serial.printf("  %s: --", p.name);
      continue;
    }
    float c = p.sensor.getTempC(p.addr);
    if (c == DEVICE_DISCONNECTED_C) {
      Serial.printf("  %s: DISCONNECTED", p.name);
      p.found = false;  // rescan it next read
      continue;
    }
    // 85.0 C is the DS18B20 power-on reset value; usually means a bad read
    Serial.printf("  %s: %7.3f C / %7.3f F%s", p.name, c, c * 9.0f / 5.0f + 32.0f,
                  c == 85.0f ? " (85.0 = power-on value, suspect)" : "");
    if (c < lo) lo = c;
    if (c > hi) hi = c;
    good++;
  }
  if (good > 1) Serial.printf("  | spread %.3f C", hi - lo);
  Serial.println();
}
