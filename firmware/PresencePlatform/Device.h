#pragma once
#include <Arduino.h>

// ============================================================
//  Device.h — Single tracked device model
//  PresencePlatform v1.0.0
// ============================================================

// How many consecutive missed polls before ABSENT is declared
#define DEFAULT_MISS_THRESHOLD     10  // 10 misses × 60s = ~10 min departure

// During departure confirmation, poll this device every N ms
#define DEPARTURE_FAST_POLL_MS     5000UL    // 5 seconds — sub-10s return detection

// How long to stay in fast-poll mode after first miss
#define DEPARTURE_FAST_POLL_DUR_MS 600000UL  // 10 minutes

// Minimum confidence to count as "seen" this cycle
#define CONFIDENCE_THRESHOLD       40
// Minimum confidence to return from ABSENT
// 70 = passive traffic OK since passive wake already confirms device is active
#define ABSENT_RETURN_CONFIDENCE   70
// Consecutive hits required to confirm return from ABSENT (unused — single high-confidence hit sufficient)
#define ABSENT_RETURN_HITS         1

enum class DeviceState {
  UNKNOWN,   // Not yet polled since boot
  PRESENT,   // Confirmed on network
  SUSPECT,   // ARP missed — running confirmation polls
  ABSENT     // Miss threshold exhausted — declared away
};

enum class DetectionMethod {
  NONE,
  ARP,
  MDNS,
  PASSIVE,
  PING
};

struct DetectionResult {
  bool        seen;
  int         confidence;   // 0–100
  DetectionMethod method;
};

class Device {
public:
  // ---- Identity ----
  String  id;           // Internal key, e.g. "jacob"
  String  friendlyName; // "Jacob's iPhone"
  uint8_t mac[6];       // Parsed MAC bytes
  String  macStr;       // "AA:BB:CC:DD:EE:FF"
  IPAddress ip;

  // ---- State ----
  DeviceState state        = DeviceState::UNKNOWN;
  int         returnHits   = 0;   // consecutive hits needed to confirm return from ABSENT
  int         missCount    = 0;
  int         missThreshold = DEFAULT_MISS_THRESHOLD;
  uint32_t    lastSeenMs   = 0;   // millis() of last confirmed detection
  uint32_t    lastSeenEpoch = 0;  // Unix timestamp of last confirmed detection
  uint32_t    addedEpoch    = 0;  // Unix timestamp when device was added
  int         lastConfidence = 0;
  DetectionMethod lastMethod = DetectionMethod::NONE;

  // ---- Departure acceleration ----
  bool     fastPollActive  = false;
  uint32_t fastPollStartMs = 0;

  // ---- Helpers ----
  const char* stateName() const {
    switch (state) {
      case DeviceState::UNKNOWN: return "unknown";
      case DeviceState::PRESENT: return "present";
      case DeviceState::SUSPECT: return "suspect";
      case DeviceState::ABSENT:  return "absent";
      default:                   return "unknown";
    }
  }

  const char* methodName() const {
    switch (lastMethod) {
      case DetectionMethod::ARP:     return "arp";
      case DetectionMethod::MDNS:    return "mdns";
      case DetectionMethod::PASSIVE: return "passive";
      case DetectionMethod::PING:    return "ping";
      default:                       return "none";
    }
  }

  bool isHome() const {
    return state == DeviceState::PRESENT || state == DeviceState::SUSPECT;
  }

  // True if this device should be polled on the fast schedule
  bool needsFastPoll() const {
    if (!fastPollActive) return false;
    return (millis() - fastPollStartMs) < DEPARTURE_FAST_POLL_DUR_MS;
  }

  void startFastPoll() {
    fastPollActive  = true;
    fastPollStartMs = millis();
  }

  void stopFastPoll() {
    fastPollActive = false;
  }

  // Called when device is confirmed present
  // Returns true if state changed to PRESENT (triggers arrival event)
  bool markSeen(int confidence, DetectionMethod method, uint32_t epochNow) {
    lastSeenMs     = millis();
    lastSeenEpoch  = epochNow;
    lastConfidence = confidence;
    lastMethod     = method;

    if (state == DeviceState::ABSENT) {
      // Require high confidence to return from ABSENT
      // This prevents passive traffic (conf 75) from triggering false arrivals
      // A single ARP (conf 100) or mDNS (conf 90) hit is sufficient
      if (confidence >= ABSENT_RETURN_CONFIDENCE) {
        state      = DeviceState::PRESENT;
        missCount  = 0;
        returnHits = 0;
        stopFastPoll();
        return true; // confirmed return
      }
      // Low confidence hit — ignore completely when absent
      return false;
    }

    // Normal case — already present or unknown
    state      = DeviceState::PRESENT;
    missCount  = 0;
    returnHits = 0;
    stopFastPoll();
    return true;
  }

  // Called when a poll cycle finds nothing — returns true ONLY on first threshold crossing
  bool recordMiss() {
    // Already ABSENT — stop counting, no more events
    if (state == DeviceState::ABSENT) {
      return false;
    }
    missCount++;
    if (state == DeviceState::PRESENT) {
      state = DeviceState::SUSPECT;
      startFastPoll();
    }
    if (missCount >= missThreshold) {
      state = DeviceState::ABSENT;
      // Keep fast polling active after going ABSENT for quick return detection
      // Fast poll runs every 30s for 10 minutes, then falls back to normal 60s poll
      // This gives sub-30-second return detection right after departure
      startFastPoll();
      return true; // threshold crossed for the FIRST time
    }
    return false;
  }
};
