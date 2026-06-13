#pragma once
#include <Arduino.h>
#include <vector>
#include <functional>
#include "Device.h"
#include "Events.h"
#include "DetectionEngine.h"

// ============================================================
//  StateMachine.h — Global occupancy state manager
//  PresencePlatform v1.0.0
//
//  Responsibilities:
//    - Own the device list
//    - Run detection cycles (calls DetectionEngine per device)
//    - Transition per-device state (UNKNOWN → PRESENT → SUSPECT → ABSENT)
//    - Derive global occupancy (HOME / AWAY) from device states
//    - Fire PresenceEvents to registered callbacks
//    - Drive relay output and status LEDs
//    - Maintain event history ring buffer (last 50 events)
// ============================================================

// Normal poll interval
#define POLL_INTERVAL_MS       60000UL   // 60 seconds normal poll

// Fast poll interval (used when any device is in SUSPECT state)
#define FAST_POLL_INTERVAL_MS  5000UL    // 5 seconds fast poll (ABSENT only)
#define SUSPECT_POLL_INTERVAL_MS 30000UL  // 30 seconds while SUSPECT

// Event history ring buffer size
#define EVENT_HISTORY_SIZE     50

// GPIO assignments (adjust to match your wiring)
#define PIN_RELAY              26
#define PIN_LED_WIFI           25   // Blue  — WiFi status
#define PIN_LED_OCCUPANCY      33   // Green — anyone home
#define PIN_LED_FAULT          32   // Red   — fault condition

enum class OccupancyState {
  UNKNOWN,
  HOME,   // At least one tracked device is PRESENT or SUSPECT
  AWAY    // All tracked devices are ABSENT
};


class StateMachine {
public:

  // ----------------------------------------------------------
  //  init() — call from setup() after WiFi is connected
  // ----------------------------------------------------------
  void init() {
    _engine.init();

    // GPIO setup
    pinMode(PIN_RELAY,         OUTPUT);
    pinMode(PIN_LED_WIFI,      OUTPUT);
    pinMode(PIN_LED_OCCUPANCY, OUTPUT);
    pinMode(PIN_LED_FAULT,     OUTPUT);

    _setRelay(false);       // Relay open = AWAY on boot
    _setLedWifi(true);      // WiFi LED on (we're connected if init is called)
    _setLedOccupancy(false);
    _setLedFault(false);

    _lastPollMs = 0; // Force immediate poll on first tick()

    Serial.println("[StateMachine] Initialised.");
  }

  // ----------------------------------------------------------
  //  tick() — call from the main detection task loop
  //  Returns true if a poll cycle was run this tick.
  // ----------------------------------------------------------
  bool tick(uint32_t epochNow = 0) {
    uint32_t now = millis();
    bool ranPoll = false;

    // Determine which interval to use
    // 60s polls while anyone is home → 10 × 60s = ~10 min departure
    // 5s fast polls only when EVERYONE is absent → fast return detection
    uint32_t interval = _allAbsent() ? FAST_POLL_INTERVAL_MS
                                     : POLL_INTERVAL_MS;

    if ((now - _lastPollMs) >= interval || _lastPollMs == 0) {
      _runPollCycle(epochNow);
      _lastPollMs = now;
      ranPoll = true;
    }

    return ranPoll;
  }

  // ----------------------------------------------------------
  //  forcePoll() — trigger an immediate out-of-schedule poll.
  //  Called by the TCP socket server on POLL command.
  // ----------------------------------------------------------
  void forcePoll(uint32_t epochNow = 0) {
    Serial.println("[StateMachine] Force poll requested.");
    _forcePollRequested = true;  // picked up by detection task on next cycle
  }

  bool consumeForcePoll() {
    if (_forcePollRequested || bForcePoll) {
      _forcePollRequested = false;
      bForcePoll = false;
      return true;
    }
    return false;
  }

  // Check if a device is ABSENT by MAC string (uppercase)
  bool isDeviceAbsentByMac(const String& mac) const {
    for (const auto& dev : _devices) {
      String devMac = dev.macStr;
      devMac.toUpperCase();
      if (devMac == mac && dev.state == DeviceState::ABSENT) {
        return true;
      }
    }
    return false;
  }

  // Update a device's IP by MAC string
  void updateDeviceIpByMac(const String& mac, const String& newIp) {
    for (auto& dev : _devices) {
      String devMac = dev.macStr;
      devMac.toUpperCase();
      if (devMac == mac) {
        IPAddress ip;
        if (ip.fromString(newIp) && ip != dev.ip) {
          Serial.printf("[StateMachine] IP updated for %s: %s → %s\n",
                        dev.friendlyName.c_str(),
                        dev.ip.toString().c_str(),
                        newIp.c_str());
          dev.ip = ip;
        }
        break;
      }
    }
  }

