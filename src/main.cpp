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
