#!/usr/bin/env python3
"""Pool temperature logger + web page.

One process does both jobs:
  - an MQTT subscriber (background thread) that stores each reading from
    the ESP32 in SQLite, timestamped with the Pi's clock
  - a small Flask web server that shows current values and history

Settings come from environment variables (see pool-monitor.env.example).
"""

import hashlib
import json
import logging
import os
import re
import sqlite3
import subprocess
import threading
import time

import paho.mqtt.client as mqtt
from flask import Flask, abort, jsonify, request, send_file, send_from_directory

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

# Reporting schedule sent to the ESP32 (retained on INTERVAL_TOPIC; the
# ESP32 reads it on every wake). Day = DAY_START <= local time < DAY_END.
INTERVAL_TOPIC = os.environ.get("INTERVAL_TOPIC", "pool/interval")
DAY_START = os.environ.get("DAY_START", "08:00")
DAY_END = os.environ.get("DAY_END", "19:00")
DAY_INTERVAL_S = int(os.environ.get("DAY_INTERVAL_S", "60"))
NIGHT_INTERVAL_S = int(os.environ.get("NIGHT_INTERVAL_S", "600"))

# OTA firmware offered to the ESP32 (uploaded from the PC by env:ota).
OTA_TOPIC = os.environ.get("OTA_TOPIC", "pool/ota")
FIRMWARE_DIR = os.environ.get("FIRMWARE_DIR", os.path.join(os.path.dirname(os.path.abspath(__file__)), "firmware"))
FIRMWARE_BIN = os.path.join(FIRMWARE_DIR, "firmware.bin")
FIRMWARE_META = os.path.join(FIRMWARE_DIR, "firmware.json")
# The default 4 MB partition table gives each app slot 1.25 MB (0x140000).
FIRMWARE_MAX_BYTES = 0x140000

FIELDS = ("surface_f", "mid_f", "bottom_f", "solar_f", "uncal", "battery_v", "rssi", "bssid", "reset", "fw", "boot", "missed", "interval", "ota_fail", "awake_ms")

log = logging.getLogger("pool")

# The git checkout this app runs from (the repo root, one level above pi/).
REPO_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# Firmware versions already checked since this process started, so a version
# that can't be resolved (e.g. built from commits never pushed) is only acted
# on once per restart instead of on every reading.
_checked_fw = set()
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
        if "reset" not in cols:
            conn.execute("ALTER TABLE readings ADD COLUMN reset TEXT")
        if "fw" not in cols:
            conn.execute("ALTER TABLE readings ADD COLUMN fw TEXT")
        if "solar_f" not in cols:
            conn.execute("ALTER TABLE readings ADD COLUMN solar_f REAL")
        for col in ("missed", "interval", "ota_fail"):
            if col not in cols:
                conn.execute("ALTER TABLE readings ADD COLUMN %s INTEGER" % col)


# --- MQTT -> SQLite --------------------------------------------------------

def on_connect(client, userdata, flags, rc, *args):
    if rc == 0:
        log.info("MQTT connected to %s:%s, subscribing to %s", MQTT_HOST, MQTT_PORT, MQTT_TOPIC)
        client.subscribe(MQTT_TOPIC)
        publish_ota_offer(client)
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
    check_fw_version(data.get("fw"))


def check_fw_version(fw):
    """If the ESP32 runs firmware built from a commit this checkout doesn't
    include yet, start the update service (git pull + restart)."""
    if not fw or fw in _checked_fw:
        return
    _checked_fw.add(fw)
    commit = fw.split("-")[0]  # drop "-dirty"
    if not re.fullmatch(r"[0-9a-f]{7,40}", commit):
        return
    # Exit 0: commit is already part of HEAD. 1: known but not merged.
    # 128: unknown commit (not fetched yet). Either non-zero means we're behind.
    result = subprocess.run(
        ["git", "-C", REPO_DIR, "merge-base", "--is-ancestor", commit, "HEAD"],
        capture_output=True,
    )
    if result.returncode == 0:
        return
    log.info("Firmware %s is newer than this checkout; starting update", fw)
    # --no-block: the update restarts this service, so don't wait on it.
    # Allowed without a password by /etc/sudoers.d/pool-monitor-update.
    started = subprocess.run(
        ["sudo", "-n", "systemctl", "start", "--no-block", "pool-monitor-update.service"],
        capture_output=True, text=True,
    )
    if started.returncode != 0:
        log.warning("Couldn't start pool-monitor-update: %s", started.stderr.strip())


def _minutes(hhmm):
    h, m = hhmm.split(":")
    return int(h) * 60 + int(m)


def current_schedule(now=None):
    """(interval_s, "day"|"night") for the given local time (default now)."""
    t = time.localtime(now)
    minute = t.tm_hour * 60 + t.tm_min
    if _minutes(DAY_START) <= minute < _minutes(DAY_END):
        return DAY_INTERVAL_S, "day"
    return NIGHT_INTERVAL_S, "night"


