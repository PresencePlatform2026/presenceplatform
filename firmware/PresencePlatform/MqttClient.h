#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "Config.h"
#include "StateMachine.h"
#include "Events.h"

// ============================================================
//  MqttClient.h — MQTT broker client
//  PresencePlatform v1.6.1
//
//  Topics published:
//    {root}/occupancy                    "home" or "away" (retained)
//    {root}/anyone_home                  "true" or "false" (retained)
//    {root}/device_count                 "1" (retained)
//    {root}/devices/{id}/state           "present" or "absent" (retained)
//    {root}/devices/{id}/confidence      "100" (retained)
//    {root}/devices/{id}/last_seen       epoch timestamp (retained)
//    {root}/events                       JSON event object (not retained)
//    {root}/health                       JSON health object (retained)
//
//  Topics subscribed:
//    {root}/cmd/poll                     trigger immediate poll
//    {root}/cmd/restart                  restart ESP32
//
//  Home Assistant auto-discovery:
//    homeassistant/binary_sensor/{id}/config   per-device presence
//    homeassistant/binary_sensor/occupancy/config  global occupancy
// ============================================================

#define MQTT_RECONNECT_INTERVAL_MS  15000UL  // 15s between reconnect attempts
#define MQTT_HEALTH_INTERVAL_MS     60000UL  // publish health every 60s
#define MQTT_KEEPALIVE_SEC          60

extern volatile uint32_t g_epochNow;

class MqttClient {
public:

  // ----------------------------------------------------------
  //  init() — call from setup() after WiFi is connected
  // ----------------------------------------------------------
  void init(Config& cfg, StateMachine& sm) {
    _cfg = &cfg;
    _sm  = &sm;

    if (!cfg.platform.mqttEnabled || cfg.platform.mqttBroker.isEmpty()) {
      Serial.println("[MQTT] Disabled or no broker configured.");
      return;
    }

    _enabled = true;
    _root    = cfg.platform.mqttTopicRoot;

    _createClient(cfg.platform.mqttBroker.c_str(), cfg.platform.mqttPort);

    Serial.printf("[MQTT] Configured. Broker: %s:%d  Root: %s\n",
                  cfg.platform.mqttBroker.c_str(),
                  cfg.platform.mqttPort,
                  _root.c_str());

    _connect();
  }

  // ----------------------------------------------------------
  //  tick() — call from a task loop every ~1s
  // ----------------------------------------------------------
  void tick() {
    if (!_enabled) return;

    // Heap warning
    if (ESP.getFreeHeap() < 50000) {
      Serial.printf("[MQTT] WARNING: Low heap %u bytes\n", ESP.getFreeHeap());
    }

    // Reconnect if needed
    if (!_client->connected()) {
      uint32_t now = millis();
      if ((now - _lastReconnectMs) > MQTT_RECONNECT_INTERVAL_MS) {
        _lastReconnectMs = now;
        // Feed watchdog before potentially blocking connect call
        vTaskDelay(pdMS_TO_TICKS(10));
        _connect();
        vTaskDelay(pdMS_TO_TICKS(10));
      }
      return;
    }

    if (_client) _client->loop();

    // Periodic health publish
    uint32_t now = millis();
    if ((now - _lastHealthMs) > MQTT_HEALTH_INTERVAL_MS) {
      _lastHealthMs = now;
      _publishHealth();
    }
  }

  // ----------------------------------------------------------
  //  publishEvent() — call from StateMachine event callback
  // ----------------------------------------------------------
  void publishEvent(const PresenceEventData& ev) {
    if (!_enabled || !_client || !_client->connected()) return;

    // Publish global occupancy state on occupancy-changing events
    if (ev.event == PresenceEvent::FIRST_ARRIVAL  ||
        ev.event == PresenceEvent::LAST_DEPARTURE ||
        ev.event == PresenceEvent::ANYONE_HOME    ||
        ev.event == PresenceEvent::EVERYONE_AWAY) {
      _publishOccupancy();
    }

    // Publish per-device state on arrival/departure
    if (ev.event == PresenceEvent::PERSON_ARRIVED ||
        ev.event == PresenceEvent::PERSON_DEPARTED) {
      Device* dev = _sm->getDevice(ev.deviceId);
      if (dev) _publishDevice(*dev);
    }

    // Always publish the raw event
    _publishRawEvent(ev);
  }

