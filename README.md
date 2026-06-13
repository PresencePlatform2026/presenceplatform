# PresencePlatform

**ESP32-based WiFi presence detection for professional AV/home automation dealers.**

ProAV Jackson — Commercial dealer product.  
Integrates with Control4, Home Assistant (MQTT), and any MQTT broker.

---

## Features

- Passive + active WiFi presence detection (sub-minute response)
- Web-based configuration GUI (no app required)
- Control4 driver with per-person variables, events, and contact sensor proxies
- Home Assistant MQTT integration
- Over-the-air (OTA) firmware updates via this repository
- LittleFS web UI updates via OTA

---

## Repository Structure

```
presenceplatform/
  firmware/
    PresencePlatform/       ← Arduino sketch + headers
      PresencePlatform.ino
      AutoUpdater.h         ← OTA update logic
      SharedTypes.h
      NetworkScanner.h
      ... 
      data/                 ← LittleFS web UI files
    version.json            ← OTA update manifest (auto-updated on release)
  .github/
    workflows/
      release.yml           ← Auto-build + publish on version tag
  tools/
    manual_release.py       ← Manual release script (alternative to CI)
  docs/
    CHANGELOG.md
```

---

## OTA Update URL

Devices poll this URL to check for updates:

```
https://raw.githubusercontent.com/PresencePlatform2026/presenceplatform/main/firmware/version.json
```

---

## Releasing a New Version

### Automated (recommended)

1. Update `FIRMWARE_VERSION` in `PresencePlatform.ino`
2. Commit everything to `main`
3. Tag and push:
   ```bash
   git tag v2.3.8
   git push origin v2.3.8
   ```
4. GitHub Actions builds the firmware + LittleFS images, updates `version.json`, and creates the release.
5. All field devices will see the update on their next check (boot or 24-hour timer).

### Manual (pre-compiled binaries)

```bash
export GITHUB_TOKEN=ghp_yourtoken
python tools/manual_release.py \
  --version 2.3.8 \
  --firmware path/to/presenceplatform.bin \
  --fs       path/to/presenceplatform.fs.bin
```

---

## Integration with AutoUpdater.h

In your main `.ino`, add:

```cpp
#include "AutoUpdater.h"

AutoUpdater autoUpdater;

void setup() {
  // ... WiFi connect ...
  autoUpdater.begin();
}

void loop() {
  // Pass true for silent auto-update, false for notification only
  bool autoUpdate = config.autoUpdateEnabled;
  OtaResult result = autoUpdater.tick(autoUpdate);

  if (result == OTA_UPDATE_AVAILABLE) {
    // Show "Update available vX.X.X" banner in GUI
    String newVer = autoUpdater.pendingVersion();
    // ... update your GUI state ...
  }
}
```

---

## Dealer Notes

- Default: auto-updates **disabled** — device shows update banner, installer confirms
- Enable "Automatic Updates" toggle in Settings for fully hands-off field updates
- LittleFS (web UI) is updated before firmware on each OTA cycle
- Minimum firmware eligible for OTA: v2.0.0

---

## License

Proprietary — ProAV Jackson. All rights reserved.
