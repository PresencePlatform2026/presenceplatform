#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "NetworkScanner.h"
extern NetworkScanner g_scanner;
#include <map>
#include <set>
#include <vector>
#include "Device.h"

// ============================================================
//  DetectionEngine.h — Multi-layer presence detection
//  PresencePlatform v1.0.0
//
//  Detection hierarchy (highest → lowest confidence):
//    1. ARP lookup          (+100) — definitive when phone is awake
//    2. mDNS / Bonjour      (+80)  — works when iOS is in light sleep
//    3. Passive traffic     (+60)  — sees any frame from device MAC
//    4. ICMP ping           (+40)  — last resort, often blocked
// ============================================================

// Timeouts
#define ARP_TIMEOUT_MS       200
#define PING_TIMEOUT_MS      500
#define MDNS_TIMEOUT_MS      200  // short — single hostname query only

// Passive traffic — how long a cached frame observation stays valid
#define PASSIVE_WINDOW_MS    300000UL  // 5 minutes

// ---- Raw ARP via lwIP ----
// We use esp_wifi and lwIP directly for ARP. The Arduino WiFi
// library does not expose ARP, so we drop to the C layer.
extern "C" {
  #include "lwip/etharp.h"
  #include "lwip/netif.h"
  #include "esp_wifi.h"
}

// ---- Passive frame cache ----
// Populated by the WiFi promiscuous callback (see DetectionEngine.cpp).
// Maps MAC (as 6-byte key packed into uint64_t) → last seen millis().
#include <map>
#include <set>
static std::map<uint64_t, uint32_t> s_passiveCache;
static SemaphoreHandle_t            s_passiveMutex = nullptr;

// Pack 6 MAC bytes into a uint64_t key
static inline uint64_t macToKey(const uint8_t* mac) {
  return ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
         ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
         ((uint64_t)mac[4] <<  8) |  (uint64_t)mac[5];
}

// ---- Passive cache max size — prevents unbounded growth ----
#define PASSIVE_CACHE_MAX_SIZE  64

// ---- Promiscuous callback (runs in WiFi task context) ----
// Set of MAC keys for ABSENT devices — updated by StateMachine
// The sniffer checks against this to trigger immediate polls
static std::set<uint64_t> s_absentMacs;
static SemaphoreHandle_t  s_absentMutex = nullptr;

// Flag set by sniffer when passive traffic seen from ABSENT device
volatile bool g_passiveWakeFlag = false;

static void IRAM_ATTR wifi_sniffer_cb(void* buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA && type != WIFI_PKT_MGMT) return;
  const wifi_promiscuous_pkt_t* pkt = (wifi_promiscuous_pkt_t*)buf;
  if (pkt->rx_ctrl.sig_len < 10) return;

  const uint8_t* src = pkt->payload + 10;
  uint64_t key = macToKey(src);

  if (s_passiveMutex && xSemaphoreTakeFromISR(s_passiveMutex, nullptr) == pdTRUE) {
    s_passiveCache[key] = millis();
    if (s_passiveCache.size() > PASSIVE_CACHE_MAX_SIZE) {
      s_passiveCache.clear();
    }
    xSemaphoreGiveFromISR(s_passiveMutex, nullptr);
  }

  // If this MAC belongs to an ABSENT tracked device, wake the detection task
  if (s_absentMutex && xSemaphoreTakeFromISR(s_absentMutex, nullptr) == pdTRUE) {
    if (s_absentMacs.count(key) > 0) {
      g_passiveWakeFlag = true;  // detection task checks this flag
      // Note: can't call Serial from ISR, flag is logged in detection task
    }
    xSemaphoreGiveFromISR(s_absentMutex, nullptr);
  }
}

