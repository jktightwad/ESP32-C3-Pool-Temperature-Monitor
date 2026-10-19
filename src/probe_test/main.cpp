// DS18B20 probe test / calibration sketch for the ESP32-C3 SuperMini.
// One probe per pin: the pin decides the depth.
// Each probe's ROM address is read when it's detected (at boot, or when it's
// reconnected) and looked up in KNOWN_PROBES[] to apply its calibration
// offset, so the offset follows the physical probe if probes are swapped.
// Unknown probes still read, flagged "uncal".
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

// Calibration table, keyed by probe ROM address.
// Current offsets are RELATIVE only: from a shared bath that was ~1.07 C, not
// a verified 0 C, so they align the probes to their mean (spread was 0.125 C)
// but don't correct absolute error. For absolute calibration, redo with a
// packed crushed-ice bath (offset = 0.0 - reading) or a reference thermometer.
struct KnownProbe {
  DeviceAddress addr;
  float offsetC;
};
static const KnownProbe KNOWN_PROBES[] = {
  { { 0x28, 0x04, 0x66, 0x6F, 0x00, 0x00, 0x00, 0x45 }, -0.050f },  // read 1.125
  { { 0x28, 0xA0, 0x2E, 0x6E, 0x00, 0x00, 0x00, 0x9D }, +0.038f },  // read 1.037
  { { 0x28, 0x39, 0x46, 0x6E, 0x00, 0x00, 0x00, 0x95 }, +0.013f },  // read 1.062
};
static const uint8_t KNOWN_COUNT = sizeof(KNOWN_PROBES) / sizeof(KNOWN_PROBES[0]);

OneWire wireSurface(SURFACE_PIN);
OneWire wireMid(MID_PIN);
OneWire wireBottom(BOTTOM_PIN);

DallasTemperature sensorSurface(&wireSurface);
DallasTemperature sensorMid(&wireMid);
DallasTemperature sensorBottom(&wireBottom);

struct Channel {
  const char *name;
  DallasTemperature *sensor;
  DeviceAddress addr;
  bool found;
  bool calibrated;
  float offsetC;
};

Channel channels[] = {
  { "surface", &sensorSurface },
  { "mid",     &sensorMid },
  { "bottom",  &sensorBottom },
};
static const uint8_t CHANNEL_COUNT = sizeof(channels) / sizeof(channels[0]);

static void printAddress(const DeviceAddress a) {
  for (uint8_t i = 0; i < 8; i++) {
    Serial.printf("0x%02X%s", a[i], i < 7 ? ", " : "");
  }
}

// Detects the probe on a channel, reads its address and looks up its offset.
static void detect(Channel &ch) {
  ch.sensor->begin();
  ch.sensor->setWaitForConversion(false);  // start all, then wait once
  ch.found = ch.sensor->getAddress(ch.addr, 0);
  if (!ch.found) return;

  ch.calibrated = false;
  ch.offsetC = 0.0f;
  for (uint8_t i = 0; i < KNOWN_COUNT; i++) {
    if (memcmp(ch.addr, KNOWN_PROBES[i].addr, sizeof(DeviceAddress)) == 0) {
      ch.calibrated = true;
      ch.offsetC = KNOWN_PROBES[i].offsetC;
      break;
    }
  }

  Serial.printf("Detected %-7s { ", ch.name);
  printAddress(ch.addr);
  if (ch.calibrated) {
    Serial.printf(" }  known, offset %+.3f C\n", ch.offsetC);
  } else {
    Serial.println(" }  NOT in KNOWN_PROBES (uncalibrated)");
  }
}

void setup() {
  Serial.begin(115200);
  delay(3000);  // time to open the serial monitor

  pinMode(PROBE_POWER_PIN, OUTPUT);
  digitalWrite(PROBE_POWER_PIN, HIGH);
  delay(50);

  Serial.println("Probe test");
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) detect(channels[i]);
}

void loop() {
  // Retry missing probes, start all conversions, wait once (750 ms at 12-bit)
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (!channels[i].found) detect(channels[i]);
  }
  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    if (channels[i].found) channels[i].sensor->requestTemperatures();
  }
  delay(750);

  for (uint8_t i = 0; i < CHANNEL_COUNT; i++) {
    Channel &ch = channels[i];
    float raw = ch.found ? ch.sensor->getTempC(ch.addr) : DEVICE_DISCONNECTED_C;
    if (raw == DEVICE_DISCONNECTED_C) {
      Serial.printf("  %s: --", ch.name);
      ch.found = false;  // detect again next loop
      continue;
    }
    float c = raw + ch.offsetC;
    Serial.printf("  %s: %6.2f C / %6.2f F (raw %6.3f)%s", ch.name, c,
                  c * 9.0f / 5.0f + 32.0f, raw, ch.calibrated ? "" : " uncal");
  }
  Serial.println();

  delay(2250);
}
