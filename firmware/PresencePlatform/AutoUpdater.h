#pragma once
#include <Arduino.h>
#include "esp_task_wdt.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>

// ============================================================
//  AutoUpdater.h — Automatic OTA updates from GitHub
//  PresencePlatform v2.3.0
//
//  Checks version.json from GitHub on boot and every 24 hours.
//  If a newer version is available:
//    - Auto mode ON:  downloads and flashes silently
//    - Auto mode OFF: sets update available flag for GUI banner
//
//  GitHub URL format:
//    https://raw.githubusercontent.com/PresencePlatform2026/
//    presenceplatform/main/firmware/version.json
// ============================================================

#define UPDATE_CHECK_INTERVAL_MS  86400000UL  // 24 hours
#define UPDATE_VERSION_URL  "https://raw.githubusercontent.com/PresencePlatform2026/presenceplatform/main/firmware/version.json"

class AutoUpdater {
public:
  bool bCheckNow = false;

  void init(bool autoUpdate) {
    _autoUpdate     = autoUpdate;
    _updateAvailable = false;
    _latestVersion  = "";
    _firmwareUrl    = "";
    _fsUrl          = "";
    _lastCheckMs    = 0;
    _checking       = false;

    Serial.printf("[Updater] Auto-update: %s\n",
                  autoUpdate ? "enabled" : "disabled");
  }

  // Callback fired before rebooting for update
  std::function<void()> onBeforeUpdate = nullptr;

  bool isWithinUpdateWindow(int currentHour, int windowStart, int windowEnd) {
    if (windowStart == windowEnd) return true;
    if (windowStart < windowEnd) {
      return currentHour >= windowStart && currentHour < windowEnd;
    }
    return currentHour >= windowStart || currentHour < windowEnd;
  }

  // Call from main loop — checks on boot and every 24h
  void tick() {
    if (_checking) return;
    uint32_t now = millis();
    bool shouldCheck = bCheckNow || (_lastCheckMs == 0) ||
                       ((now - _lastCheckMs) > UPDATE_CHECK_INTERVAL_MS);
    bCheckNow = false;
    if (shouldCheck && WiFi.status() == WL_CONNECTED) {
      _checkForUpdate();
    }

    // Auto-install if update available and within update window
    if (_autoUpdate && _updateAvailable && !_installing && !_checking) {
      // Only install during configured update window
      time_t now = time(nullptr);
      struct tm* t = localtime(&now);
      int hour = t->tm_hour;
      if (_windowStart >= 0 && _windowEnd >= 0) {
        if (isWithinUpdateWindow(hour, _windowStart, _windowEnd)) {
          Serial.printf("[Updater] Within update window (%d:00-%d:00) — installing\n",
                        _windowStart, _windowEnd);
          // Notify TCP clients before rebooting
          if (onBeforeUpdate) onBeforeUpdate();
          delay(2000); // give TCP time to send notification
          installUpdate();
        }
      }
    }
  }

  bool isUpdateAvailable()  const { return _updateAvailable; }
  String latestVersion()    const { return _latestVersion; }
  bool isAutoUpdate()       const { return _autoUpdate; }
  void setAutoUpdate(bool v)      { _autoUpdate = v; }
  void setUpdateWindow(int start, int end) {
    _windowStart = start;
    _windowEnd   = end;
  }
  void triggerCheck() { bCheckNow = true; }

  // Called from GUI "Install Update" button
  void installUpdate() {
    if (!_updateAvailable || _installing) return;
    _installing = true;
    Serial.println("[Updater] Manual install triggered.");
    _doUpdate();
  }

private:
  bool     _autoUpdate      = false;
  bool     _updateAvailable = false;
  bool     _checking        = false;
  bool     _installing      = false;
  int      _windowStart     = 2;
  int      _windowEnd       = 4;
  String   _latestVersion   = "";
  String   _firmwareUrl     = "";
  String   _fsUrl           = "";
  uint32_t _lastCheckMs     = 0;