  // ----------------------------------------------------------
  //  publishFullState() — publish everything on connect/reconnect
  // ----------------------------------------------------------
  void publishFullState() {
    if (!_enabled || !_client || !_client->connected()) return;
    _publishOccupancy();
    for (const auto& dev : _sm->devices()) {
      _publishDevice(dev);
    }
    _publishHealth();
  }

  bool isConnected() { return _enabled && (_client != nullptr) && _client->connected(); }
  bool isEnabled()   { return _enabled; }


private:

  WiFiClient*   _wifiClient = nullptr;
  PubSubClient* _client     = nullptr;
  Config*       _cfg      = nullptr;
  StateMachine* _sm       = nullptr;
  String        _root     = "presence";
  bool          _enabled  = false;

  uint32_t _lastReconnectMs = 0;
  uint32_t _lastHealthMs    = 0;

  // ----------------------------------------------------------
  //  Create fresh WiFiClient and PubSubClient instances
  //  Called on init and on each reconnect to prevent buffer leaks
  // ----------------------------------------------------------
  void _createClient(const char* broker, int port) {
    // Destroy old instances first to free any leaked buffers
    if (_client) {
      if (_client->connected()) _client->disconnect();
      delete _client;
      _client = nullptr;
    }
    if (_wifiClient) {
      _wifiClient->stop();
      delete _wifiClient;
      _wifiClient = nullptr;
    }

    // Create fresh instances
    _wifiClient = new WiFiClient();
    _wifiClient->setTimeout(3);

    _client = new PubSubClient(*_wifiClient);
    _client->setServer(broker, port);
    _client->setKeepAlive(MQTT_KEEPALIVE_SEC);
    _client->setSocketTimeout(3);
    _client->setBufferSize(1024);
    _client->setCallback([this](char* topic, byte* payload, unsigned int len) {
      _onMessage(topic, payload, len);
    });

    Serial.printf("[MQTT] Client recreated for %s:%d\n", broker, port);
  }

  // ----------------------------------------------------------
  //  Connect to broker and publish discovery + full state
  // ----------------------------------------------------------
  void _connect() {
    if (!_cfg) return;

    // Recreate client objects to flush any leaked buffers from previous attempt
    _createClient(_cfg->platform.mqttBroker.c_str(),
                  _cfg->platform.mqttPort);

    String clientId = "PresencePlatform_" + WiFi.macAddress();
    clientId.replace(":", "");

    // Last will — published if device drops offline unexpectedly
    String willTopic  = _root + "/status";
    String willMsg    = "offline";

    Serial.printf("[MQTT] Connecting to %s:%d as %s...\n",
                  _cfg->platform.mqttBroker.c_str(),
                  _cfg->platform.mqttPort,
                  clientId.c_str());

    bool connected = false;
    if (_cfg->platform.mqttUser.isEmpty()) {
      connected = _client->connect(clientId.c_str(),
                                  willTopic.c_str(), 1, true,
                                  willMsg.c_str());
    } else {
      connected = _client->connect(clientId.c_str(),
                                  _cfg->platform.mqttUser.c_str(),
                                  _cfg->platform.mqttPassword.c_str(),
                                  willTopic.c_str(), 1, true,
                                  willMsg.c_str());
    }

    if (connected) {
      Serial.println("[MQTT] Connected.");

      // Mark as online
      _client->publish((_root + "/status").c_str(), "online", true);

      // Subscribe to command topics
      _client->subscribe((_root + "/cmd/poll").c_str());
      _client->subscribe((_root + "/cmd/restart").c_str());

      // Publish HA auto-discovery
      _publishDiscovery();

      // Publish current state
      publishFullState();

    } else {
      Serial.printf("[MQTT] Connection failed. State: %d\n",
                    _client->state());
    }
  }

  // ----------------------------------------------------------
  //  Publish global occupancy
  // ----------------------------------------------------------
  void _publishOccupancy() {
    String occ = _sm->anyoneHome() ? "home" : "away";
    _publish(_root + "/occupancy",    occ,                          true);
    _publish(_root + "/anyone_home",  _sm->anyoneHome() ? "true" : "false", true);
    _publish(_root + "/device_count", String(_sm->homeCount()),     true);
  }

  // ----------------------------------------------------------
  //  Publish per-device state
  // ----------------------------------------------------------
  void _publishDevice(const Device& dev) {
    String base = _root + "/devices/" + dev.id;
    _publish(base + "/state",       dev.stateName(),              true);
    _publish(base + "/confidence",  String(dev.lastConfidence),   true);
    _publish(base + "/last_seen",   String(dev.lastSeenEpoch),    true);
    _publish(base + "/method",      dev.methodName(),             true);
  }

