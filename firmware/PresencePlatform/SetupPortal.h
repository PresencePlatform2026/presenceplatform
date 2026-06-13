#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include "Config.h"

// ============================================================
//  SetupPortal.h — WiFi provisioning captive portal
//  PresencePlatform v1.3.0
//
//  Triggered when:
//    - No WiFi credentials are stored (first boot)
//    - WiFi fails to connect after MAX_CONNECT_ATTEMPTS tries
//
//  Behavior:
//    - ESP32 broadcasts AP: "PresenceSetup_XXXX"
//    - DNS server redirects all domains to 192.168.7.1
//    - Captive portal page served at 192.168.7.1
//    - Installer enters SSID + password, hits Save
//    - Credentials written to NVS, ESP32 reboots
//    - Device joins home network, AP disappears
// ============================================================

#define PORTAL_IP           "192.168.7.1"
#define PORTAL_DNS_PORT     53
#define PORTAL_HTTP_PORT    80
#define MAX_CONNECT_ATTEMPTS 3
#define CONNECT_TIMEOUT_MS  20000UL
#define PORTAL_TIMEOUT_MS   300000UL  // 5 min — reboot if no config received


static const char PORTAL_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>PresencePlatform Setup</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;
     background:#f5f5f3;display:flex;align-items:center;justify-content:center;
     min-height:100vh;padding:16px}
.card{background:#fff;border-radius:12px;padding:32px;width:100%;max-width:380px;
      box-shadow:0 2px 16px rgba(0,0,0,.08)}
