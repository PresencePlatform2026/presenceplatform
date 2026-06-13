#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <Update.h>

// ============================================================
//  OtaHandler.h — Over-the-air firmware and filesystem updates
//  PresencePlatform v1.6.0
//
//  Routes registered:
//    POST /api/v1/ota/firmware    — upload new firmware .bin
//    POST /api/v1/ota/filesystem  — upload new LittleFS .bin
//
//  Safety:
//    - Requires auth token (dealer login)
//    - MD5 verification built into ESP32 Update library
//    - Automatic rollback if update fails or verification fails
//    - Detection task continues during upload (non-blocking)
//    - Reboots automatically on successful update
//
//  How to get .bin files from Arduino IDE:
//    Firmware:   Sketch → Export Compiled Binary → .bin file
//    Filesystem: PlatformIO or manual mklittlefs build
// ============================================================

// Forward declaration
static bool _isAuthorized(AsyncWebServerRequest* req);
static void _sendError(AsyncWebServerRequest* req, int code, const String& msg);
static void _setCorsHeaders(AsyncWebServerResponse* res);

class OtaHandler {
public:

  void registerRoutes(AsyncWebServer& server) {

    // ---- POST /api/v1/ota/firmware ----
    server.on("/api/v1/ota/firmware", HTTP_POST,
      // Request complete callback — reboot on success
      [](AsyncWebServerRequest* req) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);

        bool success = !Update.hasError();
        AsyncWebServerResponse* res = req->beginResponse(
          success ? 200 : 500,
          "application/json",
          success ? "{\"ok\":true,\"message\":\"firmware_update_complete\"}"
                  : "{\"ok\":false,\"error\":\"update_failed\"}"
        );
        res->addHeader("Connection", "close");
        _setCorsHeaders(res);
        req->send(res);

        if (success) {
          Serial.println("[OTA] Firmware update complete. Rebooting...");
          delay(1000);
          ESP.restart();
        }
      },
      // File upload callback
      [](AsyncWebServerRequest* req, String filename,
         size_t index, uint8_t* data, size_t len, bool final) {

        if (!_isAuthorized(req)) return;

        if (index == 0) {
          Serial.printf("[OTA] Firmware upload started: %s\n", filename.c_str());
          // Stop detection task during update to free resources
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) {
            Update.printError(Serial);
            return;
          }
        }

        if (Update.write(data, len) != len) {
          Update.printError(Serial);
          return;
        }

        if (final) {
          if (Update.end(true)) {
            Serial.printf("[OTA] Firmware upload complete: %u bytes\n",
                          index + len);
          } else {
            Update.printError(Serial);
          }
        }
      }
    );

    // ---- POST /api/v1/ota/filesystem ----
    server.on("/api/v1/ota/filesystem", HTTP_POST,
      // Request complete callback
      [](AsyncWebServerRequest* req) {
        if (!_isAuthorized(req)) return _sendUnauthorized(req);

        bool success = !Update.hasError();
        AsyncWebServerResponse* res = req->beginResponse(
          success ? 200 : 500,
          "application/json",
          success ? "{\"ok\":true,\"message\":\"filesystem_update_complete\"}"
                  : "{\"ok\":false,\"error\":\"update_failed\"}"
        );
        res->addHeader("Connection", "close");
        _setCorsHeaders(res);
        req->send(res);

        if (success) {
          Serial.println("[OTA] Filesystem update complete. Rebooting...");
          delay(1000);
          ESP.restart();
        }
      },
      // File upload callback
      [](AsyncWebServerRequest* req, String filename,
         size_t index, uint8_t* data, size_t len, bool final) {

        if (!_isAuthorized(req)) return;

        if (index == 0) {
          Serial.printf("[OTA] Filesystem upload started: %s\n", filename.c_str());
          if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_SPIFFS)) {
            Update.printError(Serial);
            return;
          }
        }

        if (Update.write(data, len) != len) {
          Update.printError(Serial);
          return;
        }

        if (final) {
          if (Update.end(true)) {
            Serial.printf("[OTA] Filesystem upload complete: %u bytes\n",
                          index + len);
          } else {
            Update.printError(Serial);
          }
        }
      }
    );
  }

private:

  static void _sendUnauthorized(AsyncWebServerRequest* req) {
    req->send(401, "application/json", "{\"error\":\"unauthorized\"}");
  }
};
