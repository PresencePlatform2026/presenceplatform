#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "Config.h"
#include "StateMachine.h"
#include "NtpSync.h"
#include "MqttClient.h"
#include "TcpSocketServer.h"
#include "SharedTypes.h"
#include "NetworkScanner.h"
#include "AutoUpdater.h"
extern NetworkScanner g_scanner;
extern AutoUpdater g_updater;
extern MqttClient g_mqtt;
extern TcpSocketServer g_tcp;

// ============================================================
//  ApiHandler.h — REST API route registration
//  PresencePlatform v1.6.1
//
//  All endpoints:
//    GET  /api/v1/status          — global occupancy + all devices
//    GET  /api/v1/devices         — device list
//    POST /api/v1/devices         — add a device
//    DELETE /api/v1/devices/:id   — remove a device
//    GET  /api/v1/events          — last 50 events
//    GET  /api/v1/health          — uptime, heap, RSSI
//    POST /api/v1/poll            — force immediate poll
//    GET  /api/v1/config          — get platform config (no passwords)
//    POST /api/v1/config          — update platform config
//    GET  /api/v1/config/backup   — export full config JSON
//    POST /api/v1/config/restore  — import config JSON
//    GET  /changelog              — full changelog (no auth)
//    GET  /changelog/current      — latest version only (no auth)
//    POST /api/v1/auth/login      — get session token
// ============================================================

extern volatile uint32_t g_epochNow;

// RSSI history
extern RssiReading g_rssiHistory[];
extern int g_rssiHead;
extern int g_rssiCount;

// Simple session token store (single token, single dealer session)
static String s_sessionToken = "";
static uint32_t s_tokenIssuedMs = 0;
#define TOKEN_EXPIRY_MS  86400000UL  // 24 hours

static String _generateToken() {
  String t = "";
  for (int i = 0; i < 32; i++) {
    t += String((char)('a' + random(26)));
  }
  return t;
}

static bool _tokenValid(const String& token) {
  if (token.isEmpty() || s_sessionToken.isEmpty()) return false;
  if (token != s_sessionToken) return false;
  if ((millis() - s_tokenIssuedMs) > TOKEN_EXPIRY_MS) return false;
  return true;
}

static bool _isAuthorized(AsyncWebServerRequest* req) {
  if (!req->hasHeader("Authorization")) return false;
  String auth = req->getHeader("Authorization")->value();
  if (!auth.startsWith("Bearer ")) return false;
  return _tokenValid(auth.substring(7));
}

static void _sendUnauthorized(AsyncWebServerRequest* req) {
  req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
}

static void _sendError(AsyncWebServerRequest* req, int code, const String& msg) {
  req->send(code, "application/json",
            "{\"error\":\"" + msg + "\"}");
}

static void _setCorsHeaders(AsyncWebServerResponse* res) {
  res->addHeader("Access-Control-Allow-Origin", "*");
  res->addHeader("Access-Control-Allow-Methods", "GET,POST,DELETE,OPTIONS");
  res->addHeader("Access-Control-Allow-Headers", "Authorization,Content-Type");
}


class ApiHandler {
public:

