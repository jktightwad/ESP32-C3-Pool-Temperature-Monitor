# Raspberry Pi setup

Written for Raspberry Pi OS (Debian-based). Run these on the Pi over SSH.

## 1. Mosquitto (MQTT broker)

Install the broker and the command-line test clients:

```bash
sudo apt update
sudo apt install -y mosquitto mosquitto-clients
```

Create a user for the pool monitor. It asks for a password; that password
goes in `MQTT_PASSWORD` in the ESP32's `include/secrets.h`:

```bash
sudo mosquitto_passwd -c /etc/mosquitto/passwd pool
sudo chown mosquitto:mosquitto /etc/mosquitto/passwd
sudo chmod 600 /etc/mosquitto/passwd
```

(`-c` creates the file and overwrites it if it exists. To add more users
later, leave `-c` off.)

Mosquitto 2.x only accepts connections from the Pi itself unless a listener
is configured. Copy `pi/mosquitto/pool.conf` from this repo to
`/etc/mosquitto/conf.d/pool.conf`, or create it with:

```bash
sudo tee /etc/mosquitto/conf.d/pool.conf > /dev/null <<'EOF'
listener 1883
allow_anonymous false
password_file /etc/mosquitto/passwd
EOF
```

Start it now and on every boot:

```bash
sudo systemctl enable mosquitto
sudo systemctl restart mosquitto
sudo systemctl status mosquitto --no-pager
```

`status` should show `active (running)`. If not, check
`sudo journalctl -u mosquitto -n 50 --no-pager`.

Find the Pi's IP address (put it in `MQTT_HOST` in `secrets.h`):

```bash
hostname -I
```

## 2. Test the broker

In one SSH window, subscribe to everything under `pool/`:

```bash
mosquitto_sub -h localhost -u pool -P 'your-password' -t 'pool/#' -v
```

In a second window, publish a test message:

```bash
mosquitto_pub -h localhost -u pool -P 'your-password' -t pool/test -m hello
```

The first window should print `pool/test hello`. Leave the subscriber running
when you start the ESP32's `monitor` firmware and you'll see each reading
arrive on `pool/temp`.

## 3. Logger + web page

`pi/app.py` is one service that does both: it subscribes to `pool/temp`,
stores every reading in SQLite (`pi/pool.db`, timestamped by the Pi's clock),
and serves the page on port 8090 (8080 is taken by the Orbi dashboard on piserver). Chart.js is bundled in `pi/static/`, so the
page works without internet access.

Install the Python packages from Raspbian's repository (no pip needed):

```bash
sudo apt install -y python3-flask python3-paho-mqtt git
```

Get the code onto the Pi. If the GitHub repo is public:

```bash
cd ~
git clone https://github.com/jktightwad/ESP32-C3-Pool-Temperature-Monitor.git
```

If it's private, `git clone` asks for a username and a GitHub personal access
token (not your password). Alternatively, copy just the `pi/` folder over
from your PC: `scp -r pi pi@piserver:~/ESP32-C3-Pool-Temperature-Monitor/`
(create that folder on the Pi first).

Settings file (holds the MQTT password, so only root can read it):

```bash
sudo cp ~/ESP32-C3-Pool-Temperature-Monitor/pi/pool-monitor.env.example /etc/pool-monitor.env
sudo nano /etc/pool-monitor.env        # set MQTT_PASSWORD
sudo chmod 600 /etc/pool-monitor.env
```

Install as a service so it starts at boot and restarts if it crashes. The
unit assumes user `pi` and the repo at `/home/pi/ESP32-C3-Pool-Temperature-Monitor`;
edit `User=` / `WorkingDirectory=` in the file first if yours differ:

```bash
sudo cp ~/ESP32-C3-Pool-Temperature-Monitor/pi/pool-monitor.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pool-monitor
sudo systemctl status pool-monitor --no-pager
journalctl -u pool-monitor -f          # live log, Ctrl+C to stop
```

The log should show `MQTT connected ... subscribing to pool/temp`, then a
`Stored {...}` line for each reading. Open `http://<pi-ip>:8090` in a
browser (`http://piserver.local:8090` may also work; plain `piserver` didn't
resolve from the PC on this network). Reserve the Pi's IP on the router: the
ESP32's `MQTT_HOST` depends on it too.

Updating: automatic once section 5 is set up; manually it's
`cd ~/ESP32-C3-Pool-Temperature-Monitor && git pull`, then
`sudo systemctl restart pool-monitor`.

### What the page shows

- Current surface / mid / bottom temperature (°F) and WiFi signal
- Status line: green when the last reading is recent; yellow if a probe is
  missing or uncalibrated; red if nothing has arrived for `STALE_MINUTES`
  (default 30; set it to a few times the report interval)
- Temperature and signal history for 24 h / 7 days / 30 days; lines break
  where readings are missing instead of joining across the gap
- A table view of the most recent readings

## 4. Friendly name: http://pool-temp.local:8090

Publishes the extra mDNS name `pool-temp.local` for the Pi (the Pi keeps its
own hostname). The page stays on port 8090 so port 80 remains free for other
services; if several web apps end up on this Pi, a reverse proxy on port 80
(e.g. nginx) can map each name to its app and drop the `:port` from URLs.

```bash
sudo apt install -y avahi-utils
sudo cp ~/ESP32-C3-Pool-Temperature-Monitor/pi/pool-temp-mdns.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pool-temp-mdns
systemctl is-active pool-temp-mdns
```

`pool-temp-mdns.service` contains the Pi's Ethernet IP (192.168.1.86); if
that ever changes, edit it and restart the service. Then open
`http://pool-temp.local:8090`.

## 5. Automatic updates

`pool-monitor-update.timer` runs `pi/auto-update.sh` 5 minutes after boot and
every 15 minutes after that. It fetches from GitHub and fast-forwards the
clone (it never overwrites local edits; it fails instead). If anything under
`pi/` changed it re-installs changed unit files and restarts `pool-monitor`;
firmware-only changes are pulled without a restart.

One-time setup:

```bash
cd ~/ESP32-C3-Pool-Temperature-Monitor && git pull
sudo cp pi/pool-monitor-update.service pi/pool-monitor-update.timer /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now pool-monitor-update.timer
```

Check it:

```bash
systemctl list-timers pool-monitor-update.timer    # next/last run
journalctl -u pool-monitor-update -n 20 --no-pager # what it did
```

## Troubleshooting

### `piserver.local` doesn't resolve (but the IP works)

The router here doesn't resolve device names, so names rely on mDNS (Avahi on
the Pi; the same mechanism that makes `hot-tub.local` work). On piserver,
Avahi had renamed itself to `piserver-2.local` after a name conflict; the Pi is
on the network twice (Ethernet 192.168.1.86 and WiFi 192.168.1.14), which is a
likely cause. Check with:

```bash
journalctl -u avahi-daemon --no-pager | grep -iE "host name|conflict" | tail -5
```

Fix by announcing on Ethernet only:

```bash
sudo sed -i 's/^\[server\]/[server]\nallow-interfaces=eth0/' /etc/avahi/avahi-daemon.conf
sudo systemctl restart avahi-daemon
```

Keep the ESP32's `MQTT_HOST` as an IP address: resolving a `.local` name on
every wake adds time and another way to fail.
