#pragma once
#include <Arduino.h>

// ============================================================
//  Events.h — Presence event definitions
//  PresencePlatform v1.0.0
// ============================================================

enum class PresenceEvent {
  // Global occupancy
  FIRST_ARRIVAL,      // First device home, house was empty
  LAST_DEPARTURE,     // Last device left, house now empty
  ANYONE_HOME,        // Alias for FIRST_ARRIVAL (semantic clarity in drivers)
  EVERYONE_AWAY,      // Alias for LAST_DEPARTURE

  // Per-device
  PERSON_ARRIVED,     // A specific person arrived (may not be first)
  PERSON_DEPARTED,    // A specific person departed (may not be last)

  // System
  COMM_ERROR,         // ESP32 internal fault (used by drivers monitoring health)
  COMM_RESTORED,      // Fault cleared
  DEVICE_ADDED,       // New device registered at runtime
  DEVICE_REMOVED,     // Device removed at runtime
};

struct PresenceEventData {
  PresenceEvent event;
  String        deviceId;     // Empty for global events
  String        deviceName;   // Friendly name, empty for global events
  uint32_t      timestampMs;  // millis()
  uint32_t      timestampEpoch; // Unix time (0 if NTP not synced)

  const char* eventName() const {
    switch (event) {
      case PresenceEvent::FIRST_ARRIVAL:   return "first_arrival";
      case PresenceEvent::LAST_DEPARTURE:  return "last_departure";
      case PresenceEvent::ANYONE_HOME:     return "anyone_home";
      case PresenceEvent::EVERYONE_AWAY:   return "everyone_away";
      case PresenceEvent::PERSON_ARRIVED:  return "person_arrived";
      case PresenceEvent::PERSON_DEPARTED: return "person_departed";
      case PresenceEvent::COMM_ERROR:      return "comm_error";
      case PresenceEvent::COMM_RESTORED:   return "comm_restored";
      case PresenceEvent::DEVICE_ADDED:    return "device_added";
      case PresenceEvent::DEVICE_REMOVED:  return "device_removed";
      default:                             return "unknown";
    }
  }
};

// Callback type — implement this in your platform layer to receive events
using PresenceEventCallback = std::function<void(const PresenceEventData&)>;
