// ============================================================
//  PresencePlatform.ino — Main entry point
//  PresencePlatform v2.5.0
//
//  FreeRTOS task layout:
//    detectionTask   Core 0  — detection + state machine tick
//    tcpTask         Core 1  — TCP socket server (Control4)
//    watchdogTask    Core 0  — health monitoring + WiFi reconnect
//
//  Build: Arduino IDE 2.x + ESP32 Arduino core 2.x
//  Board: ESP32 Dev Module (or equivalent)
//  Partition: Default 4MB with spiffs
//
//  Required libraries:
//    - ESPAsyncWebServer   (lacamera)
//    - AsyncTCP            (dvarrel)
//    - ArduinoJson         (Benoit Blanchon) v7+
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include <esp_task_wdt.h>
#include <ESPAsyncWebServer.h>

#include "Config.h"
#include <Preferences.h>
#include "Device.h"
#include "Events.h"
#include "DetectionEngine.h"
extern volatile bool g_passiveWakeFlag;
#include "StateMachine.h"
#include "ApiHandler.h"
#include "TcpSocketServer.h"
#include "SetupPortal.h"
#include "NtpSync.h"
#include "SharedTypes.h"
#include "NetworkScanner.h"
#include "AutoUpdater.h"

#include "MqttClient.h"
#include "OtaHandler.h"

// ---- Forward declarations ----
void detectionTask(void* param);
void tcpTask(void* param);
void mqttTask(void* param);
void watchdogTask(void* param);

// ---- Crash handler ----
void __attribute__((noreturn)) panicHandler() {
  Serial.println("[CRASH] Panic — rebooting in 3s");
  delay(3000);
  ESP.restart();
}

// ---- Globals ----
Config            g_config;
Preferences       g_statePrefs;
StateMachine      g_sm;
AsyncWebServer*   g_server = nullptr;  // created in setup() with configured port
ApiHandler        g_api;
TcpSocketServer   g_tcp;
SetupPortal       g_portal;
NtpSync           g_ntp;
MqttClient        g_mqtt;
OtaHandler        g_ota;
NetworkScanner    g_scanner;
AutoUpdater       g_updater;

// Task handles
TaskHandle_t h_detection = nullptr;
TaskHandle_t h_tcp       = nullptr;
TaskHandle_t h_mqtt      = nullptr;
TaskHandle_t h_watchdog  = nullptr;

// Shared epoch time
volatile uint32_t g_epochNow = 0;

// RSSI history — 60 readings, one per minute
RssiReading g_rssiHistory[RSSI_HISTORY_SIZE];
int g_rssiHead  = 0;
int g_rssiCount = 0;