def publish_schedule_forever(client):
    """Keeps the retained interval message in line with the time of day.
    Re-publishes every minute; the broker just replaces the retained value."""
    last = None
    while True:
        interval, mode = current_schedule()
        if client.is_connected():
            client.publish(INTERVAL_TOPIC, str(interval), qos=1, retain=True)
            if interval != last:
                log.info("Schedule: %s, reporting every %d s", mode, interval)
                last = interval
        time.sleep(60)


def firmware_meta():
    try:
        with open(FIRMWARE_META) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def publish_ota_offer(client):
    """Retained "<version> <url>" for the ESP32 to check on each wake, or
    "none" so it doesn't wait for a message that will never come."""
    meta = firmware_meta()
    offer = "%s %s" % (meta["version"], meta["url"]) if meta else "none"
    client.publish(OTA_TOPIC, offer, qos=1, retain=True)


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
    threading.Thread(target=publish_schedule_forever, args=(client,), daemon=True).start()
    global mqtt_client
    mqtt_client = client
    return client


mqtt_client = None


# --- Web -------------------------------------------------------------------

def _authorized():
    auth = request.authorization
    return bool(auth and auth.username == MQTT_USER and auth.password == MQTT_PASSWORD)


@app.route("/api/firmware", methods=["POST"])
def api_firmware_upload():
    """Receives firmware from the PC (scripts/ota_upload.py) and offers it to
    the ESP32. Same login as MQTT."""
    if not _authorized():
        return "Login required (MQTT user/password)\n", 401
    version = request.headers.get("X-Firmware-Version", "").strip()
    if not re.fullmatch(r"[0-9A-Za-z._-]{1,47}", version):
        return "Missing or invalid X-Firmware-Version\n", 400
    data = request.get_data()
    # ESP32 app images start with magic byte 0xE9.
    if not data or data[0] != 0xE9:
        return "Not an ESP32 firmware image\n", 400
    if len(data) > FIRMWARE_MAX_BYTES:
        return "Firmware is %d bytes; app slot holds %d\n" % (len(data), FIRMWARE_MAX_BYTES), 400

    os.makedirs(FIRMWARE_DIR, exist_ok=True)
    tmp = FIRMWARE_BIN + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, FIRMWARE_BIN)
    meta = {
        "version": version,
        "md5": hashlib.md5(data).hexdigest(),
        "size": len(data),
        "uploaded": int(time.time()),
        # The ESP32 downloads from the same address the PC uploaded to.
        "url": "http://%s/firmware.bin" % request.host,
    }
    with open(FIRMWARE_META + ".tmp", "w") as f:
        json.dump(meta, f)
    os.replace(FIRMWARE_META + ".tmp", FIRMWARE_META)
    if mqtt_client is not None:
        publish_ota_offer(mqtt_client)
    log.info("Firmware %s uploaded (%d bytes); offered on %s", version, len(data), OTA_TOPIC)
    return "Firmware %s stored (%d bytes) and offered to the ESP32\n" % (version, len(data))


@app.route("/firmware.bin")
def firmware_download():
    meta = firmware_meta()
    if meta is None or not os.path.exists(FIRMWARE_BIN):
        abort(404)
    resp = send_file(FIRMWARE_BIN, mimetype="application/octet-stream")
    # HTTPUpdate on the ESP32 verifies the download against this.
    resp.headers["x-MD5"] = meta["md5"]
    return resp


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
    # Probes the firmware reported but couldn't read (key present, value
    # null). Uses the raw payload so a probe the firmware doesn't know about
    # yet (e.g. older firmware without solar) isn't counted as missing.
    try:
        payload = json.loads(row["raw"])
    except (TypeError, ValueError):
        payload = {}
    out["missing"] = [k[:-2] for k, v in payload.items() if k.endswith("_f") and v is None]
    interval, mode = current_schedule()
    out["schedule"] = {"interval": interval, "mode": mode}
    meta = firmware_meta()
    out["ota_version"] = meta["version"] if meta else None
    return jsonify(out)


@app.route("/api/readings")
def api_readings():
    hours = min(max(request.args.get("hours", 24, type=float), 1), 24 * 366)
    # Window end (unix time); default now. Lets the page step back in time.
    end = request.args.get("end", time.time(), type=float)
    since = int(end - hours * 3600)
    with db() as conn:
        rows = conn.execute(
            "SELECT ts, surface_f, mid_f, bottom_f, solar_f, battery_v, rssi, bssid, reset, fw, boot, missed, interval FROM readings "
            "WHERE ts >= ? AND ts <= ? ORDER BY ts",
            (since, int(end)),
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
