// DS18B20 probe test / calibration sketch for the ESP32-C3 SuperMini.
// Scans the shared 1-Wire bus every 3 s and prints each known probe's
// calibrated temperature in F (plus the raw reading in C, for calibration)
// and the battery voltage. Probe addresses, depths and offsets live in
// include/probes.h.

#include <Arduino.h>
#include "probes.h"

void setup() {
  Serial.begin(115200);
  delay(3000);  // time to open the serial monitor

  pinMode(PROBE_POWER_PIN, OUTPUT);
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);

  Serial.println("Probe test (single bus)");
}

void loop() {
  Serial.println();
  uint8_t count = scanProbes();
  requestAllProbes();
  delay(PROBE_CONVERSION_MS);

  Serial.printf("%u device(s) on GPIO%d |", count, ONEWIRE_PIN);
  for (uint8_t k = 0; k < KNOWN_COUNT; k++) {
    float raw = readRawC(k);
    if (raw == DEVICE_DISCONNECTED_C) {
      Serial.printf("  %s: --", KNOWN_PROBES[k].depth);
      continue;
    }
    float f = (raw + KNOWN_PROBES[k].offsetC) * 9.0f / 5.0f + 32.0f;
    Serial.printf("  %s: %6.2f F (raw %6.3f C)", KNOWN_PROBES[k].depth, f, raw);
  }
  Serial.printf("  | battery %.2f V\n", readBatteryV());

  delay(2250);
}