  void registerRoutes(AsyncWebServer& server, Config& cfg, StateMachine& sm, NtpSync& ntp) {

    // ---- CORS preflight ----
    server.on("/*", HTTP_OPTIONS, [](AsyncWebServerRequest* req) {
      AsyncWebServerResponse* res = req->beginResponse(204);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- Auth ----
    server.on("/api/v1/auth/login", HTTP_POST, [](AsyncWebServerRequest* req){},
      nullptr,
      [&cfg, &ntp, &sm](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        Serial.printf("[Auth] POST body len=%d body=%.*s\n", (int)len, (int)len, (char*)data);
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
          Serial.println("[Auth] JSON parse failed");
          return _sendError(req, 400, "invalid_json");
        }
        String user = doc["username"] | "";
        String pass = doc["password"] | "";
        Serial.printf("[Auth] user='%s' pass='%s' expected_user='%s'\n",
                      user.c_str(), pass.c_str(), cfg.platform.adminUser.c_str());
        if (user != cfg.platform.adminUser || pass != cfg.platform.adminPassword) {
          return _sendError(req, 401, "invalid_credentials");
        }
        s_sessionToken  = _generateToken();
        s_tokenIssuedMs = millis();
        JsonDocument resp;
        resp["token"]      = s_sessionToken;
        resp["expires_ms"] = TOKEN_EXPIRY_MS;
        String out;
        serializeJson(resp, out);
        AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
        _setCorsHeaders(res);
        req->send(res);
      }
    );

    // ---- GET /api/v1/status ----
    server.on("/api/v1/status", HTTP_GET, [&sm](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      doc["occupancy"]    = sm.occupancyName();
      doc["anyone_home"]  = sm.anyoneHome();
      doc["device_count"] = sm.homeCount();
      doc["total_devices"]= (int)sm.devices().size();
      doc["timestamp"]    = g_epochNow;

      JsonArray devArr = doc["devices"].to<JsonArray>();
      for (const auto& d : sm.devices()) {
        JsonObject o = devArr.add<JsonObject>();
        o["id"]          = d.id;
        o["name"]        = d.friendlyName;
        o["state"]       = d.stateName();
        o["last_seen"]   = d.lastSeenEpoch;
        o["last_seen_ms"]= d.lastSeenMs;
        o["confidence"]  = d.lastConfidence;
        o["method"]      = d.methodName();
        o["miss_count"]      = d.missCount;
        o["miss_threshold"]  = d.missThreshold;
        o["added_epoch"]     = d.addedEpoch;
        o["ip"]          = d.ip.toString();
        o["mac"]         = d.macStr;
      }

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- GET /api/v1/devices ----
    server.on("/api/v1/devices", HTTP_GET, [&sm](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      for (const auto& d : sm.devices()) {
        JsonObject o = arr.add<JsonObject>();
        o["id"]            = d.id;
        o["name"]          = d.friendlyName;
        o["mac"]           = d.macStr;
        o["ip"]            = d.ip.toString();
        o["state"]         = d.stateName();
        o["miss_threshold"]= d.missThreshold;
        o["last_seen"]     = d.lastSeenEpoch;
        o["confidence"]    = d.lastConfidence;
        o["method"]        = d.methodName();
      }

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- POST /api/v1/devices ----
    server.on("/api/v1/devices", HTTP_POST, [](AsyncWebServerRequest* req){},
      nullptr,
      [&cfg, &sm](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);

        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok)
          return _sendError(req, 400, "invalid_json");

        Device dev;
        dev.id           = doc["id"]   | "";
        dev.friendlyName = doc["name"] | "";
        dev.macStr       = doc["mac"]  | "";
        String ipStr     = doc["ip"]   | "0.0.0.0";
        dev.missThreshold= doc["miss_threshold"] | DEFAULT_MISS_THRESHOLD;

        if (dev.id.isEmpty() || dev.macStr.isEmpty())
          return _sendError(req, 400, "id_and_mac_required");

        if (!Config::parseMac(dev.macStr, dev.mac))
          return _sendError(req, 400, "invalid_mac");

        dev.ip.fromString(ipStr);
        dev.addedEpoch = g_epochNow;

        sm.addDevice(dev);
        cfg.addDevice(dev);

        JsonDocument resp;
        resp["ok"]  = true;
        resp["id"]  = dev.id;
        String out;
        serializeJson(resp, out);
        AsyncWebServerResponse* res = req->beginResponse(201, "application/json", out);
        _setCorsHeaders(res);
        req->send(res);
      }
    );

    // ---- DELETE /api/v1/devices/:id ----
    server.on("/api/v1/devices", HTTP_DELETE,
      [&cfg, &sm](AsyncWebServerRequest* req) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);
        String path = req->url();
        String prefix = "/api/v1/devices/";
        if (!path.startsWith(prefix)) return _sendError(req, 400, "missing_id");
        String id = path.substring(prefix.length());
        id.trim();
        if (id.isEmpty()) return _sendError(req, 400, "missing_id");
        bool ok = sm.removeDevice(id);
        cfg.removeDevice(id);
        if (!ok) return _sendError(req, 404, "device_not_found");
        AsyncWebServerResponse* res = req->beginResponse(200, "application/json",
                                                         "{\"ok\":true}");
        _setCorsHeaders(res);
        req->send(res);
      }
    );

    // ---- GET /api/v1/events ----
    server.on("/api/v1/events", HTTP_GET, [&sm](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      int count = sm.eventHistoryCount();
      const PresenceEventData* hist = sm.eventHistory();
      for (int i = 0; i < count; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["event"]      = hist[i].eventName();
        o["device_id"]  = hist[i].deviceId;
        o["device_name"]= hist[i].deviceName;
        o["timestamp"]  = hist[i].timestampEpoch;
        o["timestamp_ms"]= hist[i].timestampMs;
      }

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- GET /api/v1/health ----
    server.on("/api/v1/health", HTTP_GET, [&ntp](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      doc["uptime_ms"]  = millis();
      doc["uptime_sec"] = millis() / 1000;
      doc["free_heap"]  = ESP.getFreeHeap();
      doc["min_heap"]   = ESP.getMinFreeHeap();
      doc["rssi"]       = WiFi.RSSI();
      doc["ip"]         = WiFi.localIP().toString();
      doc["mac"]        = WiFi.macAddress();
      doc["fw_version"]        = FIRMWARE_VERSION;
      doc["firmware_version"]  = FIRMWARE_VERSION;
      doc["version"]           = FIRMWARE_VERSION;
      doc["ntp_synced"]      = ntp.isSynced();
      doc["tcp_clients"]     = g_tcp.clientCount();
      doc["mqtt_enabled"]      = g_mqtt.isEnabled();
      doc["update_available"]  = g_updater.isUpdateAvailable();
      doc["latest_version"]    = g_updater.latestVersion();
      doc["mqtt_connected"]  = g_mqtt.isConnected();
      doc["current_time"]  = ntp.formattedTime();
      doc["epoch"]         = g_epochNow;

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- POST /api/v1/poll ----
    server.on("/api/v1/poll", HTTP_POST, [&sm](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);
      sm.forcePoll(g_epochNow);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json",
                                                       "{\"ok\":true}");
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- GET /api/v1/config ----
    server.on("/api/v1/config", HTTP_GET, [&cfg](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      doc["hostname"]          = cfg.platform.hostname;
      doc["miss_threshold"]    = cfg.platform.missThreshold;
      doc["poll_interval_sec"] = cfg.platform.pollIntervalSec;
      doc["tcp_port"]          = cfg.platform.tcpPort;
      doc["mqtt_enabled"]      = cfg.platform.mqttEnabled;
      doc["mqtt_broker"]       = cfg.platform.mqttBroker;
      doc["mqtt_port"]         = cfg.platform.mqttPort;
      doc["mqtt_topic_root"]   = cfg.platform.mqttTopicRoot;
      doc["mqtt_user"]         = cfg.platform.mqttUser;
      doc["timezone"]          = cfg.platform.timezone;

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- POST /api/v1/config ----
    server.on("/api/v1/config", HTTP_POST, [](AsyncWebServerRequest* req){},
      nullptr,
      [&cfg, &ntp, &sm](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);

        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok)
          return _sendError(req, 400, "invalid_json");

        if (doc["hostname"].is<String>())
          cfg.platform.hostname = doc["hostname"].as<String>();
        if (doc["miss_threshold"].is<int>()) {
          cfg.platform.missThreshold = doc["miss_threshold"].as<int>();
          sm.setGlobalMissThreshold(cfg.platform.missThreshold);
        }
        if (doc["poll_interval_sec"].is<int>())
          cfg.platform.pollIntervalSec = doc["poll_interval_sec"].as<int>();
        if (doc["tcp_port"].is<int>())
          cfg.platform.tcpPort = doc["tcp_port"].as<int>();
      if (doc["http_port"].is<int>())
          cfg.platform.httpPort = doc["http_port"].as<int>();
      if (doc["update_window_start"].is<int>())
          cfg.platform.updateWindowStart = doc["update_window_start"].as<int>();
      if (doc["update_window_end"].is<int>())
          cfg.platform.updateWindowEnd = doc["update_window_end"].as<int>();
        if (!doc["mqtt_enabled"].isNull())
          cfg.platform.mqttEnabled = (bool)doc["mqtt_enabled"].as<int>() || doc["mqtt_enabled"].as<bool>();
        if (doc["mqtt_broker"].is<String>())
          cfg.platform.mqttBroker = doc["mqtt_broker"].as<String>();
        if (doc["mqtt_port"].is<int>())
          cfg.platform.mqttPort = doc["mqtt_port"].as<int>();
        if (doc["mqtt_topic_root"].is<String>())
          cfg.platform.mqttTopicRoot = doc["mqtt_topic_root"].as<String>();
        if (doc["mqtt_user"].is<String>())
          cfg.platform.mqttUser = doc["mqtt_user"].as<String>();
        if (doc["mqtt_password"].is<String>() &&
            !doc["mqtt_password"].as<String>().isEmpty())
          cfg.platform.mqttPassword = doc["mqtt_password"].as<String>();
        if (!doc["auto_update"].isNull()) {
          cfg.platform.autoUpdateEnabled = doc["auto_update"].as<bool>();
          g_updater.setAutoUpdate(cfg.platform.autoUpdateEnabled);
        }
        if (doc["timezone"].is<String>()) {
          cfg.platform.timezone = doc["timezone"].as<String>();
          ntp.setTimezone(cfg.platform.timezone.c_str());
        }
        // WiFi reset — clear SSID so device boots into portal
        if (!doc["wifi_reset"].isNull() && doc["wifi_reset"].as<bool>()) {
          cfg.platform.wifiSSID = "";
        }
        if (doc["admin_password"].is<String>() &&
            doc["admin_password"].as<String>().length() >= 8)
          cfg.platform.adminPassword = doc["admin_password"].as<String>();

        cfg.save();

        AsyncWebServerResponse* res = req->beginResponse(200, "application/json",
                                                         "{\"ok\":true}");
        _setCorsHeaders(res);
        req->send(res);
      }
    );

    // ---- GET /api/v1/config/backup ----
    server.on("/api/v1/config/backup", HTTP_GET, [&cfg](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);
      String json = cfg.exportJson();
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", json);
      res->addHeader("Content-Disposition",
                     "attachment; filename=\"presence_backup.json\"");
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- POST /api/v1/config/restore ----
    server.on("/api/v1/config/restore", HTTP_POST, [](AsyncWebServerRequest* req){},
      nullptr,
      [&cfg, &ntp, &sm](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);
        String json = String((char*)data, len);
        bool ok = cfg.importJson(json);
        AsyncWebServerResponse* res = req->beginResponse(
          ok ? 200 : 400, "application/json",
          ok ? "{\"ok\":true,\"message\":\"restore_complete_reboot_recommended\"}"
             : "{\"error\":\"invalid_backup_file\"}"
        );
        _setCorsHeaders(res);
        req->send(res);
      }
    );

    // ---- POST /api/v1/update/check ----
    server.on("/api/v1/update/check", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);
      // Trigger immediate check
      g_updater.bCheckNow = true;
      // Return current known state immediately
      JsonDocument resp;
      resp["ok"]               = true;
      resp["update_available"] = g_updater.isUpdateAvailable();
      resp["latest_version"]   = g_updater.latestVersion();
      resp["current_version"]  = FIRMWARE_VERSION;
      String out; serializeJson(resp, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- POST /api/v1/update/install ----
    server.on("/api/v1/update/install", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json",
                                                       "{\"ok\":true}");
      _setCorsHeaders(res);
      req->send(res);
      // Trigger update after response is sent
      delay(200);
      g_updater.installUpdate();
    });

    // ---- POST /api/v1/reboot ----
    server.on("/api/v1/reboot", HTTP_POST, [](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json",
                                                       "{\"ok\":true,\"message\":\"rebooting\"}");
      _setCorsHeaders(res);
      req->send(res);
      delay(500);
      ESP.restart();
    });

    // ---- GET /api/v1/network/scan ----
    // Returns cached ARP scan results, triggers background refresh
    server.on("/api/v1/network/scan", HTTP_GET, [&sm](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      // Trigger a fresh scan
      g_scanner.triggerScan();

      // Return cached results (may be empty on very first request)
      auto devices = g_scanner.getDevices();

      JsonDocument doc;
      JsonArray arr = doc["devices"].to<JsonArray>();

      for (const auto& d : devices) {
        // Skip our own IP
        if (d.ip == WiFi.localIP().toString()) continue;

        // Check if tracked
        bool tracked = false;
        String trackedId = "", trackedName = "";
        for (const auto& dev : sm.devices()) {
          String tm = dev.macStr; tm.toUpperCase();
          String dm = d.mac; dm.toUpperCase();
          if (tm == dm) {
            tracked = true;
            trackedId = dev.id;
            trackedName = dev.friendlyName;
            break;
          }
        }

        JsonObject o = arr.add<JsonObject>();
        o["mac"]          = d.mac;
        o["ip"]           = d.ip;
        o["tracked"]      = tracked;
        o["tracked_id"]   = trackedId;
        o["tracked_name"] = trackedName;
        o["likely_phone"] = d.likelyPhone;
      }

      // Add scan status
      JsonDocument resp;
      resp["devices"]    = arr;
      resp["scan_ready"] = g_scanner.isScanReady();
      resp["scan_age"]   = g_scanner.lastScanAge();

      String out;
      serializeJson(resp, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- GET /api/v1/rssi/history ----    // ---- GET /api/v1/rssi/history ----    // ---- GET /api/v1/rssi/history ----
    server.on("/api/v1/rssi/history", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (!_isAuthorized(req)) return _sendUnauthorized(req);

      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();

      // Return readings in chronological order
      int start = (g_rssiCount < RSSI_HISTORY_SIZE)
                  ? 0
                  : g_rssiHead;

      for (int i = 0; i < g_rssiCount; i++) {
        int idx = (start + i) % RSSI_HISTORY_SIZE;
        JsonObject o = arr.add<JsonObject>();
        o["ts"]   = g_rssiHistory[idx].ts;
        o["rssi"] = g_rssiHistory[idx].rssi;
      }

      String out;
      serializeJson(doc, out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- GET /changelog  (no auth — public) ----
    server.on("/changelog", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (LittleFS.exists("/changelog.json")) {
        AsyncWebServerResponse* res = req->beginResponse(
          LittleFS, "/changelog.json", "application/json");
        _setCorsHeaders(res);
        req->send(res);
      } else {
        req->send(404, "application/json", "{\"error\":\"changelog_not_found\"}");
      }
    });

    // ---- GET /changelog/current  (no auth — public) ----
    server.on("/changelog/current", HTTP_GET, [](AsyncWebServerRequest* req) {
      if (!LittleFS.exists("/changelog.json")) {
        return req->send(404, "application/json", "{\"error\":\"not_found\"}");
      }
      File f = LittleFS.open("/changelog.json", "r");
      String raw = f.readString();
      f.close();

      JsonDocument doc;
      if (deserializeJson(doc, raw) != DeserializationError::Ok)
        return req->send(500, "application/json", "{\"error\":\"parse_error\"}");

      JsonArray arr = doc.as<JsonArray>();
      if (arr.size() == 0)
        return req->send(404, "application/json", "{\"error\":\"empty\"}");

      String out;
      serializeJson(arr[0], out);
      AsyncWebServerResponse* res = req->beginResponse(200, "application/json", out);
      _setCorsHeaders(res);
      req->send(res);
    });

    // ---- Serve GUI from LittleFS ----
    server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html");

    // ---- 404 fallback ----
    server.onNotFound([](AsyncWebServerRequest* req) {
      if (req->method() == HTTP_OPTIONS) {
        AsyncWebServerResponse* res = req->beginResponse(204);
        _setCorsHeaders(res);
        req->send(res);
        return;
      }
      req->send(404, "application/json", "{\"error\":\"not_found\"}");
    });
  }
};
