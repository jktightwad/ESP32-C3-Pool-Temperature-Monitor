// DS18B20 probe test / calibration sketch for the ESP32-C3 SuperMini.
// - Powers the probes from GPIO5, reads the 1-Wire bus on GPIO4.
// - Lists every probe's ROM address (copy these into the production firmware
//   to map each probe to a depth).
// - Reads all probes every few seconds at 12-bit resolution and prints each
//   temperature plus the spread between highest and lowest, for ice-bath
//   calibration (all probes in the same bath should read the same).
// Type 'r' in the serial monitor to rescan the bus (e.g. after adding a probe).
//
// Wiring (per probe, all in parallel on one bus):
//   VDD  -> GPIO5   (probe power, switched)
//   GND  -> GND
//   DATA -> GPIO4
//   4.7k resistor between DATA and VDD (one resistor for the whole bus)
// Wire colours vary by vendor -- check your probe's listing/datasheet.

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define ONEWIRE_PIN     4
#define PROBE_POWER_PIN 5

static const uint8_t  MAX_PROBES       = 8;
static const uint32_t READ_INTERVAL_MS = 3000;
static const uint32_t SERIAL_WAIT_MS   = 3000;

OneWire oneWire(ONEWIRE_PIN);
DallasTemperature sensors(&oneWire);

DeviceAddress addrs[MAX_PROBES];
uint8_t probeCount = 0;

static void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

static void scanBus() {
  sensors.begin();
  uint8_t found = sensors.getDeviceCount();
  probeCount = 0;

  Serial.printf("\nScan: %u device(s) on GPIO%d\n", found, ONEWIRE_PIN);
  for (uint8_t i = 0; i < found && probeCount < MAX_PROBES; i++) {
    if (!sensors.getAddress(addrs[probeCount], i)) {
      Serial.printf("  #%u: could not read address\n", i);
      continue;
    }
    if (!sensors.validFamily(addrs[probeCount])) {
      Serial.printf("  #%u: not a DS18x20 family device, skipped\n", i);
      continue;
    }
    sensors.setResolution(addrs[probeCount], 12);
    Serial.printf("  P%u: { ", probeCount);
    printAddress(addrs[probeCount]);
    Serial.println(" }");
    probeCount++;
  }

  if (found == 0) {
    Serial.println("  No probes found. Check wiring, pull-up resistor, and that");
    Serial.println("  GPIO5 is actually powering the probe VDD line.");
  }
  if (sensors.isParasitePowerMode()) {
    Serial.println("  WARNING: a probe reports parasite power -- check its VDD wire.");
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

  Serial.println("DS18B20 probe test");
  scanBus();
}

void loop() {
  while (Serial.available()) {
    if (Serial.read() == 'r') scanBus();
  }

  static uint32_t lastRead = 0;
  if (millis() - lastRead < READ_INTERVAL_MS) return;
  lastRead = millis();

  if (probeCount == 0) {
    Serial.println("No probes (type 'r' to rescan)");
    return;
  }

  sensors.requestTemperatures();  // blocks ~750 ms at 12-bit

  float lo = 1000, hi = -1000;
  uint8_t good = 0;
  Serial.printf("[%7lu s]", (unsigned long)(millis() / 1000));
  for (uint8_t i = 0; i < probeCount; i++) {
    float c = sensors.getTempC(addrs[i]);
    if (c == DEVICE_DISCONNECTED_C) {
      Serial.printf("  P%u: DISCONNECTED", i);
      continue;
    }
    // 85.0 C is the DS18B20 power-on reset value; usually means a bad read
    Serial.printf("  P%u: %7.3f C / %7.3f F%s", i, c, c * 9.0f / 5.0f + 32.0f,
                  c == 85.0f ? " (85.0 = power-on value, suspect)" : "");
    if (c < lo) lo = c;
    if (c > hi) hi = c;
    good++;
  }
  if (good > 1) Serial.printf("  | spread %.3f C", hi - lo);
  Serial.println();
}
