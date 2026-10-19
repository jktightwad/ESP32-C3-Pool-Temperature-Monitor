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

Not written yet: Python subscriber → SQLite → Flask + Chart.js page, run by
systemd.