// ============================================================
//  setup()
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n\n=== PresencePlatform v2.5.0 ===");

  // ---- LittleFS ----
  if (!LittleFS.begin(true)) {
    Serial.println("[BOOT] LittleFS mount failed — reformatting");
    LittleFS.format();
    LittleFS.begin(true);
  }
  Serial.println("[BOOT] LittleFS mounted.");

  // ---- Load config ----
  g_config.load();

  // ---- WiFi — portal or normal connect ----
  bool wifiOk = false;

  if (SetupPortal::isNeeded(g_config)) {
    // No credentials stored — launch captive portal
    // This call blocks until credentials are saved, then reboots
    if (!g_server) g_server = new AsyncWebServer(g_config.platform.httpPort);
    g_portal.run(g_config, *g_server);
    // Never reaches here — reboot happens inside run()
  } else {
    // Try to connect with stored credentials
    wifiOk = SetupPortal::tryConnect(g_config);
    if (!wifiOk) {
      // Failed after MAX_CONNECT_ATTEMPTS — clear SSID and reboot into portal
      Serial.println("[BOOT] WiFi failed — clearing credentials and rebooting to portal.");
      g_config.platform.wifiSSID = "";
      g_config.save();
      delay(500);
      ESP.restart();
    }
  }

  // ---- NTP time sync ----
  g_ntp.init(); // default Central time — change tz string if needed

  // ---- mDNS ----
  if (MDNS.begin(g_config.platform.hostname.c_str())) {
    MDNS.addService("http",     "tcp", 80);
    MDNS.addService("presence", "tcp", g_config.platform.tcpPort);
    Serial.printf("[BOOT] mDNS: %s.local\n",
                  g_config.platform.hostname.c_str());
  }

  // ---- State machine ----
  g_sm.init();
  g_sm.setGlobalMissThreshold(g_config.platform.missThreshold);

  // Serial event logger
  g_sm.onEvent([](const PresenceEventData& ev) {
    Serial.printf("[EVENT] >>> %s  device=%s  t=%lu\n",
                  ev.eventName(),
                  ev.deviceId.isEmpty() ? "global" : ev.deviceId.c_str(),
                  ev.timestampMs);
  });

  // TCP push callback
  g_sm.onEvent([](const PresenceEventData& ev) {
    g_tcp.pushEvent(ev);
    g_tcp.pushState();
  });

  // MQTT publish callback
  g_sm.onEvent([](const PresenceEventData& ev) {
    g_mqtt.publishEvent(ev);
  });

  // ---- Populate saved devices from config ----
  for (const auto& dev : g_config.getDevices()) {
    g_sm.addDevice(dev);
  }
  // Restore persisted device states — prevents false departures after update reboot
  g_sm.restoreState(g_statePrefs);

  // ---- REST API + web server ----
  g_server = new AsyncWebServer(g_config.platform.httpPort);
  g_api.registerRoutes(*g_server, g_config, g_sm, g_ntp);
  g_ota.registerRoutes(*g_server);
  g_server->begin();
  Serial.printf("[BOOT] Web server started on port %d.\n", g_config.platform.httpPort);

  // ---- Network scanner ----
  g_scanner.begin();
  g_scanner.init();

  // ---- Auto updater ----
  g_updater.init(g_config.platform.autoUpdateEnabled);
  g_updater.setUpdateWindow(g_config.platform.updateWindowStart,
                            g_config.platform.updateWindowEnd);
  g_updater.onBeforeUpdate = []() {
    Serial.println("[Updater] Persisting state and notifying clients before reboot...");
    g_sm.persistState(g_statePrefs);
    g_tcp.broadcastJson("{\"type\":\"update_starting\",\"message\":\"Firmware update in progress. Reconnecting in ~90 seconds.\"}");
  };

  // ---- TCP socket server ----
  g_tcp.init(g_sm, g_config.platform.tcpPort);

  // ---- MQTT client ----
  g_mqtt.init(g_config, g_sm);

  // ---- FreeRTOS tasks ----
  xTaskCreatePinnedToCore(
    detectionTask, "detection",
    12288, nullptr, 2, &h_detection, 0
  );

  xTaskCreatePinnedToCore(
    tcpTask, "tcp",
    6144, nullptr, 1, &h_tcp, 1
  );

  xTaskCreatePinnedToCore(
    mqttTask, "mqtt",
    10240, nullptr, 1, &h_mqtt, 1
  );

  xTaskCreatePinnedToCore(
    watchdogTask, "watchdog",
    6144, nullptr, 1, &h_watchdog, 0
  );

  Serial.println("[BOOT] All tasks started.");
  Serial.printf("[BOOT] GUI: http://%s:%d\n", WiFi.localIP().toString().c_str(), g_config.platform.httpPort);
  Serial.printf("[BOOT] MQTT: %s\n", g_mqtt.isEnabled() ? "enabled" : "disabled");
  Serial.printf("[BOOT] TCP: %s:%d\n",
                WiFi.localIP().toString().c_str(),
                g_config.platform.tcpPort);
}


// ============================================================
//  loop() — intentionally empty
// ============================================================
void loop() {
  vTaskDelay(pdMS_TO_TICKS(10000));
}


