#pragma once
#include <Arduino.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <vector>
#include "Device.h"

// ============================================================
//  Config.h — NVS-backed configuration manager
//  PresencePlatform v1.0.0
//
//  Stores and loads:
//    - WiFi credentials
//    - Device list (MAC, IP, name, thresholds)
//    - Platform settings (TCP port, MQTT broker, etc.)
//    - Admin credentials
// ============================================================

#define NVS_NAMESPACE   "presence"
#define MAX_DEVICES     10

struct PlatformConfig {
  // Network
  String wifiSSID;
  String wifiPassword;
  String hostname         = "presence-platform";

  // API / security
  String adminUser        = "admin";
  String adminPassword    = "changeme";

  // Detection
  int    missThreshold    = DEFAULT_MISS_THRESHOLD;
  int    pollIntervalSec  = 180;

  // TCP socket
  int    tcpPort          = 4999;
  int    httpPort         = 8090;
  int    updateWindowStart = 2;   // hour 0-23 (2 = 2AM)
  int    updateWindowEnd   = 4;   // hour 0-23 (4 = 4AM)

  // MQTT (optional)
  bool   mqttEnabled      = false;
  bool   autoUpdateEnabled = false;
  String mqttBroker;
  int    mqttPort         = 1883;
  String mqttUser;
  String mqttPassword;
  String mqttTopicRoot    = "presence";

  // Time
  String timezone         = "CST6CDT,M3.2.0,M11.1.0";
};


class Config {
public:

  PlatformConfig platform;

  // ----------------------------------------------------------
  //  load() — read everything from NVS at boot
  // ----------------------------------------------------------
  bool load() {
    _prefs.begin(NVS_NAMESPACE, true); // read-only

    platform.wifiSSID       = _prefs.getString("wifi_ssid", "");
    platform.wifiPassword   = _prefs.getString("wifi_pass", "");
    platform.hostname       = _prefs.getString("hostname",  "presence-platform");
    platform.adminUser      = _prefs.getString("admin_user","admin");
    platform.adminPassword  = _prefs.getString("admin_pass","changeme");
    platform.missThreshold  = _prefs.getInt("miss_thresh", 10);
    platform.pollIntervalSec= _prefs.getInt("poll_sec",    180);
    platform.tcpPort        = _prefs.getInt("tcp_port",    4999);
    platform.httpPort          = _prefs.getInt("http_port",   8090);
    platform.updateWindowStart = _prefs.getInt("upd_start",  2);
    platform.updateWindowEnd   = _prefs.getInt("upd_end",    4);
    platform.mqttEnabled    = _prefs.getBool("mqtt_en",    false);
    platform.autoUpdateEnabled = _prefs.getBool("auto_upd", false);
    platform.mqttBroker     = _prefs.getString("mqtt_host","");
    platform.mqttPort       = _prefs.getInt("mqtt_port",   1883);
    platform.mqttUser       = _prefs.getString("mqtt_user","");
    platform.mqttPassword   = _prefs.getString("mqtt_pass","");
    platform.mqttTopicRoot  = _prefs.getString("mqtt_root","presence");
    platform.timezone       = _prefs.getString("timezone", "CST6CDT,M3.2.0,M11.1.0");

    _prefs.end();

    // Load device list from separate key (stored as JSON blob)
    _loadDevices();

    Serial.printf("[Config] Loaded. WiFi SSID: %s, %d device(s)\n",
                  platform.wifiSSID.c_str(), (int)_devices.size());
    return true;
  }

  // ----------------------------------------------------------
  //  save() — write platform config to NVS
  // ----------------------------------------------------------
  bool save() {
    _prefs.begin(NVS_NAMESPACE, false); // read-write

    _prefs.putString("wifi_ssid",  platform.wifiSSID);
    _prefs.putString("wifi_pass",  platform.wifiPassword);
    _prefs.putString("hostname",   platform.hostname);
    _prefs.putString("admin_user", platform.adminUser);
    _prefs.putString("admin_pass", platform.adminPassword);
    _prefs.putInt   ("miss_thresh",platform.missThreshold);
    _prefs.putInt   ("poll_sec",   platform.pollIntervalSec);
    _prefs.putInt   ("tcp_port",   platform.tcpPort);
    _prefs.putInt   ("http_port",  platform.httpPort);
    _prefs.putInt   ("upd_start",  platform.updateWindowStart);
    _prefs.putInt   ("upd_end",    platform.updateWindowEnd);
    _prefs.putBool  ("mqtt_en",    platform.mqttEnabled);
    _prefs.putBool  ("auto_upd",   platform.autoUpdateEnabled);
    _prefs.putString("mqtt_host",  platform.mqttBroker);
    _prefs.putInt   ("mqtt_port",  platform.mqttPort);
    _prefs.putString("mqtt_user",  platform.mqttUser);
    _prefs.putString("mqtt_pass",  platform.mqttPassword);
    _prefs.putString("mqtt_root",  platform.mqttTopicRoot);
    _prefs.putString("timezone",   platform.timezone);

    _prefs.end();
    _saveDevices();

    Serial.println("[Config] Saved.");
    return true;
  }

  // ----------------------------------------------------------
  //  Device list management
  // ----------------------------------------------------------
  const std::vector<Device>& getDevices() const { return _devices; }
  const std::vector<Device>& devices() const { return _devices; }

  bool addDevice(const Device& dev) {
    if (_devices.size() >= MAX_DEVICES) return false;
    _devices.push_back(dev);
    _saveDevices();
    return true;
  }

  bool removeDevice(const String& id) {
    for (auto it = _devices.begin(); it != _devices.end(); ++it) {
      if (it->id == id) {
        _devices.erase(it);
        _saveDevices();
        return true;
      }
    }
    return false;
  }

