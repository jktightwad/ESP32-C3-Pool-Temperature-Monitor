// Copy this file to secrets.h (same folder) and fill in your values.
// secrets.h is in .gitignore so credentials never get committed.
#pragma once

#define WIFI_SSID     "your-ssid"
#define WIFI_PASSWORD "your-password"

// Mosquitto broker on the Pi (see docs/PI_SETUP.md)
#define MQTT_HOST     "192.168.1.x"   // Pi's IP address
#define MQTT_PORT     1883
#define MQTT_USER     "pool"
#define MQTT_PASSWORD "your-mqtt-password"
