// DS18B20 probe test / calibration sketch for the ESP32-C3 SuperMini.
// Reads all probes every 3 s and prints calibrated temperatures in F, plus
// the raw reading in C for calibration.
// Probe pins and the calibration table live in include/probes.h.
// Missing probes are detected again automatically on every read.

#include <Arduino.h>
#include "probes.h"

void setup() {
  Serial.begin(115200);
  delay(3000);  // time to open the serial monitor

  pinMode(PROBE_POWER_PIN, OUTPUT);
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);

  Serial.println("Probe test");
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) detectProbe(channels[i]);
}

void loop() {
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (!channels[i].found) detectProbe(channels[i]);
  }
  requestAllProbes();
  delay(PROBE_CONVERSION_MS);

  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    Channel &ch = channels[i];
    float raw = readRaw(ch);
    if (raw == DEVICE_DISCONNECTED_C) {
      Serial.printf("  %s: --", ch.name);
      continue;
    }
    // Displayed in F; raw stays in C (the sensor's unit) for calibration.
    float f = (raw + ch.offsetC) * 9.0f / 5.0f + 32.0f;
    Serial.printf("  %s: %6.2f F (raw %6.3f C)%s", ch.name, f, raw,
                  ch.calibrated ? "" : " uncal");
  }
  Serial.println();

  delay(2250);
}