// ---- Update absent MAC set — called when device state changes ----
// This tells the passive sniffer which MACs to wake up for
void updateAbsentMacs(const std::vector<uint8_t*>& absentMacList) {
  if (!s_absentMutex) {
    s_absentMutex = xSemaphoreCreateMutex();
  }
  if (xSemaphoreTake(s_absentMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    s_absentMacs.clear();
    for (auto& mac : absentMacList) {
      s_absentMacs.insert(macToKey(mac));
    }
    Serial.printf("[Detection] Passive watchlist: %d ABSENT MAC(s) registered\n",
                  (int)s_absentMacs.size());
    xSemaphoreGive(s_absentMutex);
  }
}

// ---- Periodic cache cleanup — remove stale entries ----
static void cleanPassiveCache() {
  if (!s_passiveMutex) return;
  uint32_t now = millis();
  if (xSemaphoreTake(s_passiveMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    auto it = s_passiveCache.begin();
    while (it != s_passiveCache.end()) {
      if ((now - it->second) > PASSIVE_WINDOW_MS) {
        it = s_passiveCache.erase(it);
      } else {
        ++it;
      }
    }
    xSemaphoreGive(s_passiveMutex);
  }
}


class DetectionEngine {
public:

  // ----------------------------------------------------------
  //  init() — call once from setup()
  // ----------------------------------------------------------
  void init() {
    s_passiveMutex  = xSemaphoreCreateMutex();
  s_absentMutex   = xSemaphoreCreateMutex();

    // Enable promiscuous mode for passive detection
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_promiscuous_rx_cb(wifi_sniffer_cb);

    Serial.println("[Detection] Engine initialised. Promiscuous mode ON.");
  }

  // ----------------------------------------------------------
  //  probe() — run the full detection stack for one device.
  //  Returns a DetectionResult with the best method that succeeded.
  // ----------------------------------------------------------
  DetectionResult probe(Device& dev) {
    // Clean stale entries from passive cache periodically
    cleanPassiveCache();
    DetectionResult result = { false, 0, DetectionMethod::NONE };

    // --- Layer 1: ARP ---
    DetectionResult arp = probeARP(dev);
    if (arp.seen) {
      Serial.printf("[Detection] [%s] ARP HIT (conf %d)\n",
                    dev.friendlyName.c_str(), arp.confidence);
      return arp;
    }

    // --- Layer 2: mDNS / Bonjour ---
    DetectionResult mdns = probeMDNS(dev);
    if (mdns.seen) {
      Serial.printf("[Detection] [%s] mDNS HIT (conf %d)\n",
                    dev.friendlyName.c_str(), mdns.confidence);
      return mdns;
    }

    // --- Layer 3: Passive traffic ---
    DetectionResult passive = probePassive(dev);
    if (passive.seen) {
      Serial.printf("[Detection] [%s] PASSIVE HIT (conf %d)\n",
                    dev.friendlyName.c_str(), passive.confidence);
      return passive;
    }

    // --- Layer 4: Ping (last resort) ---
    DetectionResult ping = probePing(dev);
    if (ping.seen) {
      Serial.printf("[Detection] [%s] PING HIT (conf %d)\n",
                    dev.friendlyName.c_str(), ping.confidence);
      return ping;
    }

    Serial.printf("[Detection] [%s] ALL METHODS MISSED\n",
                  dev.friendlyName.c_str());
    return result;
  }


private:

  // ----------------------------------------------------------
  //  Layer 1 — ARP
  //  Sends an ARP request and checks the lwIP ARP table for a reply.
  // ----------------------------------------------------------
  DetectionResult probeARP(const Device& dev) {
    DetectionResult r = { false, 0, DetectionMethod::ARP };

    struct netif* netif = netif_default;
    if (!netif) return r;

    ip4_addr_t target;
    target.addr = dev.ip;

    // Trigger an ARP request
    etharp_request(netif, &target);
    delay(ARP_TIMEOUT_MS);

    // Check if the ARP table now has an entry for this IP
    const ip4_addr_t* found_addr = nullptr;
    struct eth_addr* found_eth   = nullptr;

    s8_t result = etharp_find_addr(netif, &target,
                                   &found_eth, &found_addr);
    if (result >= 0 && found_eth != nullptr) {
      if (_macMatches(dev.mac, found_eth->addr)) {
        r.seen       = true;
        r.confidence = 100;
      } else {
        Serial.printf("[Detection] [%s] ARP MAC mismatch — possible IP conflict\n",
                      dev.friendlyName.c_str());
      }
    }

    // If ARP failed at stored IP, check scanner cache for updated IP
    if (!r.seen) {
      String macStr = dev.macStr;
      macStr.toUpperCase();
      String cachedIp = g_scanner.getIpForMac(macStr);
      if (cachedIp.length() > 0 && cachedIp != dev.ip.toString()) {
        Serial.printf("[Detection] [%s] IP updated from scanner cache: %s → %s\n",
                      dev.friendlyName.c_str(),
                      dev.ip.toString().c_str(),
                      cachedIp.c_str());
        const_cast<Device&>(dev).ip.fromString(cachedIp);
        // Try ARP at new IP
        ip4_addr_t newTarget;
        newTarget.addr = const_cast<Device&>(dev).ip;
        etharp_request(netif, &newTarget);
        delay(ARP_TIMEOUT_MS);
        const ip4_addr_t* fa2 = nullptr;
        struct eth_addr*  fe2 = nullptr;
        if (etharp_find_addr(netif, &newTarget, &fe2, &fa2) >= 0 && fe2) {
          if (_macMatches(dev.mac, fe2->addr)) {
            r.seen       = true;
            r.confidence = 100;
          }
        }
      }
    }

    return r;
  }

  // ----------------------------------------------------------
  //  Layer 2 — mDNS / Bonjour
  //  Queries for Apple sleep-proxy and general mDNS services.
  //  iOS responds to these even during screen-off low-power mode.
  // ----------------------------------------------------------
  DetectionResult probeMDNS(const Device& dev) {
    DetectionResult r = { false, 0, DetectionMethod::MDNS };

    // Only query mDNS if device has a friendly name for hostname lookup
    if (dev.friendlyName.length() == 0) return r;

    // Build iOS-style mDNS hostname:
    // "Jacob's Iphone" → "Jacobs-Iphone" (remove special chars, replace spaces)
    String hostname = dev.friendlyName;
    String clean = "";
    for (int i = 0; i < (int)hostname.length(); i++) {
      char c = hostname[i];
      if (isAlphaNumeric(c)) {
        clean += c;
      } else if (c == ' ' || c == '-') {
        clean += '-';
      }
      // drop apostrophes and other special chars
    }
    // Remove leading/trailing dashes
    while (clean.startsWith("-")) clean = clean.substring(1);
    while (clean.endsWith("-")) clean = clean.substring(0, clean.length() - 1);

    if (clean.length() == 0) return r;

    // Short timeout — don't block the poll cycle
    IPAddress found = MDNS.queryHost(clean.c_str(), 200);
    if (found != INADDR_NONE && (uint32_t)found != 0) {
      if (found != dev.ip) {
        // Device found at different IP — update stored IP (handles DHCP changes)
        Serial.printf("[Detection] [%s] mDNS IP changed: %s → %s\n",
                      dev.friendlyName.c_str(),
                      dev.ip.toString().c_str(),
                      found.toString().c_str());
        const_cast<Device&>(dev).ip = found;
      }
      r.seen       = true;
      r.confidence = 90;
      return r;
    }

    // Also try lowercase variant
    String lc = clean;
    lc.toLowerCase();
    if (lc != clean) {
      found = MDNS.queryHost(lc.c_str(), 200);
      if (found != INADDR_NONE && (uint32_t)found != 0) {
        if (found != dev.ip) {
          Serial.printf("[Detection] [%s] mDNS IP changed: %s → %s\n",
                        dev.friendlyName.c_str(),
                        dev.ip.toString().c_str(),
                        found.toString().c_str());
          const_cast<Device&>(dev).ip = found;
        }
        r.seen       = true;
        r.confidence = 90;
        return r;
      }
    }

    return r;
  }

  // ----------------------------------------------------------
  //  Layer 3 — Passive traffic detection
  //  Checks the promiscuous frame cache for recent frames
  //  from this device's MAC address.
  // ----------------------------------------------------------
  DetectionResult probePassive(const Device& dev) {
    DetectionResult r = { false, 0, DetectionMethod::PASSIVE };

    uint64_t key = macToKey(dev.mac);
    uint32_t now = millis();

    if (xSemaphoreTake(s_passiveMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      auto it = s_passiveCache.find(key);
      if (it != s_passiveCache.end()) {
        uint32_t age = now - it->second;
        if (age < PASSIVE_WINDOW_MS) {
          r.seen       = true;
          r.confidence = 60;
          // Bonus: the more recent the frame, the higher confidence
          if (age < 60000UL)  r.confidence = 70; // seen in last 1 min
          if (age < 10000UL)  r.confidence = 75; // seen in last 10 sec
        }
      }
      xSemaphoreGive(s_passiveMutex);
    }

    return r;
  }

  // ----------------------------------------------------------
  //  Layer 4 — ICMP Ping
  //  Last resort. Many phones block ICMP in power-save mode.
  //  Used only to break a tie or confirm after other methods miss.
  // ----------------------------------------------------------
  DetectionResult probePing(const Device& dev) {
    DetectionResult r = { false, 0, DetectionMethod::PING };

    // Use ESP32 ping via esp_ping (lwIP)
    // We send 1 ping with a short timeout to avoid blocking the task long
    bool success = _sendPing(dev.ip, PING_TIMEOUT_MS);
    if (success) {
      r.seen       = true;
      r.confidence = 40;
    }
    return r;
  }

  // ----------------------------------------------------------
  //  Helpers
  // ----------------------------------------------------------
  bool _macMatches(const uint8_t* a, const uint8_t* b) {
    return memcmp(a, b, 6) == 0;
  }

  bool _sendPing(IPAddress ip, uint32_t timeoutMs) {
    // Raw ICMP echo request via lwIP
    // esp_ping is available in ESP-IDF but not directly in Arduino.
    // We use a simple TCP connect attempt as a lightweight proxy
    // for ping when ICMP is not available.
    WiFiClient client;
    client.setTimeout(timeoutMs / 1000);
    // Try port 62078 (Apple iOS sync port — often open on iPhones)
    bool ok = client.connect(ip, 62078);
    client.stop();
    if (ok) return true;

    // Try port 7 (echo) as fallback
    ok = client.connect(ip, 7);
    client.stop();
    return ok;
  }
};
