#!/usr/bin/env python3
"""Pool temperature logger + web page.

One process does both jobs:
  - an MQTT subscriber (background thread) that stores each reading from
    the ESP32 in SQLite, timestamped with the Pi's clock
  - a small Flask web server that shows current values and history

Settings come from environment variables (see pool-monitor.env.example).
"""

import json
import logging
import os
import sqlite3
import time

import paho.mqtt.client as mqtt
from flask import Flask, jsonify, request, send_from_directory

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
MQTT_USER = os.environ.get("MQTT_USER", "pool")
MQTT_PASSWORD = os.environ.get("MQTT_PASSWORD", "")
MQTT_TOPIC = os.environ.get("MQTT_TOPIC", "pool/temp")
DB_PATH = os.environ.get("DB_PATH", os.path.join(os.path.dirname(__file__), "pool.db"))
WEB_PORT = int(os.environ.get("WEB_PORT", "8090"))
# Readings older than this show as "stale" on the page. Should be a few
# report intervals (firmware default interval is 10 min).
STALE_MINUTES = int(os.environ.get("STALE_MINUTES", "30"))

# Battery thresholds for the page's warning (yellow) and low (red) states.
# Starting guesses: with the 1N5817 in the power path the ESP32 is expected to
# brown out somewhere around 3.3-3.4 V battery; adjust once real data shows it.
BATT_WARN_V = float(os.environ.get("BATT_WARN_V", "3.6"))
BATT_LOW_V = float(os.environ.get("BATT_LOW_V", "3.45"))

FIELDS = ("surface_f", "mid_f", "bottom_f", "uncal", "battery_v", "rssi", "bssid", "boot", "awake_ms")

log = logging.getLogger("pool")
app = Flask(__name__, static_folder="static")


def db():
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    with db() as conn:
        conn.execute("PRAGMA journal_mode=WAL")  # reader and writer don't block
        conn.execute(
            """CREATE TABLE IF NOT EXISTS readings (
                   ts        INTEGER NOT NULL,  -- unix time (Pi clock), UTC
                   surface_f REAL,
                   mid_f     REAL,
                   bottom_f  REAL,
                   uncal     INTEGER,
                   rssi      INTEGER,
                   boot      INTEGER,
                   awake_ms  INTEGER,
                   raw       TEXT              -- original payload
               )"""
        )
        conn.execute("CREATE INDEX IF NOT EXISTS readings_ts ON readings (ts)")
        # Databases created before battery monitoring lack this column.
        cols = [r[1] for r in conn.execute("PRAGMA table_info(readings)")]
        if "battery_v" not in cols:
            conn.execute("ALTER TABLE readings ADD COLUMN battery_v REAL")
        if "bssid" not in cols:
            conn.execute("ALTER TABLE readings ADD COLUMN bssid TEXT")


# --- MQTT -> SQLite --------------------------------------------------------

def on_connect(client, userdata, flags, rc, *args):
    if rc == 0:
        log.info("MQTT connected to %s:%s, subscribing to %s", MQTT_HOST, MQTT_PORT, MQTT_TOPIC)
        client.subscribe(MQTT_TOPIC)
    else:
        log.error("MQTT connect failed, rc=%s", rc)


def on_message(client, userdata, msg):
    # The ESP32 publishes retained, so the broker re-sends the last reading
    # whenever we (re)subscribe. That copy has the retain flag set; skip it so
    # restarts don't log duplicates. Live messages arrive with retain=0.
    if msg.retain:
        return
    raw = msg.payload.decode("utf-8", errors="replace")
    try:
        data = json.loads(raw)
    except ValueError:
        log.warning("Ignoring non-JSON payload: %r", raw)
        return
    row = [int(time.time())] + [data.get(f) for f in FIELDS] + [raw]
    with db() as conn:
        conn.execute(
            "INSERT INTO readings (ts, %s, raw) VALUES (?, %s, ?)"
            % (", ".join(FIELDS), ", ".join("?" * len(FIELDS))),
            row,
        )
    log.info("Stored %s", raw)


def start_mqtt():
    # paho-mqtt 2.x requires a callback API version; 1.x (Raspbian bullseye's
    # python3-paho-mqtt) doesn't have the argument.
    try:
        client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION1, client_id="pool-logger")
    except AttributeError:
        client = mqtt.Client(client_id="pool-logger")
    client.username_pw_set(MQTT_USER, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    client.reconnect_delay_set(min_delay=1, max_delay=60)
    client.connect_async(MQTT_HOST, MQTT_PORT, keepalive=60)
    client.loop_start()  # background thread, reconnects automatically
    return client


# --- Web -------------------------------------------------------------------

@app.route("/")
def index():
    return send_from_directory(app.static_folder, "index.html")


@app.route("/api/latest")
def api_latest():
    with db() as conn:
        row = conn.execute("SELECT * FROM readings ORDER BY ts DESC LIMIT 1").fetchone()
    if row is None:
        return jsonify(None)
    out = {k: row[k] for k in ("ts",) + FIELDS}
    out["age_s"] = int(time.time()) - row["ts"]
    out["stale"] = out["age_s"] > STALE_MINUTES * 60
    out["stale_minutes"] = STALE_MINUTES
    out["batt_warn_v"] = BATT_WARN_V
    out["batt_low_v"] = BATT_LOW_V
    return jsonify(out)


@app.route("/api/readings")
def api_readings():
    hours = min(max(request.args.get("hours", 24, type=float), 1), 24 * 366)
    since = int(time.time() - hours * 3600)
    with db() as conn:
        rows = conn.execute(
            "SELECT ts, surface_f, mid_f, bottom_f, battery_v, rssi, bssid FROM readings "
            "WHERE ts >= ? ORDER BY ts",
            (since,),
        ).fetchall()
    return jsonify({
        "gap_s": STALE_MINUTES * 60,  # break lines across gaps longer than this
        "rows": [dict(r) for r in rows],
    })


def main():
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    init_db()
    start_mqtt()
    # Flask's built-in server is fine for a few users on a home LAN.
    app.run(host="0.0.0.0", port=WEB_PORT, threaded=True)


if __name__ == "__main__":
    main()
