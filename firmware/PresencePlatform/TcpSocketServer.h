#pragma once
#include "SharedTypes.h"
#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include "StateMachine.h"
#include "Events.h"

// ============================================================
//  TcpSocketServer.h — Persistent TCP socket for Control4
//  PresencePlatform v1.2.0
//
//  Protocol: newline-delimited JSON on port 4999 (configurable)
//
//  ESP32 pushes on every state change:
//    { "type": "event",  "event": "first_arrival", "device_id": "jacob",
//      "device_name": "Jacob's iPhone", "timestamp": 0 }
//    { "type": "state",  "occupancy": "home", "anyone_home": true,
//      "device_count": 1, "devices": [...] }
//
//  Client can send:
//    POLL\n          — request full status immediately
//    PING\n          — keepalive, ESP32 replies PONG\n
//
//  Max 4 simultaneous connections.
//  Reconnects are handled by the client — ESP32 just accepts.
// ============================================================

#define TCP_MAX_CLIENTS     4
#define TCP_RX_BUFFER_SIZE  64
#define TCP_PORT_DEFAULT    4999

class TcpSocketServer {
public:

  // ----------------------------------------------------------
  //  init() — call from setup() after WiFi is connected
  // ----------------------------------------------------------
  void init(StateMachine& sm, int port = TCP_PORT_DEFAULT) {
    _sm   = &sm;
    _port = port;

    _server = new WiFiServer(port);
    _server->begin();
    _server->setNoDelay(true);

    Serial.printf("[TCP] Socket server started on port %d\n", port);
  }

  // ----------------------------------------------------------
  //  tick() — call from the TCP task loop every ~50ms
  //  Accepts new connections, reads incoming commands,
  //  cleans up disconnected clients.
  // ----------------------------------------------------------
  void tick() {
    // Accept new connections
    if (_server->hasClient()) {
      WiFiClient incoming = _server->accept();
      bool slotFound = false;
      for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
        if (!_clients[i] || !_clients[i].connected()) {
          _clients[i] = incoming;
          slotFound = true;
          Serial.printf("[TCP] Client connected: %s  (slot %d)\n",
                        incoming.remoteIP().toString().c_str(), i);
          // Send current state immediately on connect
          _pushState(_clients[i]);
          break;
        }
      }
      if (!slotFound) {
        // All slots full — reject
        incoming.println("{\"error\":\"max_clients_reached\"}");
        incoming.stop();
        Serial.println("[TCP] Connection rejected — max clients reached.");
      }
    }

    // Service each connected client
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
      if (!_clients[i]) continue;

      if (!_clients[i].connected()) {
        Serial.printf("[TCP] Client disconnected (slot %d)\n", i);
        _clients[i].stop();
        continue;
      }

      // Read incoming data
      while (_clients[i].available()) {
        char c = _clients[i].read();
        if (c == '\n' || c == '\r') {
          if (_rxLen > 0) {
            _rxBuf[_rxLen] = '\0';
            _handleCommand(i, String(_rxBuf));
            _rxLen = 0;
          }
        } else if (_rxLen < TCP_RX_BUFFER_SIZE - 1) {
          _rxBuf[_rxLen++] = c;
        }
      }
    }
  }

  // ----------------------------------------------------------
  //  pushEvent() — broadcast a presence event to all clients
  //  Called from the StateMachine event callback
  // ----------------------------------------------------------
  void pushEvent(const PresenceEventData& ev) {
    JsonDocument doc;
    doc["type"]        = "event";
    doc["event"]       = ev.eventName();
    doc["device_id"]   = ev.deviceId;
    doc["device_name"] = ev.deviceName;
    doc["timestamp"]   = ev.timestampEpoch;
    doc["timestamp_ms"]= ev.timestampMs;

    String msg;
    serializeJson(doc, msg);
    msg += "\n";

    _broadcast(msg);
  }

  // ----------------------------------------------------------
  // Broadcast arbitrary JSON to all connected TCP clients
  void broadcastJson(const String& msg) { _broadcast(msg); }

  //  pushState() — broadcast full occupancy state to all clients
  //  Called after every state change
  // ----------------------------------------------------------
  void pushState() {
    if (!_sm) return;
    String msg = _buildStateJson();
    msg += "\n";
    _broadcast(msg);
  }

  int clientCount() {
    int n = 0;
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
      if (_clients[i] && _clients[i].connected()) n++;
    }
    return n;
  }


private:

  WiFiServer*  _server   = nullptr;
  StateMachine* _sm      = nullptr;
  int           _port    = TCP_PORT_DEFAULT;
  WiFiClient    _clients[TCP_MAX_CLIENTS];

  // Per-client receive buffer (shared — only one command at a time)
  char _rxBuf[TCP_RX_BUFFER_SIZE];
  int  _rxLen = 0;

  // ----------------------------------------------------------
  //  Handle an incoming command from a client
  // ----------------------------------------------------------
  void _handleCommand(int slot, const String& cmd) {
    String c = cmd;
    c.trim();
    c.toUpperCase();

    Serial.printf("[TCP] Command from slot %d: %s\n", slot, c.c_str());

    if (c == "POLL") {
      // Send full current state
      _pushState(_clients[slot]);

    } else if (c == "PING") {
      _clients[slot].println("PONG");
    } else if (c == "VERSION") {
      // Return firmware version over TCP — no HTTP auth required
      String msg = "{\"type\":\"version\",\"version\":\"" + String(FIRMWARE_VERSION) + "\"}";
      _clients[slot].println(msg);

    } else if (c.startsWith("{")) {
      // JSON command — reserved for future use
      // e.g. { "cmd": "set_threshold", "device": "jacob", "value": 3 }
      _clients[slot].println("{\"error\":\"json_commands_not_yet_supported\"}");

    } else {
      _clients[slot].println("{\"error\":\"unknown_command\"}");
    }
  }

  // ----------------------------------------------------------
  //  Send current full state to one client
  // ----------------------------------------------------------
  void _pushState(WiFiClient& client) {
    if (!client.connected() || !_sm) return;
    String msg = _buildStateJson();
    msg += "\n";
    client.print(msg);
  }

  // ----------------------------------------------------------
  //  Broadcast a message to all connected clients
  // ----------------------------------------------------------
  void _broadcast(const String& msg) {
    for (int i = 0; i < TCP_MAX_CLIENTS; i++) {
      if (_clients[i] && _clients[i].connected()) {
        _clients[i].print(msg);
      }
    }
  }

  // ----------------------------------------------------------
  //  Build the full state JSON string
  // ----------------------------------------------------------
  String _buildStateJson() {
    JsonDocument doc;
    doc["type"]            = "state";
    doc["occupancy"]       = _sm->occupancyName();
    doc["anyone_home"]     = _sm->anyoneHome();
    doc["device_count"]    = _sm->homeCount();
    doc["total_devices"]   = (int)_sm->devices().size();
    doc["uptime_ms"]       = millis();
    doc["firmware_version"]= FIRMWARE_VERSION;

    JsonArray arr = doc["devices"].to<JsonArray>();
    for (const auto& d : _sm->devices()) {
      JsonObject o = arr.add<JsonObject>();
      o["id"]         = d.id;
      o["name"]       = d.friendlyName;
      o["state"]      = d.stateName();
      o["last_seen"]  = d.lastSeenEpoch;
      o["confidence"] = d.lastConfidence;
      o["method"]     = d.methodName();
      o["ip"]         = d.ip.toString();
      o["mac"]        = d.macStr;
    }

    String out;
    serializeJson(doc, out);
    return out;
  }
};
