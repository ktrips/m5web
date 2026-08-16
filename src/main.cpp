#include <Arduino.h>

#include "camera_link.h"
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
    checkResetButton();
    printHeartbeat();
}
