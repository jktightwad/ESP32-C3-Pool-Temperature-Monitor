# PlatformIO upload script for env:ota: instead of flashing over USB, sends
# the built firmware to the Pi, which offers it to the ESP32 on its next wake.
# Uses MQTT_HOST / MQTT_USER / MQTT_PASSWORD from include/secrets.h (the Pi
# accepts the same login), and OTA_WEB_PORT from there if set (default 8090).
import base64
import os
import re
import urllib.error
import urllib.request

Import("env")


def read_secrets():
    path = os.path.join(env.subst("$PROJECT_INCLUDE_DIR"), "secrets.h")
    values = {}
    with open(path, encoding="utf-8") as f:
        for m in re.finditer(r'^\s*#define\s+(\w+)\s+(?:"([^"]*)"|(\S+))', f.read(), re.M):
            values[m.group(1)] = m.group(2) if m.group(2) is not None else m.group(3)
    return values


def ota_upload(source, target, env):
    firmware = str(source[0])
    secrets = read_secrets()
    url = "http://%s:%s/api/firmware" % (secrets["MQTT_HOST"], secrets.get("OTA_WEB_PORT", "8090"))
    version = env.get("FW_VERSION_STR", "unknown")
    with open(firmware, "rb") as f:
        data = f.read()
    auth = base64.b64encode(
        ("%s:%s" % (secrets["MQTT_USER"], secrets["MQTT_PASSWORD"])).encode()
    ).decode()
    req = urllib.request.Request(url, data=data, method="POST", headers={
        "Content-Type": "application/octet-stream",
        "X-Firmware-Version": version,
        "Authorization": "Basic " + auth,
    })
    print("Sending %s (%d bytes, version %s) to %s" % (firmware, len(data), version, url))
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            print(resp.read().decode())
    except urllib.error.HTTPError as e:
        print("Pi refused the upload: %d %s" % (e.code, e.read().decode()))
        return 1
    except OSError as e:
        print("Couldn't reach the Pi at %s: %s" % (url, e))
        return 1
    print("The ESP32 installs it on its next wake (battery must be >= 3.6 V).")
    return 0


env.Replace(UPLOADCMD=ota_upload)