  // ----------------------------------------------------------
  //  Check version.json from GitHub
  // ----------------------------------------------------------
  void _checkForUpdate() {
    _checking    = true;
    _lastCheckMs = millis();

    Serial.println("[Updater] Checking for updates...");

    // Add timestamp to URL to bust GitHub CDN cache
    String url = String(UPDATE_VERSION_URL) + "?t=" + String(millis());
    HTTPClient http;
    http.begin(url);
    http.setTimeout(10000);
    http.addHeader("Cache-Control", "no-cache");
    http.addHeader("Pragma", "no-cache");
    int code = http.GET();

    if (code != 200) {
      Serial.printf("[Updater] Version check failed. HTTP %d\n", code);
      http.end();
      _checking = false;
      return;
    }

    String body = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);
    if (err) {
      Serial.printf("[Updater] JSON parse error: %s\n", err.c_str());
      _checking = false;
      return;
    }

    String latestVer  = doc["version"]      | "";
    String fwUrl      = doc["firmware_url"] | "";
    String fsUrl      = doc["fs_url"]       | "";
    String currentVer = FIRMWARE_VERSION;

    Serial.printf("[Updater] Current: %s  Latest: %s\n",
                  currentVer.c_str(), latestVer.c_str());

    if (latestVer.isEmpty() || latestVer == currentVer) {
      Serial.println("[Updater] Already up to date.");
      _updateAvailable = false;
      _checking = false;
      return;
    }

    // Newer version available
    _updateAvailable = true;
    _latestVersion   = latestVer;
    _firmwareUrl     = fwUrl;
    _fsUrl           = fsUrl;

    Serial.printf("[Updater] Update available: v%s\n", latestVer.c_str());

    if (_autoUpdate) {
      Serial.println("[Updater] Auto-update enabled — installing...");
      _doUpdate();
    }

    _checking = false;
  }

  // ----------------------------------------------------------
  //  Download and flash firmware + filesystem
  // ----------------------------------------------------------
  void _doUpdate() {
    if (_firmwareUrl.isEmpty()) {
      Serial.println("[Updater] No firmware URL in version.json");
      return;
    }

    // Disable task watchdog during download — HTTP can take several seconds
    esp_task_wdt_delete(NULL);
    Serial.println("[Updater] Watchdog suspended for update.");

    // Flash filesystem first (if URL provided)
    if (!_fsUrl.isEmpty()) {
      Serial.println("[Updater] Flashing filesystem...");
      HTTPClient fsHttp;
      fsHttp.begin(_fsUrl);
      t_httpUpdate_return ret = httpUpdate.updateSpiffs(fsHttp, _fsUrl);
      fsHttp.end();
      if (ret == HTTP_UPDATE_OK) {
        Serial.println("[Updater] Filesystem updated.");
      } else {
        Serial.printf("[Updater] Filesystem update failed: %s — skipping.\n",
                      httpUpdate.getLastErrorString().c_str());
      }
    }

    // Flash firmware
    Serial.println("[Updater] Flashing firmware...");
    HTTPClient fwHttp;
    fwHttp.begin(_firmwareUrl);
    httpUpdate.rebootOnUpdate(false);  // we control the reboot
    t_httpUpdate_return ret = httpUpdate.update(fwHttp, _firmwareUrl);
    fwHttp.end();

    if (ret == HTTP_UPDATE_OK) {
      Serial.println("[Updater] Firmware updated successfully — rebooting.");
      delay(1000);
      ESP.restart();
    } else {
      Serial.printf("[Updater] Firmware update failed: %s\n",
                    httpUpdate.getLastErrorString().c_str());
      Serial.println("[Updater] Staying online — will retry on next check.");
      // Re-add this task to watchdog now that download is done
      esp_task_wdt_add(NULL);
      _updateAvailable = true;
    }
  }
};
