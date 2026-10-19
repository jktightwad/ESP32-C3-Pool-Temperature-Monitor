# Pool Temperature Monitor — Handoff

## Goal
Battery-powered multi-depth water temperature monitor for a 42" deep above-ground **chlorine** pool. Device wakes, reads probes, sends over WiFi/MQTT, deep sleeps. A Raspberry Pi (piserver) logs data and hosts a web page showing values over time.

## Decisions so far
- **Board:** ESP32-C3 SuperMini (plain, no LCD). Pending sleep-current + WiFi tests.
- **Power:** 1000 mAh LiPo into the **5V pin** (through onboard regulator; never 3.3V pin). No outlet near pool. No solar for now.
- **Sensors:** DS18B20 waterproof probes on one 1-Wire bus, mapped by ROM address to depth.
  - Planned depths: surface (~3–4" below waterline, shaded), mid (~20"), bottom (~36–38"); optional air probe.
  - Mounted on a weighted nylon line hung from top rail, away from wall.
  - Seal probe/cable joints with adhesive-lined heat shrink; inspect after a few weeks.
- **Pins (C3, avoid strapping pins 2/8/9):**
  - GPIO4 = 1-Wire data (4.7k pull-up to probe VDD)
  - GPIO5 = probe power (switched off during sleep)
  - GPIO3 = battery voltage via high-value divider (ADC1, usable with WiFi on)
- **Calibration:** ice-bath all probes together, store per-ROM offsets in firmware.
- **Pi side:** Mosquitto → Python subscriber → SQLite → Flask + Chart.js page (temp vs time per depth, battery voltage, "last seen" warning), systemd services.

## Power plan / estimates (unverified — measure)
- Wake cycle target ~1 s: static IP, cache BSSID/channel in RTC memory, MQTT QoS 0, start DS18B20 "convert all" while WiFi connects, 11-bit resolution (375 ms).
- Interval 10–15 min. Report battery voltage + RSSI in every payload.
- Cutoff ~3.3 V under load. Watch enclosure heat (Texas sun) — shade it.

## Current state
- `platformio.ini` + `src/main.cpp`: test sketch that connects WiFi, prints connect time / RSSI / channel / BSSID, then deep sleeps 20 s in a loop.
  - LED assumed GPIO8 active-low — verify.

## Next steps
1. Run test sketch: record WiFi connect time and RSSI (ideally at pool edge).
2. Measure deep-sleep current on battery only (LiPo+ → meter mA range → 5V pin). If >~0.1 mA, remove power LED and remeasure.
3. Write probe scan + ice-bath calibration sketch.
4. Write production firmware (deep sleep, fast reconnect, ROM→depth map with offsets, battery ADC, JSON over MQTT).
5. Write Pi stack (Mosquitto config, subscriber, Flask app, HTML/Chart.js, systemd units, install steps).

## User preferences
- No positive affirmation; skip status recaps; focus on next steps.
- Has Arduino/ESP/VS Code experience, not an expert.
- Evaluate for accuracy, own errors, never state assumptions as fact.
- Prefers complete file outputs over partial edits.