  // ----------------------------------------------------------
  //  Publish raw event JSON
  // ----------------------------------------------------------
  void _publishRawEvent(const PresenceEventData& ev) {
    String payload = "{\"event\":\"" + String(ev.eventName()) + "\","
                   + "\"device_id\":\"" + ev.deviceId + "\","
                   + "\"device_name\":\"" + ev.deviceName + "\","
                   + "\"timestamp\":" + String(ev.timestampEpoch) + "}";
    _publish(_root + "/events", payload, false);
  }

  // ----------------------------------------------------------
  //  Publish health JSON
  // ----------------------------------------------------------
  void _publishHealth() {
    String payload = "{\"uptime_sec\":" + String(millis()/1000) + ","
                   + "\"free_heap\":" + String(ESP.getFreeHeap()) + ","
                   + "\"rssi\":" + String(WiFi.RSSI()) + ","
                   + "\"ip\":\"" + WiFi.localIP().toString() + "\","
                   + "\"fw_version\":\"2.5.0\"}";
    _publish(_root + "/health", payload, true);
  }

  // ----------------------------------------------------------
  //  Home Assistant MQTT auto-discovery
  //  Publishes config topics so HA creates entities automatically
  // ----------------------------------------------------------
  void _publishDiscovery() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");

    // Device info block shared across all entities
    String devBlock = "\"device\":{"
      "\"identifiers\":[\"presenceplatform_" + mac + "\"],"
      "\"name\":\"PresencePlatform\","
      "\"model\":\"PresencePlatform ESP32\","
      "\"manufacturer\":\"PresencePlatform\","
      "\"sw_version\":\"1.5.0\""
    "}";

    // Global occupancy binary sensor
    String occTopic = "homeassistant/binary_sensor/presenceplatform_"
                    + mac + "_occupancy/config";
    String occConfig = "{"
      "\"name\":\"Occupancy\","
      "\"unique_id\":\"pp_" + mac + "_occupancy\","
      "\"state_topic\":\"" + _root + "/occupancy\","
      "\"payload_on\":\"home\","
      "\"payload_off\":\"away\","
      "\"device_class\":\"occupancy\","
      "\"availability_topic\":\"" + _root + "/status\","
      "\"payload_available\":\"online\","
      "\"payload_not_available\":\"offline\","
      + devBlock + "}";
    _publish(occTopic, occConfig, true);

    // Per-device presence binary sensors
    for (const auto& dev : _sm->devices()) {
      String devTopic = "homeassistant/binary_sensor/presenceplatform_"
                      + mac + "_" + dev.id + "/config";
      String devConfig = "{"
        "\"name\":\"" + dev.friendlyName + "\","
        "\"unique_id\":\"pp_" + mac + "_" + dev.id + "\","
        "\"state_topic\":\"" + _root + "/devices/" + dev.id + "/state\","
        "\"payload_on\":\"present\","
        "\"payload_off\":\"absent\","
        "\"device_class\":\"presence\","
        "\"availability_topic\":\"" + _root + "/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        + devBlock + "}";
      _publish(devTopic, devConfig, true);
    }

    Serial.println("[MQTT] HA auto-discovery published.");
  }

  // ----------------------------------------------------------
  //  Handle incoming command messages
  // ----------------------------------------------------------
  void _onMessage(char* topic, byte* payload, unsigned int len) {
    String t = String(topic);
    Serial.printf("[MQTT] Command received: %s\n", t.c_str());

    if (t == _root + "/cmd/poll") {
      Serial.println("[MQTT] Force poll requested.");
      _sm->forcePoll(g_epochNow);
      publishFullState();
    } else if (t == _root + "/cmd/restart") {
      Serial.println("[MQTT] Restart requested via MQTT.");
      delay(500);
      ESP.restart();
    }
  }

  // ----------------------------------------------------------
  //  Safe publish helper
  // ----------------------------------------------------------
  void _publish(const String& topic, const String& payload, bool retain) {
    if (!_client->connected()) return;
    bool ok = _client->publish(topic.c_str(), payload.c_str(), retain);
    if (!ok) {
      Serial.printf("[MQTT] Publish failed: %s\n", topic.c_str());
    }
  }
};
