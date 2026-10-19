// Pin identification test: drives one GPIO at a time HIGH (3.3 V) for a few
// seconds while the others are LOW, and prints which GPIO is HIGH.
// Measure each board label with a multimeter (black probe on GND) to confirm
// the silkscreen number matches the GPIO number.
// Disconnect the probe adapters' DATA wires before running this.

#include <Arduino.h>

static const uint8_t PINS[] = {4, 5, 6, 7};
static const uint8_t PIN_COUNT = sizeof(PINS) / sizeof(PINS[0]);
static const uint32_t HOLD_MS = 5000;

void setup() {
  Serial.begin(115200);
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    pinMode(PINS[i], OUTPUT);
    digitalWrite(PINS[i], LOW);
  }
}

void loop() {
  for (uint8_t i = 0; i < PIN_COUNT; i++) {
    digitalWrite(PINS[i], HIGH);
    Serial.printf("GPIO%u HIGH (others LOW)\n", PINS[i]);
    delay(HOLD_MS);
    digitalWrite(PINS[i], LOW);
  }
}