  // Persist device states to NVS before reboot (prevents false departures after update)
  void persistState(Preferences& prefs) {
    prefs.begin("pp_state", false);
    prefs.clear();
    for (int i = 0; i < (int)_devices.size(); i++) {
      String key = "dev_" + String(i) + "_state";
      prefs.putInt(key.c_str(), (int)_devices[i].state);
      String keyHome = "dev_" + String(i) + "_home";
      prefs.putBool(keyHome.c_str(), _devices[i].state == DeviceState::PRESENT);
    }
    prefs.putInt("dev_count", (int)_devices.size());
    prefs.end();
    Serial.println("[StateMachine] Device states persisted to NVS.");
  }

  // Restore device states from NVS after reboot
  // Devices start as their last known state instead of UNKNOWN
  void restoreState(Preferences& prefs) {
    prefs.begin("pp_state", true);
    int count = prefs.getInt("dev_count", 0);
    if (count == 0 || count != (int)_devices.size()) {
      prefs.end();
      return;
    }
    for (int i = 0; i < count && i < (int)_devices.size(); i++) {
      bool wasHome = prefs.getBool(("dev_" + String(i) + "_home").c_str(), false);
      if (wasHome) {
        // Restore as PRESENT — device gets benefit of the doubt
        // Miss counter starts at 0, departure needs 10 full misses
        _devices[i].state       = DeviceState::PRESENT;
        _devices[i].missCount   = 0;
        Serial.printf("[StateMachine] Restored %s as PRESENT (was home before reboot)\n",
                      _devices[i].friendlyName.c_str());
      } else {
        _devices[i].state       = DeviceState::ABSENT;
        _devices[i].missCount   = _devices[i].missThreshold;
        Serial.printf("[StateMachine] Restored %s as ABSENT (was away before reboot)\n",
                      _devices[i].friendlyName.c_str());
      }
    }
    prefs.end();
    Serial.println("[StateMachine] Device states restored from NVS.");
  }

  // Public force poll flag (checked by detection task)
  volatile bool bForcePoll = false;

  void runForcedPoll(uint32_t epochNow) {
    Serial.println("[StateMachine] Running forced poll cycle.");
    _runPollCycle(epochNow);
    _lastPollMs = millis();
  }

  // ----------------------------------------------------------
  //  Device management
  // ----------------------------------------------------------
  void addDevice(Device dev) {
    // Always apply global miss threshold
    dev.missThreshold = _globalMissThreshold;
    _devices.push_back(dev);
    Serial.printf("[StateMachine] Device added: %s (%s) threshold=%d\n",
                  dev.friendlyName.c_str(), dev.macStr.c_str(), dev.missThreshold);
    _fireEvent({ PresenceEvent::DEVICE_ADDED, dev.id, dev.friendlyName,
                 millis(), 0 });
  }

  void setGlobalMissThreshold(int threshold) {
    _globalMissThreshold = threshold;
    // Update all existing devices
    for (auto& d : _devices) {
      d.missThreshold = threshold;
    }
    Serial.printf("[StateMachine] Global miss threshold set to %d\n", threshold);
  }

  int globalMissThreshold() const { return _globalMissThreshold; }

  bool removeDevice(const String& id) {
    for (auto it = _devices.begin(); it != _devices.end(); ++it) {
      if (it->id == id) {
        String name = it->friendlyName;
        _devices.erase(it);
        _fireEvent({ PresenceEvent::DEVICE_REMOVED, id, name, millis(), 0 });
        _recalcOccupancy(0);
        Serial.printf("[StateMachine] Device removed: %s\n", id.c_str());
        return true;
      }
    }
    return false;
  }

  Device* getDevice(const String& id) {
    for (auto& d : _devices) {
      if (d.id == id) return &d;
    }
    return nullptr;
  }

  const std::vector<Device>& devices() const { return _devices; }

  // ----------------------------------------------------------
  //  Occupancy accessors
  // ----------------------------------------------------------
  OccupancyState occupancy()  const { return _occupancy; }
  bool           anyoneHome() const { return _occupancy == OccupancyState::HOME; }
  int            homeCount()  const {
    int n = 0;
    for (const auto& d : _devices) if (d.isHome()) n++;
    return n;
  }

  const char* occupancyName() const {
    switch (_occupancy) {
      case OccupancyState::HOME:    return "home";
      case OccupancyState::AWAY:    return "away";
      default:                      return "unknown";
    }
  }

  // ----------------------------------------------------------
  //  Event history (ring buffer, last 50 events)
  // ----------------------------------------------------------
  const PresenceEventData* eventHistory()    const { return _history; }
  int                      eventHistoryCount() const { return _historyCount; }

