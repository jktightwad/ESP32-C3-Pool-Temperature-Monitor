// DS18B20 probe test for the ESP32-C3 SuperMini.
// One probe per pin: the pin decides the depth, no ROM addresses needed.
// Prints all three temperatures every 3 seconds.
//
// Wiring per probe adapter board (each has its own 4.7k pull-up):
//   VCC  -> GPIO4 (3.3 V when on -- never 5 V, see README)
//   GND  -> GND
//   DATA -> SURFACE_PIN, MID_PIN or BOTTOM_PIN below
//
// GPIO5 and GPIO6 found no probes on the first board (all three probes work
// on GPIO7; cause not identified), so surface/mid moved to GPIO10/GPIO1.

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define PROBE_POWER_PIN 4
#define SURFACE_PIN     10
#define MID_PIN         1
#define BOTTOM_PIN      7

OneWire wireSurface(SURFACE_PIN);
OneWire wireMid(MID_PIN);
OneWire wireBottom(BOTTOM_PIN);

DallasTemperature surface(&wireSurface);
DallasTemperature mid(&wireMid);
DallasTemperature bottom(&wireBottom);

static void printTemp(const char *name, DallasTemperature &probe) {
  float c = probe.getTempCByIndex(0);
  if (c == DEVICE_DISCONNECTED_C) {
    Serial.printf("  %s: --", name);
    probe.begin();  // look for it again next time
    probe.setWaitForConversion(false);
  } else {
    Serial.printf("  %s: %6.2f C / %6.2f F", name, c, c * 9.0f / 5.0f + 32.0f);
  }
}

void setup() {
  Serial.begin(115200);
  delay(3000);  // time to open the serial monitor

  pinMode(PROBE_POWER_PIN, OUTPUT);
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);

  surface.begin();
  mid.begin();
  bottom.begin();
  surface.setWaitForConversion(false);
  mid.setWaitForConversion(false);
  bottom.setWaitForConversion(false);

  Serial.println("Probe test");
}

void loop() {
  // Start all three, then wait once for the conversion (750 ms at 12-bit)
  surface.requestTemperatures();
  mid.requestTemperatures();
  bottom.requestTemperatures();
  delay(750);

  printTemp("surface", surface);
  printTemp("mid", mid);
  printTemp("bottom", bottom);
  Serial.println();

  delay(2250);
}
