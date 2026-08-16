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

#include "camera_link.h"
#include "printer.h"
#include "web_server.h"
#include "wifi_manager.h"

// ATOM Lite's builtin button (G39, active LOW).
//   short press (released before kResetHoldMs) -> reprint the last M5StickV
//     camera frame, if any
//   held past kResetHoldMs                     -> forget saved WiFi
//     credentials and drop back into AP setup mode
constexpr uint8_t kButtonPin = 39;
constexpr unsigned long kResetHoldMs = 5000;
constexpr unsigned long kMinPressMs = 50;  // debounce floor for a "real" short press
unsigned long buttonDownSinceMs = 0;

void checkButton() {
    bool pressed = digitalRead(kButtonPin) == LOW;
    if (pressed) {
        if (buttonDownSinceMs == 0) buttonDownSinceMs = millis();
        if (millis() - buttonDownSinceMs > kResetHoldMs) {
            WifiManager::forgetAndRestart();  // restarts the board; does not return
        }
    } else if (buttonDownSinceMs != 0) {
        unsigned long heldMs = millis() - buttonDownSinceMs;
        buttonDownSinceMs = 0;
        if (heldMs >= kMinPressMs && heldMs < kResetHoldMs) {
            Serial.println("[button] short press: reprinting last camera frame");
            if (!CameraLink::printLastFrame()) {
                Serial.println("[button] no camera frame to print yet");
            }
        }
    }
}

// Periodic snapshot of what the device is doing, so opening the serial
// monitor mid-session immediately shows current state rather than only
// past event logs.
constexpr unsigned long kHeartbeatIntervalMs = 10000;
unsigned long lastHeartbeatMs = 0;

void printHeartbeat() {
    unsigned long now = millis();
    if (now - lastHeartbeatMs < kHeartbeatIntervalMs) return;
    lastHeartbeatMs = now;

    Serial.println("---- m5web status ----");
    Serial.printf("uptime: %lus  free heap: %u bytes\n", now / 1000, ESP.getFreeHeap());

    if (WifiManager::isConnected()) {
        Serial.printf("wifi: station, ssid=%s ip=%s\n", WifiManager::ssid().c_str(),
                      WifiManager::localIP().c_str());
    } else if (WifiManager::mode() == WifiManager::Mode::kAccessPoint) {
        Serial.printf("wifi: AP mode, ssid=%s ip=%s\n", WifiManager::apSSID().c_str(),
                      WifiManager::localIP().c_str());
    } else {
        Serial.println("wifi: connecting...");
    }

    CameraLink::Status cs = CameraLink::status();
    Serial.printf("camera: mode=%s frameReady=%d pendingPrint=%d last=%ux%u brightness=%d contrast=%d\n",
                  cs.mode == CameraLink::Mode::kPreview ? "preview" : "auto", cs.frameReady, cs.pendingPrint,
                  cs.width, cs.height, cs.brightness, cs.contrast);
    Serial.println("-----------------------");
}

void setup() {
    Serial.begin(115200);  // USB/UART0 diagnostic log, see `pio device monitor`
    Serial.println("\n=== m5web starting ===");
    pinMode(kButtonPin, INPUT);
    Printer::begin();
    CameraLink::begin();
    WifiManager::begin();
    WebServer_::begin();
    Serial.println("=== m5web ready ===");
}

void loop() {
    WifiManager::loop();
    WebServer_::loop();
    CameraLink::poll();
    checkButton();
    printHeartbeat();
}