  // ----------------------------------------------------------
  //  Callback registration
  //  Multiple callbacks can be registered (one per output channel).
  // ----------------------------------------------------------
  void onEvent(PresenceEventCallback cb) {
    _callbacks.push_back(cb);
  }

  // ----------------------------------------------------------
  //  Fault reporting (called by watchdog or comms layer)
  // ----------------------------------------------------------
  void reportFault(const String& reason) {
    if (!_faultActive) {
      _faultActive = true;
      _setLedFault(true);
      Serial.printf("[StateMachine] FAULT: %s\n", reason.c_str());
      _fireEvent({ PresenceEvent::COMM_ERROR, "", reason, millis(), 0 });
    }
  }

  void clearFault() {
    if (_faultActive) {
      _faultActive = false;
      _setLedFault(false);
      Serial.println("[StateMachine] Fault cleared.");
      _fireEvent({ PresenceEvent::COMM_RESTORED, "", "", millis(), 0 });
    }
  }


private:

  DetectionEngine              _engine;
  int                          _globalMissThreshold = DEFAULT_MISS_THRESHOLD;
  volatile bool                _forcePollRequested  = false;
  std::vector<Device>          _devices;
  std::vector<PresenceEventCallback> _callbacks;

  OccupancyState  _occupancy    = OccupancyState::UNKNOWN;
  uint32_t        _lastPollMs   = 0;
  bool            _faultActive  = false;

  // Event history ring buffer
  PresenceEventData _history[EVENT_HISTORY_SIZE];
  int               _historyHead  = 0;
  int               _historyCount = 0;

  // ----------------------------------------------------------
  //  Core poll cycle — probe every device, update state
  // ----------------------------------------------------------
  void _runPollCycle(uint32_t epochNow) {
    Serial.printf("[StateMachine] Poll cycle — %d device(s) tracked\n",
                  _devices.size());

    for (auto& dev : _devices) {
      _pollDevice(dev, epochNow);
    }

    _recalcOccupancy(epochNow);
  }

  // ----------------------------------------------------------
  //  Poll a single device and handle state transitions
  // ----------------------------------------------------------
  void _pollDevice(Device& dev, uint32_t epochNow) {
    // Respect per-device fast poll schedule
    if (dev.fastPollActive && !dev.needsFastPoll()) {
      dev.stopFastPoll();
    }

    DeviceState prevState = dev.state;

    DetectionResult result = _engine.probe(dev);

    if (result.seen && result.confidence >= CONFIDENCE_THRESHOLD) {
      // ---- Device found ----
      bool wasAbsent  = (prevState == DeviceState::ABSENT);
      bool wasUnknown = (prevState == DeviceState::UNKNOWN);

      bool stateChanged = dev.markSeen(result.confidence, result.method, epochNow);

      if (stateChanged && (wasAbsent || wasUnknown)) {
        // Device has returned or been seen for the first time
        Serial.printf("[StateMachine] [%s] → ARRIVED (was %s, conf %d via %s)\n",
                      dev.friendlyName.c_str(),
                      _stateName(prevState), result.confidence,
                      dev.methodName());

        _fireEvent({ PresenceEvent::PERSON_ARRIVED, dev.id, dev.friendlyName,
                     millis(), epochNow });
        _updateAbsentMacs(); // remove from passive watch list
      } else if (wasAbsent && !stateChanged) {
        // Still accumulating return confirmation hits
        Serial.printf("[StateMachine] [%s] return hit %d/%d (conf %d)\n",
                      dev.friendlyName.c_str(),
                      dev.returnHits, ABSENT_RETURN_HITS, result.confidence);
      }
      // If was SUSPECT → PRESENT: no event, just resolved naturally

    } else {
      // ---- Device not found ----
      bool thresholdCrossed = dev.recordMiss();

      Serial.printf("[StateMachine] [%s] miss %d/%d → %s\n",
                    dev.friendlyName.c_str(),
                    dev.missCount, dev.missThreshold,
                    dev.stateName());

      if (thresholdCrossed) {
        // Device is now ABSENT — register MAC with passive sniffer for wake-on-return
        _updateAbsentMacs();
        _fireEvent({ PresenceEvent::PERSON_DEPARTED, dev.id, dev.friendlyName,
                     millis(), epochNow });
      }
    }
  }

