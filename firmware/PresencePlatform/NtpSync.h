#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <time.h>

// ============================================================
//  NtpSync.h — Network time synchronization
//  PresencePlatform v1.6.1
// ============================================================

#define NTP_SYNC_TIMEOUT_MS   15000UL
#define NTP_RETRY_INTERVAL_MS 30000UL

extern volatile uint32_t g_epochNow;

class NtpSync {
public:

  void init(const char* tz = "CST6CDT,M3.2.0,M11.1.0") {
    _tz = tz;
    _synced = false;
    _lastAttemptMs = 0;

    // Set timezone FIRST before any configTime call
    setenv("TZ", _tz, 1);
    tzset();

    _attemptSync();
  }

  void tick() {
    if (_synced) {
      _updateEpoch();
      return;
    }
    if ((millis() - _lastAttemptMs) > NTP_RETRY_INTERVAL_MS) {
      _attemptSync();
    }
  }

  bool isSynced() const { return _synced; }

  String formattedTime() {
    if (!_synced) return "not synced";
    time_t now = (time_t)g_epochNow;
    struct tm* t = localtime(&now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    return String(buf);
  }

  void setTimezone(const char* tz) {
    _tz = tz;
    setenv("TZ", tz, 1);
    tzset();
    // Re-sync to apply new timezone immediately
    if (_synced) _updateEpoch();
    Serial.printf("[NTP] Timezone set to: %s\n", tz);
  }

  const char* timezone() const { return _tz; }


private:

  const char* _tz           = "CST6CDT,M3.2.0,M11.1.0";
  bool        _synced       = false;
  uint32_t    _lastAttemptMs = 0;

  void _attemptSync() {
    _lastAttemptMs = millis();
    Serial.println("[NTP] Attempting sync...");

    // Always set timezone before configTime
    setenv("TZ", _tz, 1);
    tzset();

    // Configure NTP servers
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com", "time.google.com");

    // Wait for valid time
    uint32_t start = millis();
    while ((millis() - start) < NTP_SYNC_TIMEOUT_MS) {
      time_t now;
      time(&now);
      if (now > 1000000000UL) {
        // Apply timezone to the synced time
        setenv("TZ", _tz, 1);
        tzset();
        _updateEpoch();
        _synced = true;
        Serial.printf("[NTP] Synced. Local: %s\n", formattedTime().c_str());
        return;
      }
      delay(200);
    }
    Serial.println("[NTP] Sync timeout — will retry.");
  }

  void _updateEpoch() {
    time_t now;
    time(&now);
    if (now > 1000000000UL) {
      g_epochNow = (uint32_t)now;
    }
  }
};
