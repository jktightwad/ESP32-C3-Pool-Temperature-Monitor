# Pool Temperature Monitor

Battery-powered multi-depth water temperature monitor for an above-ground pool.
An ESP32-C3 SuperMini wakes, reads DS18B20 probes, publishes over WiFi/MQTT and
deep sleeps. A Raspberry Pi logs to SQLite and serves a Chart.js web page.

Design notes and decisions: [docs/HANDOFF.md](docs/HANDOFF.md)

## Status

Test sketches, each with its own PlatformIO env (pick it in the bottom status
bar's `env:` selector):

| Env | Source | Purpose |
|-----|--------|---------|
| `monitor` (default) | `src/monitor/` | Production firmware: reads probes, publishes JSON to MQTT `pool/temp`, sleeps |
| `probe_test` | `src/probe_test/` | Lists DS18B20 ROM addresses, reads all probes every 3 s; used for ice-bath calibration |
| `wifi_test` | `src/wifi_test/` | WiFi connect time / RSSI / BSSID, deep-sleep cycle |

## Getting started (VS Code + PlatformIO)

1. Install the **PlatformIO IDE** extension in VS Code.
2. Clone this repo: `Ctrl+Shift+P` → **Git: Clone** → paste the repo URL.
   Open the folder; PlatformIO detects `platformio.ini`.
3. Copy `include/secrets.example.h` to `include/secrets.h` and fill in your WiFi
   and MQTT settings. `secrets.h` is git-ignored. Pi/broker setup: [docs/PI_SETUP.md](docs/PI_SETUP.md).
4. Build (✓ in the status bar), then Upload (→). If the upload doesn't start,
   hold **BOOT**, tap **RESET**, release **BOOT**, and upload again.
5. Open the Serial Monitor (plug icon). With `DEBUG_NO_DEEP_SLEEP 1` (the
   default) the board stays awake between cycles so serial stays connected.
   With `0` it really deep sleeps and USB drops every cycle, so output is
   mostly unreadable -- use that mode for battery/current tests only.

## Pin map (ESP32-C3 SuperMini)

| GPIO | Use | Notes |
|------|-----|-------|
| 4 | Probe power (3.3 V) | To all three adapter VCC terminals; off during sleep |
| 10 | Surface probe DATA | Own 1-Wire bus; pull-up on the adapter board |
| 1 | Mid probe DATA | Own 1-Wire bus; pull-up on the adapter board |
| 7 | Bottom probe DATA | Own 1-Wire bus; pull-up on the adapter board |
| 5, 6 | Not used | Found no probes on the first board; cause not identified |
| 3 | Battery voltage | High-value divider, ADC1 (works with WiFi on) |
| 8 | Onboard LED | Assumed active-low, still to be verified; strapping pin |

Avoid GPIO 2, 8 and 9 (strapping pins) for external circuits.

Each probe connects through its own adapter board (4.7 kΩ "472" pull-up from
DATA to VCC on the board). Power the adapters from GPIO4 (3.3 V), **never 5 V**:
the pull-up puts VCC on the DATA line, and the C3's GPIOs aren't 5 V tolerant.

## Test procedure

1. **Probes:** wire per the table above and run `probe_test`. The pin decides
   the depth; calibration offsets are per pin, so redo the ice bath for a
   probe if it moves to a different pin. For calibration, put all probes in a stirred
   ice-water bath (lots of crushed ice, topped with water) and let them settle
   a few minutes. Record each probe's reading; the offset is `0.000 - reading`.
   Type `r` in the monitor to rescan after adding or swapping probes.
2. **WiFi:** run the sketch at the pool edge. Record connect time and RSSI over
   several cycles, and note any failures.
3. **Sleep current:** set `DEBUG_NO_DEEP_SLEEP 0` and `USE_LED 0`, run from the battery only
   (LiPo+ → meter on mA range → 5V pin) with USB disconnected, and read the
   current during the 20 s sleep. If it's above ~0.1 mA, remove the power LED
   and measure again.

## Open items to verify (not yet confirmed)

- **USB + battery together:** check whether the SuperMini has a diode between
  USB VBUS and the 5V pin. If it doesn't, plugging USB in with the LiPo connected
  would back-feed the battery. Until that's confirmed, disconnect the battery
  before plugging in USB.
- **Charging:** as far as I know the plain SuperMini has no LiPo charger. Plan on
  an external charger/protection module (e.g. a protected TP4056 board) or
  charging the battery separately.
- **Regulator dropout:** as the battery falls toward ~3.4–3.5 V, the 3.3 V rail
  will sag under WiFi load. Confirm the actual brownout point with the battery
  ADC reading, then set the firmware's low-battery cutoff from that.
- **Probe power pin in deep sleep:** GPIO output states aren't guaranteed in
  deep sleep. The production firmware needs to hold GPIO4 low explicitly (or
  pull it down) and confirm the probes are actually off.
- **Battery ADC accuracy:** with a high-value divider, add ~100 nF from the
  ADC pin to GND so the ADC's sample capacitor charges properly. Calibrate
  against a multimeter.

## Roadmap

1. WiFi / sleep-current tests (this sketch)
2. Probe scan + ice-bath calibration sketch
3. Production firmware (fast reconnect, ROM→depth map with offsets, battery ADC,
   JSON over MQTT)
4. Pi stack (Mosquitto, Python subscriber → SQLite, Flask + Chart.js, systemd)