  // ----------------------------------------------------------
  //  Recalculate global occupancy and fire global events
  // ----------------------------------------------------------
  void _recalcOccupancy(uint32_t epochNow) {
    OccupancyState prev = _occupancy;

    // Count present + suspect as "home"
    int homeCount = 0;
    for (const auto& d : _devices) {
      if (d.isHome()) homeCount++;
    }

    OccupancyState next = (homeCount > 0)
                          ? OccupancyState::HOME
                          : OccupancyState::AWAY;

    if (next == prev && prev != OccupancyState::UNKNOWN) return;

    _occupancy = next;
    _setRelay(next == OccupancyState::HOME);
    _setLedOccupancy(next == OccupancyState::HOME);

    if (next == OccupancyState::HOME && prev != OccupancyState::HOME) {
      Serial.println("[StateMachine] OCCUPANCY → HOME");
      _fireEvent({ PresenceEvent::FIRST_ARRIVAL, "", "", millis(), epochNow });
      _fireEvent({ PresenceEvent::ANYONE_HOME,   "", "", millis(), epochNow });
    } else if (next == OccupancyState::AWAY && prev != OccupancyState::AWAY) {
      Serial.println("[StateMachine] OCCUPANCY → AWAY");
      _fireEvent({ PresenceEvent::LAST_DEPARTURE, "", "", millis(), epochNow });
      _fireEvent({ PresenceEvent::EVERYONE_AWAY,  "", "", millis(), epochNow });
    }
  }

  // ----------------------------------------------------------
  //  Event dispatch — fires all callbacks + stores in history
  // ----------------------------------------------------------
  void _updateAbsentMacs() {
    std::vector<uint8_t*> absentMacs;
    for (auto& dev : _devices) {
      if (dev.state == DeviceState::ABSENT) {
        absentMacs.push_back(dev.mac);
      }
    }
    updateAbsentMacs(absentMacs);
  }

  void _fireEvent(const PresenceEventData& ev) {
    // Deduplicate — suppress same event+device if it fired within last 5 minutes
    uint32_t now = ev.timestampEpoch > 0 ? ev.timestampEpoch : (millis() / 1000);
    if (_historyCount > 0) {
      for (int i = 0; i < _historyCount; i++) {
        int idx = (_historyHead - 1 - i + EVENT_HISTORY_SIZE) % EVENT_HISTORY_SIZE;
        const PresenceEventData& past = _history[idx];
        uint32_t pastTs = past.timestampEpoch > 0 ? past.timestampEpoch : 0;
        // Stop searching if event is older than 5 minutes
        if (pastTs > 0 && now > pastTs && (now - pastTs) > 300) break;
        if (past.event == ev.event && past.deviceId == ev.deviceId) {
          Serial.printf("[Event] %s device=%s (suppressed — fired within 5min)\n",
                        ev.eventName(),
                        ev.deviceId.isEmpty() ? "(global)" : ev.deviceId.c_str());
          // Don't fire callbacks either — prevents repeated C4/HA/MQTT events
          return;
        }
      }
    }
    // Store in ring buffer (newest at head)
    _history[_historyHead] = ev;
    _historyHead = (_historyHead + 1) % EVENT_HISTORY_SIZE;
    if (_historyCount < EVENT_HISTORY_SIZE) _historyCount++;

    Serial.printf("[Event] %s device=%s\n",
                  ev.eventName(),
                  ev.deviceId.isEmpty() ? "(global)" : ev.deviceId.c_str());

    for (auto& cb : _callbacks) {
      cb(ev);
    }
  }

  // ----------------------------------------------------------
  //  GPIO helpers
  // ----------------------------------------------------------
  void _setRelay(bool closed) {
    digitalWrite(PIN_RELAY, closed ? HIGH : LOW);
  }

  void _setLedWifi(bool on) {
    digitalWrite(PIN_LED_WIFI, on ? HIGH : LOW);
  }

  void _setLedOccupancy(bool on) {
    digitalWrite(PIN_LED_OCCUPANCY, on ? HIGH : LOW);
  }

  void _setLedFault(bool on) {
    digitalWrite(PIN_LED_FAULT, on ? HIGH : LOW);
  }

  // ----------------------------------------------------------
  //  Helpers
  // ----------------------------------------------------------
  bool _anyAbsent() const {
    for (const auto& d : _devices) {
      if (d.state == DeviceState::ABSENT) return true;
    }
    return false;
  }

  // Returns true only when ALL tracked devices are absent
  // Used to trigger fast polling for return detection
  bool _allAbsent() const {
    if (_devices.empty()) return false;
    for (const auto& d : _devices) {
      if (d.state != DeviceState::ABSENT) return false;
    }
    return true;
  }

  bool _anySuspect() const {
    for (const auto& d : _devices) {
      if (d.state == DeviceState::SUSPECT) return true;
    }
    return false;
  }

  static const char* _stateName(DeviceState s) {
    switch (s) {
      case DeviceState::UNKNOWN: return "UNKNOWN";
      case DeviceState::PRESENT: return "PRESENT";
      case DeviceState::SUSPECT: return "SUSPECT";
      case DeviceState::ABSENT:  return "ABSENT";
      default:                   return "?";
    }
  }
};
