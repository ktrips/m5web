#include "led.h"

namespace Led {

namespace {

constexpr uint8_t kPin = 27;  // ATOM Lite onboard RGB LED (SK6812)
constexpr uint8_t kGreen = 40;  // dim — this is a status light, not a flash

constexpr unsigned long kBlinkOnMs = 150;
constexpr unsigned long kBlinkOffMs = 150;
constexpr uint8_t kBlinkCount = 5;

bool cameraPending = false;
bool galleryNonEmpty = false;

bool blinking = false;
bool blinkLit = false;
uint8_t blinkHalfStepsLeft = 0;  // remaining on/off transitions after the initial on
unsigned long blinkNextMs = 0;

void setGreen(bool on) { neopixelWrite(kPin, 0, on ? kGreen : 0, 0); }

void applyBaseState() { setGreen(cameraPending || galleryNonEmpty); }

}  // namespace

void begin() { setGreen(false); }

void notifyNewImage() {
    blinking = true;
    blinkLit = true;
    setGreen(true);
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

void poll() {
    if (!blinking) return;
    if ((long)(millis() - blinkNextMs) < 0) return;

    blinkLit = !blinkLit;
    setGreen(blinkLit);
    blinkHalfStepsLeft--;
    if (blinkHalfStepsLeft == 0) {
        blinking = false;
        applyBaseState();
        return;
    }
    blinkNextMs = millis() + (blinkLit ? kBlinkOnMs : kBlinkOffMs);
}

}  // namespace Led