.logo{font-size:18px;font-weight:500;margin-bottom:4px}
.sub{font-size:13px;color:#6b6b66;margin-bottom:28px;line-height:1.5}
.label{font-size:12px;font-weight:500;color:#6b6b66;display:block;margin-bottom:4px}
.input{width:100%;padding:10px 12px;border:1px solid #d0d0cc;border-radius:8px;
       font-size:14px;font-family:inherit;margin-bottom:16px;transition:border .15s}
.input:focus{outline:none;border-color:#378ADD}
.btn{width:100%;padding:11px;background:#1a1a18;color:#fff;border:none;
     border-radius:8px;font-size:14px;font-family:inherit;cursor:pointer;
     font-weight:500;transition:opacity .15s}
.btn:hover{opacity:.85}
.btn:disabled{opacity:.5;cursor:not-allowed}
.networks{margin-bottom:16px}
.network-item{padding:10px 12px;border:1px solid #e5e5e2;border-radius:8px;
              cursor:pointer;margin-bottom:6px;display:flex;align-items:center;
              justify-content:space-between;transition:all .15s;font-size:13px}
.network-item:hover{background:#f5f5f3;border-color:#d0d0cc}
.network-item.selected{border-color:#378ADD;background:#E6F1FB}
.signal{font-size:11px;color:#9b9b96}
.scan-btn{font-size:12px;color:#378ADD;background:none;border:none;cursor:pointer;
          padding:0;margin-bottom:12px;font-family:inherit}
.success{background:#E1F5EE;border:1px solid #1D9E75;border-radius:8px;
         padding:14px;color:#085041;font-size:13px;text-align:center;line-height:1.6}
.error{background:#FAECE7;border:1px solid #D85A30;border-radius:8px;
       padding:14px;color:#4A1B0C;font-size:13px;margin-bottom:16px}
.spinner{display:inline-block;width:14px;height:14px;border:2px solid rgba(255,255,255,.3);
         border-top-color:#fff;border-radius:50%;animation:spin .6s linear infinite;
         margin-right:6px;vertical-align:middle}
@keyframes spin{to{transform:rotate(360deg)}}
.hidden{display:none}
.manual-toggle{font-size:12px;color:#378ADD;background:none;border:none;
               cursor:pointer;padding:0;margin-bottom:12px;font-family:inherit}
</style>
</head>
<body>
<div class="card">
  <div class="logo">PresencePlatform</div>
  <div class="sub">Connect this device to your WiFi network to complete setup.</div>

  <div id="setupForm">
    <button class="scan-btn" onclick="scanNetworks()">⟳ Scan for networks</button>
    <div id="networkList" class="networks hidden"></div>

    <button class="manual-toggle" onclick="toggleManual()" id="manualToggle">
      Enter network name manually
    </button>

    <div id="manualEntry">
      <label class="label">Network name (SSID)</label>
      <input id="ssid" class="input" type="text" placeholder="Your WiFi network name" autocomplete="off">
    </div>

    <label class="label">Password</label>
    <input id="pass" class="input" type="password" placeholder="WiFi password" autocomplete="off">

    <div id="errorMsg" class="error hidden"></div>

    <button class="btn" id="saveBtn" onclick="saveConfig()">
      Connect to network
    </button>
  </div>

  <div id="successMsg" class="success hidden">
    <strong>Connected!</strong><br>
    Your device is joining the network.<br>
    This page will close in a few seconds.<br><br>
    Access the dashboard at:<br>
    <strong id="deviceUrl">http://presence-platform.local</strong><br>
    <div id="deviceIpRow" style="display:none;margin-top:6px">
      Or by IP address:<br>
      <strong id="deviceIp"></strong>
    </div>
  </div>
</div>

<script>
var selectedSSID = '';
var manualVisible = true;

var _scanAttempts = 0;
var _scanMaxAttempts = 8;
var _scanPollDelay = 1500;

function scanNetworks() {
  _scanAttempts = 0;
  var list = document.getElementById('networkList');
  list.innerHTML = '<div style="font-size:12px;color:#9b9b96;padding:8px 0">Scanning for networks...</div>';
  list.classList.remove('hidden');
  _doScan();
}

function _doScan() {
  fetch('/scan').then(function(r){
    if(!r.ok) throw new Error('HTTP ' + r.status);
    return r.json();
  }).then(function(d){
    if(d.status === 'scanning'){
      _scanAttempts++;
      if(_scanAttempts < _scanMaxAttempts){
        // Increase delay slightly each attempt to give scan more time
        var delay = _scanPollDelay + (_scanAttempts * 200);
        setTimeout(_doScan, delay);
      } else {
        _showScanFailed();
      }
    } else {
      renderNetworks(d);
    }
  }).catch(function(){
    _scanAttempts++;
    if(_scanAttempts < _scanMaxAttempts){
      setTimeout(_doScan, _scanPollDelay * 2);
    } else {
      _showScanFailed();
    }
  });
}

function _showScanFailed() {
  var list = document.getElementById('networkList');
  list.innerHTML = '<div style="font-size:12px;color:#9b9b96;padding:8px 0">Scan did not complete. ' +
    '<span style="color:#378ADD;cursor:pointer;text-decoration:underline" onclick="scanNetworks()">Try again</span>' +
    ' or enter the network name manually below.</div>';
}

function renderNetworks(networks) {
  var list = document.getElementById('networkList');
  if(!networks || networks.length === 0){
    list.innerHTML = '<div style="font-size:12px;color:#9b9b96;padding:8px 0">No networks found. ' +
      '<span style="color:#378ADD;cursor:pointer;text-decoration:underline" onclick="scanNetworks()">Scan again</span>' +
      ' or enter name manually.</div>';
    return;
  }
  list.innerHTML = networks.map(function(n){
    var bars = n.rssi > -60 ? '▂▄▆█' : n.rssi > -70 ? '▂▄▆' : n.rssi > -80 ? '▂▄' : '▂';
    var lock = n.encrypted ? ' 🔒' : '';
    return '<div class="network-item" onclick="selectNetwork(this,\''+escSSID(n.ssid)+'\')">'+
      '<span>'+n.ssid+lock+'</span><span class="signal">'+bars+'</span></div>';
  }).join('');
  document.getElementById('manualEntry').classList.add('hidden');
  document.getElementById('manualToggle').textContent = 'Enter network name manually';
  manualVisible = false;
}


function escSSID(s){ return s.replace(/'/g,"\\'"); }

function selectNetwork(el, ssid){
  document.querySelectorAll('.network-item').forEach(function(i){i.classList.remove('selected');});
  el.classList.add('selected');
  selectedSSID = ssid;
  document.getElementById('ssid').value = ssid;
}

function toggleManual(){
  manualVisible = !manualVisible;
  var entry = document.getElementById('manualEntry');
  var btn   = document.getElementById('manualToggle');
  entry.classList.toggle('hidden', !manualVisible);
  btn.textContent = manualVisible ? 'Choose from scanned networks' : 'Enter network name manually';
}

function saveConfig(){
  var ssid = document.getElementById('ssid').value.trim();
  var pass = document.getElementById('pass').value;
  var err  = document.getElementById('errorMsg');

  if(!ssid){
    err.textContent = 'Please enter or select a network name.';
    err.classList.remove('hidden');
    return;
  }

  var btn = document.getElementById('saveBtn');
  btn.disabled = true;
  btn.innerHTML = '<span class="spinner"></span>Connecting...';
  err.classList.add('hidden');

  _trySave(ssid, pass, btn, err, 0);
}

function _trySave(ssid, pass, btn, err, attempt) {
  fetch('/save', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({ssid: ssid, password: pass})
  }).then(function(r){return r.json();}).then(function(d){
    if(d.ok){
      document.getElementById('setupForm').classList.add('hidden');
      var msg = document.getElementById('successMsg');
      msg.classList.remove('hidden');
      var localUrl = 'http://' + d.hostname + '.local';
      var ipUrl = d.ip ? 'http://' + d.ip : null;
      document.getElementById('deviceUrl').textContent = localUrl;
      if(ipUrl) {
        document.getElementById('deviceIp').textContent = ipUrl;
        document.getElementById('deviceIpRow').style.display = 'block';
      }
      setTimeout(function(){ window.location.href = ipUrl || localUrl; }, 5000);
    } else {
      err.textContent = d.error || 'Failed to save. Try again.';
      err.classList.remove('hidden');
      btn.disabled = false;
      btn.textContent = 'Connect to network';
    }
  }).catch(function(){
    if(attempt < 2){
      // Auto-retry up to 2 times before giving up
      setTimeout(function(){ _trySave(ssid, pass, btn, err, attempt + 1); }, 1500);
      btn.innerHTML = '<span class="spinner"></span>Retrying...';
    } else {
      err.textContent = 'Could not reach device. Make sure you are connected to PresenceSetup network and try again.';
      err.classList.remove('hidden');
      btn.disabled = false;
      btn.textContent = 'Connect to network';
    }
  });
}

// Auto-scan on load
window.onload = function(){ scanNetworks(); };
</script>
</body>
</html>
)rawhtml";


class SetupPortal {
public:

  // ----------------------------------------------------------
  //  isNeeded() — check if portal should be launched
  //  Returns true if no credentials or repeated connect failure
  // ----------------------------------------------------------
  static bool isNeeded(Config& cfg) {
    return cfg.platform.wifiSSID.isEmpty();
  }

  // ----------------------------------------------------------
  //  tryConnect() — attempt WiFi connection with retry logic
  //  Returns true if connected, false if all attempts failed
  // ----------------------------------------------------------
  static bool tryConnect(Config& cfg) {
    if (cfg.platform.wifiSSID.isEmpty()) return false;

    for (int attempt = 1; attempt <= MAX_CONNECT_ATTEMPTS; attempt++) {
      Serial.printf("[WiFi] Connect attempt %d/%d to '%s'...\n",
                    attempt, MAX_CONNECT_ATTEMPTS,
                    cfg.platform.wifiSSID.c_str());

      WiFi.mode(WIFI_STA);
      WiFi.setHostname(cfg.platform.hostname.c_str());
      WiFi.begin(cfg.platform.wifiSSID.c_str(),
                 cfg.platform.wifiPassword.c_str());

      uint32_t start = millis();
      while (WiFi.status() != WL_CONNECTED) {
        if ((millis() - start) > CONNECT_TIMEOUT_MS) break;
        delay(500);
        Serial.print(".");
      }
      Serial.println();

      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
                      WiFi.localIP().toString().c_str(),
                      WiFi.RSSI());
        return true;
      }

      Serial.printf("[WiFi] Attempt %d failed.\n", attempt);
      WiFi.disconnect(true);
      delay(1000);
    }

    Serial.println("[WiFi] All attempts failed — launching setup portal.");
    return false;
  }

  // ----------------------------------------------------------
  //  run() — launch the captive portal and block until
  //  credentials are saved, then reboot.
  // ----------------------------------------------------------
  void run(Config& cfg, AsyncWebServer& server) {
    Serial.println("[Portal] Starting setup AP...");

    // Build unique AP name from MAC
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[32];
    snprintf(apName, sizeof(apName), "PresenceSetup_%02X%02X", mac[4], mac[5]);

    // Start AP
    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(
      IPAddress(192, 168, 7, 1),
      IPAddress(192, 168, 7, 1),
      IPAddress(255, 255, 255, 0)
    );
    WiFi.softAP(apName);
    Serial.printf("[Portal] AP: '%s'  IP: 192.168.7.1\n", apName);

    // DNS server — redirect everything to 192.168.7.1
    _dns.start(PORTAL_DNS_PORT, "*", IPAddress(192, 168, 7, 1));

    // Web server
    _setupRoutes(cfg, server);
    server.begin();

    Serial.println("[Portal] Waiting for WiFi credentials...");

    uint32_t startMs = millis();

    // Block here until credentials saved or timeout
    while (!_credentialsSaved) {
      _dns.processNextRequest();
      delay(10);

      // Timeout — reboot and try again
      if ((millis() - startMs) > PORTAL_TIMEOUT_MS) {
        Serial.println("[Portal] Timeout — rebooting.");
        ESP.restart();
      }
    }

    // Credentials received — verify they are non-empty before rebooting
    if (cfg.platform.wifiSSID.isEmpty()) {
      Serial.println("[Portal] Empty SSID received — ignoring, waiting again.");
      _credentialsSaved = false;
      startMs = millis();
      while (!_credentialsSaved) {
        _dns.processNextRequest();
        delay(10);
        if ((millis() - startMs) > PORTAL_TIMEOUT_MS) {
          ESP.restart();
        }
      }
    }

    // Good credentials — save and reboot
    Serial.println("[Portal] Valid credentials saved. Rebooting in 2s...");
    delay(2000);
    ESP.restart();
  }


private:

  DNSServer      _dns;
  bool           _credentialsSaved = false;
  String         _scanCache       = "";
  uint32_t       _scanCacheTime   = 0;
  static const uint32_t SCAN_CACHE_MS = 30000UL; // cache scan results for 30s

  void _setupRoutes(Config& cfg, AsyncWebServer& server) {

    // ---- Captive portal detection — redirect all to setup page ----
    // iOS, Android, Windows all probe these URLs to detect portals
    auto captiveRedirect = [](AsyncWebServerRequest* req) {
      req->redirect("http://192.168.7.1/");
    };
    server.on("/hotspot-detect.html",      HTTP_GET, captiveRedirect);
    server.on("/library/test/success.html",HTTP_GET, captiveRedirect);
    server.on("/connecttest.txt",           HTTP_GET, captiveRedirect);
    server.on("/redirect",                  HTTP_GET, captiveRedirect);
    server.on("/generate_204",              HTTP_GET, captiveRedirect);
    server.on("/ncsi.txt",                  HTTP_GET, captiveRedirect);

    // ---- Main setup page ----
    server.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
      req->send_P(200, "text/html", PORTAL_HTML);
    });

    // ---- WiFi scan — trigger async scan ----
    server.on("/scan", HTTP_GET, [this](AsyncWebServerRequest* req) {
      // Serve cached results if fresh (avoids re-scan on retry)
      if (!_scanCache.isEmpty() && (millis() - _scanCacheTime) < SCAN_CACHE_MS) {
        req->send(200, "application/json", _scanCache);
        return;
      }
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        req->send(202, "application/json", "{\"status\":\"scanning\"}");
        return;
      }
      if (n <= 0) {
        WiFi.scanNetworks(true);
        req->send(202, "application/json", "{\"status\":\"scanning\"}");
        return;
      }
      // Results ready — build JSON and cache it
      JsonDocument doc;
      JsonArray arr = doc.to<JsonArray>();
      for (int i = 0; i < n; i++) {
        bool dup = false;
        for (int j = 0; j < i; j++) {
          if (WiFi.SSID(i) == WiFi.SSID(j)) { dup = true; break; }
        }
        if (dup || WiFi.SSID(i).isEmpty()) continue;
        JsonObject o = arr.add<JsonObject>();
        o["ssid"]      = WiFi.SSID(i);
        o["rssi"]      = WiFi.RSSI(i);
        o["encrypted"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
      }
      // Cache results for 30s — don't delete immediately
      // so rapid retries get instant results
      _scanCacheTime = millis();
      serializeJson(doc, _scanCache);
      WiFi.scanDelete();

      String out = _scanCache;
      req->send(200, "application/json", out);
    });

    // ---- Save credentials ----
    server.on("/save", HTTP_POST,
      [](AsyncWebServerRequest* req){},
      nullptr,
      [&cfg, this](AsyncWebServerRequest* req, uint8_t* data, size_t len, size_t, size_t) {
        JsonDocument doc;
        if (deserializeJson(doc, data, len) != DeserializationError::Ok) {
          req->send(400, "application/json", "{\"ok\":false,\"error\":\"invalid_json\"}");
          return;
        }

        String ssid = doc["ssid"] | "";
        String pass = doc["password"] | "";

        if (ssid.isEmpty()) {
          req->send(400, "application/json", "{\"ok\":false,\"error\":\"ssid_required\"}");
          return;
        }

        // Save to config
        cfg.platform.wifiSSID     = ssid;
        cfg.platform.wifiPassword = pass;
        cfg.save();

        Serial.printf("[Portal] Credentials saved. SSID: %s\n", ssid.c_str());

        JsonDocument resp;
        resp["ok"]       = true;
        resp["hostname"] = cfg.platform.hostname;
        // Include IP if already connected (may not be available yet)
        String ip = WiFi.localIP().toString();
        if (ip != "0.0.0.0") resp["ip"] = ip;
        String out;
        serializeJson(resp, out);
        req->send(200, "application/json", out);

        _credentialsSaved = true;
      }
    );

    // ---- 404 — redirect to setup page ----
    server.onNotFound([](AsyncWebServerRequest* req) {
      req->redirect("http://192.168.7.1/");
    });
  }
};
