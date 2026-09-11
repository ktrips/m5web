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
// LED matrix feedback (5x5 SK6812, G27, driven via FastLED — see
// setMatrix()/blinkMatrixGreen()):
//   - WHITE, solid, for as long as the button is held down (immediate
//     "you're pressing it" feedback).
//   - GREEN, 3x blink, once the trigger pulse to CamS3 has been sent in
//     full. This means "this board successfully sent the signal" — there
//     is no wire back from CamS3, so it is NOT a confirmation that CamS3
//     actually captured/printed a photo (see CamS3's own LED — GPIO14 —
//     or its serial log for that). If the press is swallowed by the
//     debounce window (see kDebounceMs) instead of triggering, the white
//     just turns off with no green blink, since nothing was sent.
//   - No sound: this board has no onboard speaker/buzzer (unlike M5Stack
//     ATOM Echo, which does) — LED-only feedback is all that's possible
//     without adding external hardware.
//
// Target hardware: M5Stack ATOM Matrix (same ESP32-PICO-D4 + onboard
// button on G39 as ATOM Lite — see src/main.cpp's checkButton() for the
// same button-reading approach reused here — just with a 5x5 SK6812 LED
// matrix on G27 instead of ATOM Lite's single pixel). Arduino IDE: board
// "M5Atom" (same entry covers both Lite and Matrix). Requires the
// "FastLED" library (Daniel Garcia et al.) via Arduino Library Manager —
// the only extra dependency; the core's own neopixelWrite() helper
// (used by ATOM Lite's led.cpp) only drives a single WS2812-type LED, not
// this board's 25-pixel array.
//
// UNTESTED ON REAL HARDWARE.

#include <Arduino.h>
#include <FastLED.h>

constexpr uint8_t kButtonPin = 39;      // onboard button, active LOW — same pin/polarity as ATOM Lite
constexpr uint8_t kTriggerOutPin = 26;  // -> CamS3 GPIO0 (see wiring above)
constexpr unsigned long kMinPressMs = 50;    // debounce floor for a "real" press, same as src/main.cpp
constexpr unsigned long kDebounceMs = 300;   // ignore re-triggers within this long of the last one
constexpr unsigned long kPulseMs = 250;      // how long the output pin stays LOW per trigger — comfortably
                                              // longer than one pass of CamS3's own loop() so its
                                              // digitalRead() can't miss the pulse between iterations

constexpr uint8_t kMatrixPin = 27;    // onboard 5x5 SK6812 matrix
constexpr uint8_t kMatrixCount = 25;
constexpr uint8_t kMatrixBrightness = 40;  // dim — matches led.cpp's own kBrightness for the same reason
constexpr uint8_t kSuccessBlinkCount = 3;
constexpr unsigned long kSuccessBlinkOnMs = 150;
constexpr unsigned long kSuccessBlinkOffMs = 150;

CRGB matrixLeds[kMatrixCount];

unsigned long buttonDownSinceMs = 0;
unsigned long lastTriggerMs = 0;

void setMatrix(const CRGB &color) {
    fill_solid(matrixLeds, kMatrixCount, color);
    FastLED.show();
}

void blinkMatrixGreen() {
    for (uint8_t i = 0; i < kSuccessBlinkCount; i++) {
        setMatrix(CRGB::Green);
        delay(kSuccessBlinkOnMs);
        setMatrix(CRGB::Black);
        if (i + 1 < kSuccessBlinkCount) delay(kSuccessBlinkOffMs);
    }
}

void triggerCamS3() {
    Serial.println("[atomMatrix] button pressed — pulsing GPIO26 LOW to trigger CamS3's shutter");
    // Matrix is already white from checkButton()'s press-down handling;
    // stays that way through the pulse itself, then switches to the
    // green "sent" blink below.
    digitalWrite(kTriggerOutPin, LOW);
    delay(kPulseMs);
    digitalWrite(kTriggerOutPin, HIGH);
    Serial.println("[atomMatrix] pulse done");
    blinkMatrixGreen();
}

void checkButton() {
    bool pressed = digitalRead(kButtonPin) == LOW;
    if (pressed) {
        if (buttonDownSinceMs == 0) {
            buttonDownSinceMs = millis();
            setMatrix(CRGB::White);  // immediate "you're pressing it" feedback
        }
    } else if (buttonDownSinceMs != 0) {
        unsigned long heldMs = millis() - buttonDownSinceMs;
        buttonDownSinceMs = 0;
        if (heldMs >= kMinPressMs && (millis() - lastTriggerMs) > kDebounceMs) {
            lastTriggerMs = millis();
            triggerCamS3();  // leaves the matrix off after its green blink
        } else {
            setMatrix(CRGB::Black);  // press too short / still in debounce window — nothing sent, so no green blink
        }
    }
}

void setup() {
    Serial.begin(115200);
    Serial.println("\n=== atomMatrix starting ===");

    pinMode(kButtonPin, INPUT);
    pinMode(kTriggerOutPin, OUTPUT);
    digitalWrite(kTriggerOutPin, HIGH);  // idle high — CamS3's GPIO0 is INPUT_PULLUP, active LOW

    FastLED.addLeds<WS2812, kMatrixPin, GRB>(matrixLeds, kMatrixCount);
    FastLED.setBrightness(kMatrixBrightness);
    setMatrix(CRGB::Black);

    Serial.println("=== atomMatrix ready — press the button to trigger CamS3's shutter ===");
}

void loop() {
    checkButton();
}
