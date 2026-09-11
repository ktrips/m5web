// atomMatrix.ino — ATOM Matrix as a wired physical shutter button for CamS3.
//
// Watches this board's own onboard button and, on each press, pulses a
// GPIO output LOW for a moment — wired straight to a CamS3's GPIO0 pin
// (see arduino/camS3/camS3.ino's own GPIO0 handling, already built to
// expect exactly this: "ground GPIO0 briefly (an external push button
// between GPIO0 and GND)"). CamS3 can't tell the difference between this
// and a plain mechanical button, so no CamS3-side changes are needed.
//
// Deliberately NOT WiFi-based: this exists as an alternative to
// CamS3Remote (src/cams3_remote.*, the ATOM Lite's own single-click ->
// HTTP GET http://m5cam.local/shutter trigger) for exactly the failure
// mode that path is exposed to — WiFi/mDNS being unreachable or slow.
// A direct wire has no network to fail, at the cost of needing the two
// boards physically close together and sharing a ground.
//
// Wiring (2 wires, no Grove/JST needed — plain jumper wires to the pin
// header on both boards):
//   ATOM Matrix G26 (this board's kTriggerOutPin) -> CamS3 GPIO0
//   ATOM Matrix GND                               -> CamS3 GND
// A shared ground is required for the logic-level signal to mean
// anything on the receiving end — without it this will not work
// reliably (or at all).
//
// ⚠️ CamS3's GPIO0 is an ESP32 boot-strapping pin: held LOW at power-on
// or reset, it boots into the UART download/bootloader instead of
// running the app. Fine for normal operation (this board only pulses it
// low well after CamS3 has already booted), but don't power up or reset
// CamS3 while this board happens to be mid-pulse.
//
// Target hardware: M5Stack ATOM Matrix (same ESP32-PICO-D4 + onboard
// button on G39 as ATOM Lite — see src/main.cpp's checkButton() for the
// same button-reading approach reused here — just with a 5x5 SK6812 LED
// matrix on G27 instead of ATOM Lite's single pixel; that matrix is NOT
// driven by this sketch, see below). Arduino IDE: board "M5Atom" (same
// entry covers both Lite and Matrix), no extra libraries — this is
// plain digitalRead()/digitalWrite()/Serial, nothing else.
//
// No visual feedback on the LED matrix in this version: ATOM Lite's
// single-pixel led.cpp uses the core's neopixelWrite() helper, which
// only drives one WS2812-type LED — ATOM Matrix's 25-pixel array needs a
// proper NeoPixel driver (FastLED or Adafruit_NeoPixel), a new
// dependency deliberately left out to keep this bridge minimal. Add one
// if a lit-up confirmation on button press is wanted; Serial logging
// (115200bps) is the only feedback for now.
//
// UNTESTED ON REAL HARDWARE.

#include <Arduino.h>

constexpr uint8_t kButtonPin = 39;      // onboard button, active LOW — same pin/polarity as ATOM Lite
constexpr uint8_t kTriggerOutPin = 26;  // -> CamS3 GPIO0 (see wiring above)
constexpr unsigned long kMinPressMs = 50;    // debounce floor for a "real" press, same as src/main.cpp
constexpr unsigned long kDebounceMs = 300;   // ignore re-triggers within this long of the last one
constexpr unsigned long kPulseMs = 250;      // how long the output pin stays LOW per trigger — comfortably
                                              // longer than one pass of CamS3's own loop() so its
                                              // digitalRead() can't miss the pulse between iterations

unsigned long buttonDownSinceMs = 0;
unsigned long lastTriggerMs = 0;

void triggerCamS3() {
    Serial.println("[atomMatrix] button pressed — pulsing GPIO26 LOW to trigger CamS3's shutter");
    digitalWrite(kTriggerOutPin, LOW);
    delay(kPulseMs);
    digitalWrite(kTriggerOutPin, HIGH);
    Serial.println("[atomMatrix] pulse done");
}

void checkButton() {
    bool pressed = digitalRead(kButtonPin) == LOW;
    if (pressed) {
        if (buttonDownSinceMs == 0) buttonDownSinceMs = millis();
    } else if (buttonDownSinceMs != 0) {
        unsigned long heldMs = millis() - buttonDownSinceMs;
        buttonDownSinceMs = 0;
        if (heldMs >= kMinPressMs && (millis() - lastTriggerMs) > kDebounceMs) {
            lastTriggerMs = millis();
            triggerCamS3();
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== atomMatrix starting ===");

    pinMode(kButtonPin, INPUT);
    pinMode(kTriggerOutPin, OUTPUT);
    digitalWrite(kTriggerOutPin, HIGH);  // idle high — CamS3's GPIO0 is INPUT_PULLUP, active LOW

    Serial.println("=== atomMatrix ready — press the button to trigger CamS3's shutter ===");
}

void loop() {
    checkButton();
}
