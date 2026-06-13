#pragma once
#include <Arduino.h>

// ============================================================
//  SharedTypes.h — Structs shared across multiple files
//  PresencePlatform v2.5.0
// ============================================================

#define FIRMWARE_VERSION "2.5.0"

#define RSSI_HISTORY_SIZE 60

struct RssiReading {
  uint32_t ts;
  int      rssi;
};
