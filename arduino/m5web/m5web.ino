// m5web — Arduino IDE build of the same firmware as ../../src/main.cpp.
// This folder is a flat-file mirror of ../../src + ../../data, kept in sync
// manually because the Arduino IDE requires every source file to sit
// directly inside a folder named after the .ino (no subfolders, unlike
// PlatformIO's src/ layout). If you change one copy, mirror the change in
// the other.
//
// Board setup:
//   1. Boards Manager URL: https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
//   2. Install "esp32" by Espressif Systems, then select board "M5Atom".
//   3. Tools > Partition Scheme: any scheme with a SPIFFS/LittleFS region
//      (e.g. "Default 4MB with spiffs").
//   4. Upload this sketch normally (Sketch > Upload), then upload the
//      data/ folder to LittleFS — see README.md for the plugin needed.

#include <Arduino.h>

#include "printer.h"
#include "web_server.h"
#include "wifi_manager.h"

// ATOM Lite's builtin button (G39, active LOW). Hold 5s to forget saved
// WiFi credentials and drop back into AP setup mode.
constexpr uint8_t kButtonPin = 39;
constexpr unsigned long kResetHoldMs = 5000;
unsigned long buttonDownSinceMs = 0;

void checkResetButton() {
    bool pressed = digitalRead(kButtonPin) == LOW;
    if (pressed) {
        if (buttonDownSinceMs == 0) buttonDownSinceMs = millis();
        if (millis() - buttonDownSinceMs > kResetHoldMs) {
            WifiManager::forgetAndRestart();
        }
    } else {
        buttonDownSinceMs = 0;
    }
}

void setup() {
    pinMode(kButtonPin, INPUT);
    Printer::begin();
    WifiManager::begin();
    WebServer_::begin();
}

void loop() {
    WifiManager::loop();
    WebServer_::loop();
    checkResetButton();
}
