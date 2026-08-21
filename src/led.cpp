#include "led.h"

namespace Led {

namespace {

constexpr uint8_t kPin = 27;  // ATOM Lite onboard RGB LED (SK6812)
constexpr uint8_t kBrightness = 40;  // dim — this is a status light, not a flash

constexpr unsigned long kBlinkOnMs = 150;
constexpr unsigned long kBlinkOffMs = 150;
constexpr uint8_t kBlinkCount = 5;

bool cameraPending = false;
bool galleryNonEmpty = false;
bool haikuMode = true;  // true = green (俳句), false = blue (ポエム); see setModeColor()

bool blinking = false;
bool blinkLit = false;
uint8_t blinkHalfStepsLeft = 0;  // remaining on/off transitions after the initial on
unsigned long blinkNextMs = 0;

// Lights the LED in the current mode color (green/blue), or off — never
// both channels at once, so the color always unambiguously reads as one
// mode or the other.
void setLit(bool on) {
    if (haikuMode) {
        neopixelWrite(kPin, 0, on ? kBrightness : 0, 0);
    } else {
        neopixelWrite(kPin, 0, 0, on ? kBrightness : 0);
    }
}

void applyBaseState() { setLit(cameraPending || galleryNonEmpty); }

}  // namespace

void begin() { setLit(false); }

void notifyNewImage() {
    blinking = true;
    blinkLit = true;
    setLit(true);
    blinkHalfStepsLeft = kBlinkCount * 2 - 1;  // remaining: off,on,off,on,off
    blinkNextMs = millis() + kBlinkOnMs;
}

void setCameraPending(bool pending) {
    cameraPending = pending;
    if (!blinking) applyBaseState();
}

void setGalleryNonEmpty(bool nonEmpty) {
    galleryNonEmpty = nonEmpty;
    if (!blinking) applyBaseState();
}

void setModeColor(bool isHaikuMode) {
    haikuMode = isHaikuMode;
    if (!blinking) applyBaseState();
}

void poll() {
    if (!blinking) return;
    if ((long)(millis() - blinkNextMs) < 0) return;

    blinkLit = !blinkLit;
    setLit(blinkLit);
    blinkHalfStepsLeft--;
    if (blinkHalfStepsLeft == 0) {
        blinking = false;
        applyBaseState();
        return;
    }
    blinkNextMs = millis() + (blinkLit ? kBlinkOnMs : kBlinkOffMs);
}

}  // namespace Led
