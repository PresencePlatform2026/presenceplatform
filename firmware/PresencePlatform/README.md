# PresencePlatform — Firmware v1.0.0

ESP32-based network presence detector. Monitors phones on your local
network via ARP, mDNS, passive traffic, and ping. Fires automation
events when occupancy changes.

---

## Project structure

```
PresencePlatform/
├── PresencePlatform.ino     Main entry point, FreeRTOS tasks
├── data/
│   └── changelog.json       Firmware changelog (flashed to SPIFFS)
└── src/
    ├── Device.h             Device model + per-device state machine
    ├── Events.h             Event types and callback interface
    ├── DetectionEngine.h    ARP / mDNS / passive / ping detection stack
    ├── StateMachine.h       Global occupancy manager, event dispatcher
    └── Config.h             NVS-backed config + backup/restore
```

---

## Required libraries

Install via Arduino IDE → Library Manager:

| Library | Author | Version |
|---|---|---|
| ESPAsyncWebServer | me-no-dev | latest |
| AsyncTCP | me-no-dev | latest |
| ArduinoJson | Benoit Blanchon | 7.x |

ESPmDNS ships with the ESP32 Arduino core — no separate install needed.

---

## Board settings (Arduino IDE)

| Setting | Value |
|---|---|
| Board | ESP32 Dev Module |
| Partition Scheme | Default 4MB with SPIFFS |
| CPU Frequency | 240 MHz |
| Upload Speed | 921600 |
| Flash Size | 4MB |

---

## GPIO wiring

| Pin | Function | Notes |
|---|---|---|
| GPIO 26 | Relay output | HIGH = HOME, LOW = AWAY |
| GPIO 25 | LED — WiFi (Blue) | HIGH = connected |
| GPIO 33 | LED — Occupancy (Green) | HIGH = someone home |
| GPIO 32 | LED — Fault (Red) | HIGH = fault active |

Use a transistor or relay driver board — do not drive a relay coil
directly from an ESP32 GPIO pin.

---

## First flash procedure

1. Open `PresencePlatform.ino` in Arduino IDE
2. Install required libraries (see above)
3. Select your board and port
4. Flash the firmware: **Sketch → Upload**
5. Flash the SPIFFS image: **Tools → ESP32 Sketch Data Upload**
   (requires the ESP32FS plugin — install separately)
6. Open Serial Monitor at 115200 baud

On first boot, the device has no WiFi credentials. It will log
`No SSID configured — starting setup AP` and wait.
Phase 2 adds the setup AP provisioning flow.

For now, pre-configure credentials directly in Config.h or via
a one-time sketch that writes them to NVS before flashing the
main firmware.

---

## Adding devices (Phase 1 — code method)

Until the web GUI is built, add devices directly in
`PresencePlatform.ino` inside `setup()`:

```cpp
Device jacob;
jacob.id           = "jacob";
jacob.friendlyName = "Jacob's iPhone";
jacob.macStr       = "AA:BB:CC:DD:EE:FF"; // your actual MAC
jacob.ip.fromString("192.168.1.101");
jacob.missThreshold = 5;
Config::parseMac(jacob.macStr, jacob.mac);
g_sm.addDevice(jacob);
```

---

## Serial output guide

```
[BOOT]       — startup sequence
[WiFi]       — WiFi connection status
[Config]     — NVS load/save
[Task:*]     — FreeRTOS task start/stop
[Detection]  — per-device probe results (ARP HIT / MISS etc.)
[StateMachine] — state transitions
[Event]      — presence events fired
[Watchdog]   — health heartbeat every 60s
```

---

## What's built (v1.0.0)

- [x] Multi-method detection engine (ARP, mDNS, passive, ping)
- [x] Per-device state machine (UNKNOWN → PRESENT → SUSPECT → ABSENT)
- [x] Departure acceleration (30s fast-poll on first miss)
- [x] Global occupancy state (HOME / AWAY)
- [x] Event system with ring buffer (last 50 events)
- [x] Relay output + status LEDs
- [x] NVS-backed config with backup/restore
- [x] FreeRTOS task architecture
- [x] Watchdog with WiFi auto-reconnect

## What comes next (v1.1.0)

- [ ] REST API (AsyncWebServer routes)
- [ ] Web GUI (served from SPIFFS)
- [ ] Setup AP for WiFi provisioning
- [ ] Changelog tab in GUI
- [ ] TCP socket server (Control4)
- [ ] MQTT client
- [ ] OTA firmware updates