// ============================================================
//  detectionTask — Core 0
// ============================================================
void detectionTask(void* param) {
  Serial.println("[Task:detection] Started.");
  vTaskDelay(pdMS_TO_TICKS(8000));

  for (;;) {
    esp_task_wdt_reset();

    // Check for passive wake — ABSENT device seen by sniffer
    if (g_passiveWakeFlag) {
      g_passiveWakeFlag = false;
      Serial.println("[Task:detection] Passive wake — running immediate poll for ABSENT device");
      g_sm.runForcedPoll(g_epochNow);
    }

    // Check scanner cache for ABSENT tracked devices
    // Use a per-MAC cooldown to avoid hammering polls when ARP still fails
    static uint32_t lastScannerCheckMs = 0;
    static String   lastScannerMac = "";
    static uint32_t lastScannerTriggerMs = 0;
    if (millis() - lastScannerCheckMs > 2000) {
      lastScannerCheckMs = millis();
      auto devices = g_scanner.getDevicesCached();
      for (const auto& sd : devices) {
        String mac = sd.mac;
        mac.toUpperCase();
        if (g_sm.isDeviceAbsentByMac(mac)) {
          // Only trigger once per 30 seconds per MAC to avoid infinite poll loops
          bool sameMac   = (mac == lastScannerMac);
          bool cooldown  = (millis() - lastScannerTriggerMs) < 30000;
          if (!sameMac || !cooldown) {
            Serial.printf("[Task:detection] Scanner found ABSENT device %s at %s — triggering poll\n",
                          mac.c_str(), sd.ip.c_str());
            g_sm.updateDeviceIpByMac(mac, sd.ip);
            lastScannerMac       = mac;
            lastScannerTriggerMs = millis();
            g_sm.bForcePoll      = true;
          }
          break;
        }
      }
    }

    // Check for force poll request from GUI/TCP
    if (g_sm.consumeForcePoll()) {
      g_sm.runForcedPoll(g_epochNow);
      Serial.printf("[Task:detection] Forced poll complete. Occupancy: %s  Home: %d/%d\n",
                    g_sm.occupancyName(),
                    g_sm.homeCount(),
                    (int)g_sm.devices().size());
    }

    bool polled = g_sm.tick(g_epochNow);

    if (polled) {
      Serial.printf("[Task:detection] Poll complete. Occupancy: %s  Home: %d/%d\n",
                    g_sm.occupancyName(),
                    g_sm.homeCount(),
                    (int)g_sm.devices().size());
    }

    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}


// ============================================================
//  tcpTask — Core 1
// ============================================================
void tcpTask(void* param) {
  Serial.println("[Task:tcp] Started.");

  for (;;) {
    g_tcp.tick();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}


// ============================================================
//  watchdogTask — Core 0
// ============================================================
void mqttTask(void* param) {
  Serial.println("[Task:mqtt] Started.");

  for (;;) {
    g_mqtt.tick();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}


void watchdogTask(void* param) {
  Serial.println("[Task:watchdog] Started.");

  uint32_t lastHeartbeatMs = 0;
  bool     wifiWasLost     = false;

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(10000));

    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (!wifiWasLost) {
        wifiWasLost = true;
        Serial.println("[Watchdog] WiFi lost — attempting reconnect.");
        g_sm.reportFault("wifi_lost");
      }
      WiFi.reconnect();
    } else if (wifiWasLost) {
      wifiWasLost = false;
      Serial.printf("[Watchdog] WiFi restored. IP: %s\n",
                    WiFi.localIP().toString().c_str());
      g_sm.clearFault();
    }

    // Update epoch time every tick
    g_ntp.tick();

    if ((now - lastHeartbeatMs) >= 60000UL) {
      lastHeartbeatMs = now;
      static uint32_t lastHeap = 0;
      uint32_t currentHeap = ESP.getFreeHeap();
      int32_t heapDelta = lastHeap > 0 ? (int32_t)currentHeap - (int32_t)lastHeap : 0;
      lastHeap = currentHeap;
      // Store RSSI reading
      g_rssiHistory[g_rssiHead] = { g_epochNow, WiFi.RSSI() };
      g_rssiHead = (g_rssiHead + 1) % RSSI_HISTORY_SIZE;
      if (g_rssiCount < RSSI_HISTORY_SIZE) g_rssiCount++;

      Serial.printf("[Watchdog] Heartbeat — uptime %lus  heap %u (%+d)  RSSI %d  occ=%s  tcp=%d  time=%s\n",
                    now / 1000,
                    currentHeap,
                    heapDelta,
                    WiFi.RSSI(),
                    g_sm.occupancyName(),
                    g_tcp.clientCount(),
                    g_ntp.formattedTime().c_str());
      if (heapDelta < -5000) Serial.println("[Watchdog] WARNING: Significant heap loss this minute!");
      Serial.printf("[Watchdog] MQTT: %s\n", g_mqtt.isConnected() ? "connected" : "disconnected");
      g_updater.tick();
    }

    if (h_detection && eTaskGetState(h_detection) == eDeleted) {
      Serial.println("[Watchdog] Detection task died — rebooting.");
      ESP.restart();
    }
  }
}
