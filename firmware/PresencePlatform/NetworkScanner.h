#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <vector>
#include <map>
#include <lwip/etharp.h>
#include <lwip/netif.h>

// ============================================================
//  NetworkScanner.h — Background ARP scanner with own cache
//  PresencePlatform v2.1.5
//
//  Probes the subnet with ARP requests and stores ALL replies
//  in its own map — not limited by the ESP32's ARP_TABLE_SIZE.
//  Results persist across scans; entries expire after 10 minutes.
// ============================================================

#define SCAN_INTERVAL_MS   120000UL  // 2 min auto-scan — faster cache population
#define SCAN_ARP_DELAY_MS  800       // wait for ARP replies
#define SCAN_ENTRY_TTL_MS  600000UL  // 10 min entry expiry

struct NetworkDevice {
  String   mac;
  String   ip;
  bool     likelyPhone;
  uint32_t lastSeenMs;
};

class NetworkScanner {
public:

  void begin() {
    _mutex = xSemaphoreCreateMutex();
  }

  void init() {
    _lastScanMs  = 0;
    _scanReady   = false;
    _triggerScan = true;
    xTaskCreatePinnedToCore(
      _scanTask, "netscan",
      8192, this, 1, &_taskHandle, 1
    );
    Serial.println("[Scanner] Network scanner started.");
  }

  std::vector<NetworkDevice> getDevices() {
    std::vector<NetworkDevice> result;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
      uint32_t now = millis();
      for (auto& kv : _cache) {
        // Only return non-expired entries
        if ((now - kv.second.lastSeenMs) < SCAN_ENTRY_TTL_MS) {
          result.push_back(kv.second);
        }
      }
      xSemaphoreGive(_mutex);
    }
    return result;
  }

  void triggerScan() { _triggerScan = true; }

  // Read-only cache lookup — does NOT trigger a new scan
  std::vector<NetworkDevice> getDevicesCached() {
    std::vector<NetworkDevice> result;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      uint32_t now = millis();
      for (auto& kv : _cache) {
        if ((now - kv.second.lastSeenMs) < SCAN_ENTRY_TTL_MS) {
          result.push_back(kv.second);
        }
      }
      xSemaphoreGive(_mutex);
    }
    return result;
  }

  // Look up current IP for a MAC address in the cache
  // Returns empty string if not found
  String getIpForMac(const String& mac) {
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      auto it = _cache.find(mac);
      String ip = "";
      if (it != _cache.end()) {
        ip = it->second.ip;
      }
      xSemaphoreGive(_mutex);
      return ip;
    }
    return "";
  }
  bool isScanReady()  const { return _scanReady; }
  uint32_t lastScanAge() const {
    return _lastScanMs > 0 ? (millis() - _lastScanMs) / 1000 : 999;
  }

