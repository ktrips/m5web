#include <Arduino.h>
#include <esp32-hal-bt.h>

#include "camera_link.h"
#include "gallery.h"
#include "haiku.h"
#include "led.h"
#include "openai.h"
#include "printer.h"
#include "web_server.h"
#include "wifi_manager.h"

// ATOM Lite's builtin button (G39, active LOW).
//   single short press (released before kResetHoldMs, with no second short
//     press following within kDoubleTapWindowMs) -> reprint the last
//     M5StickV camera frame, if any
//   double-click (two short-press releases within kDoubleTapWindowMs of
//     each other)                                -> toggle 俳句/ポエム
//     generation mode (Haiku::poemType()), reflected by the LED color
//     (green=俳句, blue=ポエム; see led.h)
//   held past kResetHoldMs                        -> forget saved WiFi
//     credentials and drop back into AP setup mode
constexpr uint8_t kButtonPin = 39;
constexpr unsigned long kResetHoldMs = 5000;
constexpr unsigned long kMinPressMs = 50;  // debounce floor for a "real" short press
constexpr unsigned long kDoubleTapWindowMs = 400;
unsigned long buttonDownSinceMs = 0;
// 0 = no single-click awaiting confirmation; otherwise the millis() of a
// short-press release still waiting to see whether a second one follows
// within kDoubleTapWindowMs (in which case it becomes a double-click
// instead, and the pending reprint never fires).
unsigned long pendingSingleClickMs = 0;

void togglePoemMode() {
    bool nowHaiku = Haiku::poemType() != "haiku";  // i.e. was "poem" (or unset) -> becomes "haiku"
    Haiku::setPoemType(nowHaiku ? "haiku" : "poem");
    Led::setModeColor(nowHaiku);
    Serial.printf("[button] double-click: poem mode -> %s\n", Haiku::poemType().c_str());
}

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
            unsigned long now = millis();
            if (pendingSingleClickMs != 0 && (now - pendingSingleClickMs) <= kDoubleTapWindowMs) {
                pendingSingleClickMs = 0;
                togglePoemMode();
            } else {
                pendingSingleClickMs = now;
            }
        }
    }

    // A single short press only becomes a reprint once the double-tap
    // window has passed without a second press — see kDoubleTapWindowMs.
    if (pendingSingleClickMs != 0 && millis() - pendingSingleClickMs > kDoubleTapWindowMs) {
        pendingSingleClickMs = 0;
        Serial.println("[button] short press: reprinting last camera frame");
        if (!CameraLink::printLastFrame()) {
            Serial.println("[button] no camera frame to print yet");
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
    const char *modeStr = cs.mode == CameraLink::Mode::kPreview ? "preview" : "auto";
    Serial.printf("camera: mode=%s frameReady=%d pendingPrint=%d last=%ux%u brightness=%d contrast=%d\n", modeStr,
                  cs.frameReady, cs.pendingPrint, cs.width, cs.height, cs.brightness, cs.contrast);
    Serial.printf("poem: type=%s autoMode=%s\n", Haiku::poemType().c_str(), Haiku::autoMode().c_str());
    Serial.println("-----------------------");
}

void setup() {
    Serial.begin(115200);  // USB/UART0 diagnostic log, see `pio device monitor`
    Serial.println("\n=== m5web starting ===");
    // This project never uses Bluetooth — releases the ~36KB the BT
    // controller would otherwise reserve back to the heap. The core
    // usually does this itself (btInUse() defaults to false unless a BT
    // library is linked in), but calling it explicitly costs nothing and
    // removes the doubt while chasing a heap-fragmentation-sensitive bug
    // (see openai.cpp's generateHaiku(), which needs a large *contiguous*
    // free block for TLS certificate parsing — this is about giving it
    // more room to find one, not about raising total free bytes).
    btStop();
    pinMode(kButtonPin, INPUT);
    Printer::begin();
    Led::begin();
    Haiku::begin();
    Led::setModeColor(Haiku::poemType() == "haiku");  // reflect the persisted mode before anything can light the LED
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
