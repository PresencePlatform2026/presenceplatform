# Changelog

All notable changes to PresencePlatform are documented here.

---

## [2.3.7] — 2026-06-10

### Fixed
- HTTP auth sequencing: authenticate before fetching health endpoint in Control4 driver
- Duplicate event suppression for departure/arrival cycles
- Passive sniffer wake mechanism for ABSENT devices

### Changed
- Miss threshold reduced from 15 to 8 minutes (confidence gate ≥80 prevents false positives)
- Flash usage optimization pass

---

## [2.3.0] — 2026-06

### Added
- GitHub-based OTA auto-update system (`version.json` + `AutoUpdater.h`)
- Auto-update toggle in Settings GUI
- "Update available" banner on dashboard

### Fixed
- ARP table limit — persistent `NetworkScanner` cache survives 10-entry ESP32 cap
- URL encoding for device names with spaces/apostrophes

---

## [2.2.5] — 2026-05

### Changed
- Confidence gate: ≥80 required to transition from ABSENT → HOME (prevents false arrivals)
- MQTT topics standardized to `presence/[label]/state`

---

## [2.0.0] — 2026-04

### Added
- Control4 combo driver v2.0.0 (rebuilt from scratch)
  - 10 contact sensor proxy slots (bindings 5101–5110)
  - Per-person variables: `{Name}_Home`, `{Name}_Confidence`, `{Name}_LastSeen`
  - Per-person events via `C4:AddEvent`
  - Auto-slot assignment from ESP32 state messages
- ESP32 AP subnet moved to 192.168.99.x (avoids eero conflict)

### Fixed
- LuaJIT Lua 5.1 `goto` syntax compatibility
- `C4:FireEvent` (string) replacing `C4:FireEventByID` (numeric)

---

## [1.x] — Legacy

See prior development notes. Not OTA-upgrade compatible.