private:
  TaskHandle_t                      _taskHandle  = nullptr;
  SemaphoreHandle_t                 _mutex       = nullptr;
  std::map<String, NetworkDevice>   _cache;       // keyed by MAC
  uint32_t                          _lastScanMs  = 0;
  bool                              _scanReady   = false;
  volatile bool                     _triggerScan = true;

  static void _scanTask(void* param) {
    NetworkScanner* self = (NetworkScanner*)param;
    for (;;) {
      bool shouldScan = self->_triggerScan;
      if (!shouldScan && self->_lastScanMs > 0) {
        shouldScan = (millis() - self->_lastScanMs) > SCAN_INTERVAL_MS;
      }
      if (shouldScan && WiFi.status() == WL_CONNECTED) {
        self->_triggerScan = false;
        self->_doScan();
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }

  void _doScan() {
    struct netif* netif = netif_default;
    if (!netif) return;

    uint32_t myIp   = ntohl(ip_2_ip4(&netif->ip_addr)->addr);
    uint32_t myMask = ntohl(ip_2_ip4(&netif->netmask)->addr);
    uint32_t base   = myIp & myMask;
    uint32_t count  = (~myMask) & 0xFFFFFFFF;
    if (count > 254) count = 254;

    Serial.printf("[Scanner] Scanning %d IPs...\n", count);
    uint32_t start = millis();

    // Send ARP requests in batches
    for (uint32_t i = 1; i <= count; i++) {
      uint32_t targetIp = base | i;
      if (targetIp == myIp) continue;
      ip4_addr_t target;
      target.addr = htonl(targetIp);
      etharp_request(netif, &target);
      if ((i % 16) == 0) vTaskDelay(pdMS_TO_TICKS(8));
    }

    // Wait for replies
    vTaskDelay(pdMS_TO_TICKS(SCAN_ARP_DELAY_MS));

    // Read ARP table and store ALL found entries in our own cache
    // We do multiple passes to catch as many as possible before eviction
    int found = 0;
    for (int pass = 0; pass < 5; pass++) {
      for (uint32_t i = 1; i <= count; i++) {
        uint32_t targetIp = base | i;
        if (targetIp == myIp) continue;

        ip4_addr_t target;
        target.addr = htonl(targetIp);

        const ip4_addr_t* found_ip  = nullptr;
        struct eth_addr*  found_eth = nullptr;

        s8_t idx = etharp_find_addr(netif, &target, &found_eth, &found_ip);
        if (idx < 0 || !found_eth || !found_ip) continue;

        char macStr[18];
        snprintf(macStr, sizeof(macStr), "%02X:%02X:%02X:%02X:%02X:%02X",
                 found_eth->addr[0], found_eth->addr[1], found_eth->addr[2],
                 found_eth->addr[3], found_eth->addr[4], found_eth->addr[5]);

        char ipStr[16];
        ip4addr_ntoa_r(found_ip, ipStr, sizeof(ipStr));

        String macString = String(macStr);
        String ipString  = String(ipStr);

        if (ipString.endsWith(".255")) continue;
        if (ipString == WiFi.localIP().toString()) continue;

        // Determine if likely phone
        bool likelyPhone = (found_eth->addr[0] & 0x02) != 0;
        if (!likelyPhone) {
          uint32_t oui = ((uint32_t)found_eth->addr[0] << 16) |
                         ((uint32_t)found_eth->addr[1] << 8)  |
                          (uint32_t)found_eth->addr[2];
          // Apple devices (iPhone, iPad, Mac, Apple Watch, AirPods)
          // Apple owns many OUI blocks — check first byte 0x00 with known Apple second bytes
          // and other common Apple prefixes
          bool isApple = (
            oui == 0x848BE1 || oui == 0xF0B429 || oui == 0xDCA904 ||
            oui == 0x3C2211 || oui == 0x786C1C || oui == 0xB88D12 ||
            oui == 0x000000 || // placeholder - use byte check below
            false
          );
          // More reliable: check known Apple OUI first-byte patterns
          uint8_t b0 = found_eth->addr[0];
          uint8_t b1 = found_eth->addr[1];
          isApple = isApple ||
            (b0 == 0x00 && (b1 == 0x0A || b1 == 0x1B || b1 == 0x1E ||
                            b1 == 0x23 || b1 == 0x25 || b1 == 0x26 ||
                            b1 == 0x50 || b1 == 0xCD || b1 == 0xC6)) ||
            (b0 == 0x04 && (b1 == 0x15 || b1 == 0x4B || b1 == 0x52 ||
                            b1 == 0xD3 || b1 == 0xF0 || b1 == 0x54)) ||
            (b0 == 0x08 && (b1 == 0x66 || b1 == 0x6D || b1 == 0x74 ||
                            b1 == 0xF8 || b1 == 0xBE)) ||
            (b0 == 0x0C && (b1 == 0x30 || b1 == 0x74 || b1 == 0x77 ||
                            b1 == 0xCB || b1 == 0xE9)) ||
            (b0 == 0x10 && (b1 == 0x1C || b1 == 0x40 || b1 == 0x41 ||
                            b1 == 0x9F || b1 == 0xDD)) ||
            (b0 == 0x14 && (b1 == 0x10 || b1 == 0x20 || b1 == 0x5A ||
                            b1 == 0x8B || b1 == 0x99 || b1 == 0xC2)) ||
            (b0 == 0x18 && (b1 == 0x20 || b1 == 0x65 || b1 == 0x9D ||
                            b1 == 0xAF || b1 == 0xE7 || b1 == 0xF6)) ||
            (b0 == 0x1C && (b1 == 0x1A || b1 == 0x36 || b1 == 0x91 ||
                            b1 == 0xB0 || b1 == 0xE8 || b1 == 0xF2)) ||
            (b0 == 0x20 && (b1 == 0x76 || b1 == 0x78 || b1 == 0x7B ||
                            b1 == 0x9E || b1 == 0xAB || b1 == 0xC9)) ||
            (b0 == 0x24 && (b1 == 0x1E || b1 == 0x50 || b1 == 0x5A ||
                            b1 == 0xA0 || b1 == 0xAB || b1 == 0xE4)) ||
            (b0 == 0x28 && (b1 == 0x37 || b1 == 0x6A || b1 == 0xCF ||
                            b1 == 0xD0 || b1 == 0xE1 || b1 == 0xF0)) ||
            (b0 == 0x2C && (b1 == 0x1F || b1 == 0x61 || b1 == 0xAC ||
                            b1 == 0xBE || b1 == 0xF0)) ||
            (b0 == 0x34 && (b1 == 0x08 || b1 == 0x15 || b1 == 0x36 ||
                            b1 == 0xAB || b1 == 0xC7 || b1 == 0xE8)) ||
            (b0 == 0x38 && (b1 == 0x0F || b1 == 0x48 || b1 == 0x71 ||
                            b1 == 0xCA || b1 == 0xF9)) ||
            (b0 == 0x3C && (b1 == 0x06 || b1 == 0x07 || b1 == 0x15 ||
                            b1 == 0x2E || b1 == 0xD0 || b1 == 0xE1)) ||
            (b0 == 0x40 && (b1 == 0x30 || b1 == 0x3C || b1 == 0x6B ||
                            b1 == 0x9B || b1 == 0xCB || b1 == 0xD3)) ||
            (b0 == 0x44 && (b1 == 0x00 || b1 == 0x2C || b1 == 0x4C ||
                            b1 == 0xD8 || b1 == 0xFB)) ||
            (b0 == 0x48 && (b1 == 0x3B || b1 == 0x43 || b1 == 0x60 ||
                            b1 == 0x74 || b1 == 0xA9 || b1 == 0xD7)) ||
            (b0 == 0x4C && (b1 == 0x57 || b1 == 0x74 || b1 == 0x8D ||
                            b1 == 0xB1 || b1 == 0xBC)) ||
            (b0 == 0x50 && (b1 == 0x32 || b1 == 0x7A || b1 == 0x8D ||
                            b1 == 0xBC || b1 == 0xEA || b1 == 0xED)) ||
            (b0 == 0x54 && (b1 == 0x26 || b1 == 0x4A || b1 == 0x72 ||
                            b1 == 0xAE || b1 == 0xE4)) ||
            (b0 == 0x58 && (b1 == 0x1F || b1 == 0x40 || b1 == 0x55 ||
                            b1 == 0x7C || b1 == 0x9A || b1 == 0xB1)) ||
            (b0 == 0x5C && (b1 == 0x59 || b1 == 0x86 || b1 == 0x95 ||
                            b1 == 0xAD || b1 == 0xF9 || b1 == 0xFC)) ||
            (b0 == 0x60 && (b1 == 0x03 || b1 == 0x33 || b1 == 0x9A ||
                            b1 == 0xC5 || b1 == 0xD9 || b1 == 0xF8)) ||
            (b0 == 0x64 && (b1 == 0x20 || b1 == 0x4A || b1 == 0x76 ||
                            b1 == 0x9B || b1 == 0xA3 || b1 == 0xE8)) ||
            (b0 == 0x68 && (b1 == 0x09 || b1 == 0x64 || b1 == 0x96 ||
                            b1 == 0xAB || b1 == 0xD4 || b1 == 0xFB)) ||
            (b0 == 0x6C && (b1 == 0x19 || b1 == 0x40 || b1 == 0x4A ||
                            b1 == 0x72 || b1 == 0x8D || b1 == 0x96)) ||
            (b0 == 0x70 && (b1 == 0x3E || b1 == 0x56 || b1 == 0x73 ||
                            b1 == 0xEC)) ||
            (b0 == 0x74 && (b1 == 0x1B || b1 == 0x8B || b1 == 0xE0 ||
                            b1 == 0xF4)) ||
            (b0 == 0x78 && (b1 == 0x4F || b1 == 0x6D || b1 == 0x7E ||
                            b1 == 0xCA || b1 == 0xD6 || b1 == 0xFD)) ||
            (b0 == 0x7C && (b1 == 0x04 || b1 == 0x11 || b1 == 0x5C ||
                            b1 == 0x6D || b1 == 0xF9)) ||
            (b0 == 0x80 && (b1 == 0x49 || b1 == 0x92 || b1 == 0xB6 ||
                            b1 == 0xE6 || b1 == 0xED)) ||
            (b0 == 0x84 && (b1 == 0x29 || b1 == 0x38 || b1 == 0x41 ||
                            b1 == 0x78 || b1 == 0x85 || b1 == 0x8B ||
                            b1 == 0xA1 || b1 == 0xFC)) ||
            (b0 == 0x88 && (b1 == 0x19 || b1 == 0x1D || b1 == 0x63 ||
                            b1 == 0x66 || b1 == 0xBE || b1 == 0xE9)) ||
            (b0 == 0x8C && (b1 == 0x00 || b1 == 0x29 || b1 == 0x2D ||
                            b1 == 0x58 || b1 == 0x85 || b1 == 0xB0)) ||
            (b0 == 0x90 && (b1 == 0x27 || b1 == 0x60 || b1 == 0x72 ||
                            b1 == 0x8D || b1 == 0xB0 || b1 == 0xDD)) ||
            (b0 == 0x94 && (b1 == 0x03 || b1 == 0x65 || b1 == 0x8B ||
                            b1 == 0xE6 || b1 == 0xF6)) ||
            (b0 == 0x98 && (b1 == 0x01 || b1 == 0x10 || b1 == 0x5A ||
                            b1 == 0x9C || b1 == 0xE1 || b1 == 0xF3)) ||
            (b0 == 0x9C && (b1 == 0x04 || b1 == 0x20 || b1 == 0x35 ||
                            b1 == 0x84 || b1 == 0xF3)) ||
            (b0 == 0xA0 && (b1 == 0x18 || b1 == 0x3E || b1 == 0x45 ||
                            b1 == 0x99 || b1 == 0xB0 || b1 == 0xD9)) ||
            (b0 == 0xA4 && (b1 == 0x31 || b1 == 0x67 || b1 == 0x6B ||
                            b1 == 0xB8 || b1 == 0xC3 || b1 == 0xD9)) ||
            (b0 == 0xA8 && (b1 == 0x20 || b1 == 0x51 || b1 == 0x86 ||
                            b1 == 0x88 || b1 == 0xBB || b1 == 0xFA)) ||
            (b0 == 0xAC && (b1 == 0x1F || b1 == 0x29 || b1 == 0x61 ||
                            b1 == 0x87 || b1 == 0xBC || b1 == 0xE0)) ||
            (b0 == 0xB0 && (b1 == 0x19 || b1 == 0x34 || b1 == 0x65 ||
                            b1 == 0x70 || b1 == 0x9F)) ||
            (b0 == 0xB4 && (b1 == 0x18 || b1 == 0xF1 || b1 == 0x6B)) ||
            (b0 == 0xB8 && (b1 == 0x09 || b1 == 0x44 || b1 == 0x53 ||
                            b1 == 0x8A || b1 == 0xC7 || b1 == 0xE8)) ||
            (b0 == 0xBC && (b1 == 0x3A || b1 == 0x52 || b1 == 0x67 ||
                            b1 == 0x92 || b1 == 0xEC)) ||
            (b0 == 0xC0 && (b1 == 0x1A || b1 == 0x63 || b1 == 0x84 ||
                            b1 == 0xCB || b1 == 0xD3 || b1 == 0xF2)) ||
            (b0 == 0xC4 && (b1 == 0x2C || b1 == 0x3A || b1 == 0xB5 ||
                            b1 == 0xD3 || b1 == 0xF0)) ||
            (b0 == 0xC8 && (b1 == 0x1E || b1 == 0x3A || b1 == 0x69 ||
                            b1 == 0x85 || b1 == 0xB3 || b1 == 0xD3)) ||
            (b0 == 0xCC && (b1 == 0x08 || b1 == 0x29 || b1 == 0x44 ||
                            b1 == 0x78 || b1 == 0xA4)) ||
            (b0 == 0xD0 && (b1 == 0x03 || b1 == 0x4F || b1 == 0xB5 ||
                            b1 == 0xC6 || b1 == 0xD1 || b1 == 0xE7)) ||
            (b0 == 0xD4 && (b1 == 0x20 || b1 == 0x61 || b1 == 0x8B ||
                            b1 == 0x9C || b1 == 0xF5)) ||
            (b0 == 0xD8 && (b1 == 0x1D || b1 == 0x30 || b1 == 0x96 ||
                            b1 == 0xBB || b1 == 0xD3 || b1 == 0xF3)) ||
            (b0 == 0xDC && (b1 == 0x2B || b1 == 0x37 || b1 == 0x56 ||
                            b1 == 0x86 || b1 == 0x9F || b1 == 0xD6)) ||
            (b0 == 0xE0 && (b1 == 0x33 || b1 == 0x5E || b1 == 0x70 ||
                            b1 == 0xAC || b1 == 0xB5 || b1 == 0xF8)) ||
            (b0 == 0xE4 && (b1 == 0x25 || b1 == 0x9B || b1 == 0xC8 ||
                            b1 == 0xCE || b1 == 0xE4)) ||
            (b0 == 0xE8 && (b1 == 0x04 || b1 == 0x06 || b1 == 0x50 ||
                            b1 == 0x69 || b1 == 0x8D || b1 == 0xB2)) ||
            (b0 == 0xEC && (b1 == 0x35 || b1 == 0x86 || b1 == 0x9B ||
                            b1 == 0xA3)) ||
            (b0 == 0xF0 && (b1 == 0x18 || b1 == 0x49 || b1 == 0x79 ||
                            b1 == 0x99 || b1 == 0xB4 || b1 == 0xD4 ||
                            b1 == 0xDB || b1 == 0xDC)) ||
            (b0 == 0xF4 && (b1 == 0x0E || b1 == 0x37 || b1 == 0x5C ||
                            b1 == 0xAB || b1 == 0xF9)) ||
            (b0 == 0xF8 && (b1 == 0x1E || b1 == 0x27 || b1 == 0x38 ||
                            b1 == 0x62 || b1 == 0xFF)) ||
            (b0 == 0xFC && (b1 == 0x25 || b1 == 0x4B || b1 == 0xE1 ||
                            b1 == 0xFC));
          likelyPhone = isApple;
        }

        // Store in our persistent cache
        if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          bool isNew = (_cache.find(macString) == _cache.end());
          _cache[macString] = { macString, ipString, likelyPhone, millis() };
          if (isNew) found++;
          xSemaphoreGive(_mutex);
        }
      }
      // Brief pause between passes
      if (pass < 4) vTaskDelay(pdMS_TO_TICKS(150));
    }

    _lastScanMs = millis();
    _scanReady  = true;

    int total = 0;
    if (xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      total = _cache.size();
      xSemaphoreGive(_mutex);
    }
    Serial.printf("[Scanner] Done. %d new, %d total cached (%lums)\n",
                  found, total, millis() - start);
  }
};