  bool updateDevice(const Device& dev) {
    for (auto& d : _devices) {
      if (d.id == dev.id) {
        d = dev;
        _saveDevices();
        return true;
      }
    }
    return false;
  }

  // ----------------------------------------------------------
  //  Backup / restore (JSON blob for export via web GUI)
  // ----------------------------------------------------------
  String exportJson() {
    JsonDocument doc;
    doc["version"]          = "1.0.0";
    doc["hostname"]         = platform.hostname;
    doc["miss_threshold"]   = platform.missThreshold;
    doc["poll_interval_sec"]= platform.pollIntervalSec;
    doc["tcp_port"]         = platform.tcpPort;
    doc["http_port"]           = platform.httpPort;
    doc["update_window_start"] = platform.updateWindowStart;
    doc["update_window_end"]   = platform.updateWindowEnd;
    doc["mqtt_enabled"]     = platform.mqttEnabled;
      doc["auto_update"]      = platform.autoUpdateEnabled;
    doc["mqtt_broker"]      = platform.mqttBroker;
    doc["mqtt_port"]        = platform.mqttPort;
    doc["mqtt_topic_root"]  = platform.mqttTopicRoot;
    doc["timezone"]         = platform.timezone;

    JsonArray devArr = doc["devices"].to<JsonArray>();
    for (const auto& d : _devices) {
      JsonObject o = devArr.add<JsonObject>();
      o["id"]           = d.id;
      o["friendly_name"]= d.friendlyName;
      o["mac"]          = d.macStr;
      o["ip"]           = d.ip.toString();
      o["miss_threshold"]= d.missThreshold;
      o["added_epoch"]   = d.addedEpoch;
    }

    String out;
    serializeJsonPretty(doc, out);
    return out;
  }

  bool importJson(const String& json) {
    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return false;

    platform.hostname        = doc["hostname"]         | platform.hostname;
    platform.missThreshold   = doc["miss_threshold"]   | platform.missThreshold;
    platform.pollIntervalSec = doc["poll_interval_sec"]| platform.pollIntervalSec;
    platform.tcpPort         = doc["tcp_port"]         | platform.tcpPort;
    platform.mqttEnabled     = doc["mqtt_enabled"]     | platform.mqttEnabled;
      platform.autoUpdateEnabled = doc["auto_update"]      | platform.autoUpdateEnabled;
    platform.mqttBroker      = doc["mqtt_broker"]      | platform.mqttBroker.c_str();
    platform.mqttPort        = doc["mqtt_port"]        | platform.mqttPort;
    platform.mqttTopicRoot   = doc["mqtt_topic_root"]  | platform.mqttTopicRoot.c_str();
    if (doc["timezone"].is<String>())
      platform.timezone = doc["timezone"].as<String>();

    _devices.clear();
    JsonArray arr = doc["devices"].as<JsonArray>();
    for (JsonObject o : arr) {
      Device d;
      d.id           = o["id"]            | "";
      d.friendlyName = o["friendly_name"] | "";
      d.macStr       = o["mac"]           | "";
      _parseMac(d.macStr, d.mac);
      d.ip.fromString(o["ip"] | "0.0.0.0");
      d.missThreshold= o["miss_threshold"] | DEFAULT_MISS_THRESHOLD;
      d.addedEpoch   = o["added_epoch"]    | 0;
      if (d.id.length() && d.macStr.length()) {
        _devices.push_back(d);
      }
    }

    save();
    Serial.printf("[Config] Imported. %d device(s) restored.\n",
                  (int)_devices.size());
    return true;
  }

  // ----------------------------------------------------------
  //  MAC parsing utility (shared with web handler)
  // ----------------------------------------------------------
  static bool parseMac(const String& macStr, uint8_t* out) {
    return _parseMac(macStr, out);
  }


private:

  Preferences          _prefs;
  std::vector<Device>  _devices;

  void _loadDevices() {
    _prefs.begin(NVS_NAMESPACE, true);
    String json = _prefs.getString("devices_json", "[]");
    _prefs.end();

    JsonDocument doc;
    if (deserializeJson(doc, json) != DeserializationError::Ok) return;

    _devices.clear();
    for (JsonObject o : doc.as<JsonArray>()) {
      Device d;
      d.id            = o["id"]             | "";
      d.friendlyName  = o["friendly_name"]  | "";
      d.macStr        = o["mac"]            | "";
      _parseMac(d.macStr, d.mac);
      d.ip.fromString(o["ip"] | "0.0.0.0");
      d.missThreshold = o["miss_threshold"] | DEFAULT_MISS_THRESHOLD;
      if (d.id.length()) _devices.push_back(d);
    }
  }

  void _saveDevices() {
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    for (const auto& d : _devices) {
      JsonObject o = arr.add<JsonObject>();
      o["id"]             = d.id;
      o["friendly_name"]  = d.friendlyName;
      o["mac"]            = d.macStr;
      o["ip"]             = d.ip.toString();
      o["miss_threshold"] = d.missThreshold;
    }
    String json;
    serializeJson(doc, json);

    _prefs.begin(NVS_NAMESPACE, false);
    _prefs.putString("devices_json", json);
    _prefs.end();
  }

  static bool _parseMac(const String& macStr, uint8_t* out) {
    // Accepts "AA:BB:CC:DD:EE:FF" or "AA-BB-CC-DD-EE-FF"
    String s = macStr;
    s.replace("-", ":");
    if (s.length() < 17) return false;
    for (int i = 0; i < 6; i++) {
      out[i] = (uint8_t)strtoul(s.substring(i * 3, i * 3 + 2).c_str(), nullptr, 16);
    }
    return true;
  }
};
