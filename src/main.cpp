#include <Arduino.h>

#include "camera_link.h"
#include "gallery.h"
#include "led.h"
#include "openai.h"
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
    const char *modeStr = cs.mode == CameraLink::Mode::kPreview     ? "preview"
                          : cs.mode == CameraLink::Mode::kAutoHaiku ? "autoHaiku"
                                                                     : "auto";
    Serial.printf("camera: mode=%s frameReady=%d pendingPrint=%d last=%ux%u brightness=%d contrast=%d\n", modeStr,
                  cs.frameReady, cs.pendingPrint, cs.width, cs.height, cs.brightness, cs.contrast);
    Serial.println("-----------------------");
}

void setup() {
    Serial.begin(115200);  // USB/UART0 diagnostic log, see `pio device monitor`
    Serial.println("\n=== m5web starting ===");
    pinMode(kButtonPin, INPUT);
    Printer::begin();
    Led::begin();
    Gallery::begin();  // reports pre-existing saved photos to Led on begin()
    CameraLink::begin();
    OpenAI::begin();
    WifiManager::begin();
    WebServer_::begin();
    Serial.println("=== m5web ready ===");
}

void loop() {
    WifiManager::loop();
    WebServer_::loop();
    CameraLink::poll();
    Led::poll();
    checkButton();
    printHeartbeat();
}
