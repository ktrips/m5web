#include "led.h"

namespace Led {

namespace {

constexpr uint8_t kPin = 27;  // ATOM Lite onboard RGB LED (SK6812)
constexpr uint8_t kBrightness = 40;  // dim — this is a status light, not a flash

constexpr unsigned long kBlinkOnMs = 150;
constexpr unsigned long kBlinkOffMs = 150;
constexpr uint8_t kNewImageBlinkCount = 5;
constexpr uint8_t kPrintDoneBlinkCount = 3;

// notifyShutterOk()'s single blink is deliberately longer/slower than the
// others (see kBlinkOnMs above) so a lone flash reads as "shutter" at a
// glance, distinct from notifyPrintDone()'s brisk 3x — same green either
// way (see setGreenLit()), just a different rhythm.
constexpr unsigned long kShutterBlinkOnMs = 500;
constexpr uint8_t kShutterBlinkCount = 1;

bool cameraPending = false;
bool galleryNonEmpty = false;
bool haikuMode = true;  // true = green (俳句), false = blue (ポエム); see setModeColor()

bool blinking = false;
bool blinkLit = false;
uint8_t blinkHalfStepsLeft = 0;  // remaining on/off transitions after the initial on
unsigned long blinkNextMs = 0;
unsigned long blinkOnMs = kBlinkOnMs;  // this blink's on-duration; off-duration is always kBlinkOffMs
// Which color the in-progress blink uses — set once when the blink starts
// (see startBlink()) and read back each half-step by poll(). Not used
// outside a blink; the steady state always uses setWhiteLit() instead.
enum class BlinkColor { kModeColor, kGreen };
BlinkColor blinkColor = BlinkColor::kModeColor;

// Lights the LED in the current mode color (green/blue), or off — never
// both channels at once, so the color always unambiguously reads as one
// mode or the other. Used only for the notifyNewImage() blink; the steady
// "something is printable" state uses setWhiteLit() instead (see
// applyBaseState()) so it reads as a distinct, unambiguous "ready" signal
// regardless of poem mode.
void setModeColorLit(bool on) {
    if (haikuMode) {
        neopixelWrite(kPin, 0, on ? kBrightness : 0, 0);
    } else {
        neopixelWrite(kPin, 0, 0, on ? kBrightness : 0);
    }
}

// Fixed green regardless of poem mode — used for notifyPrintDone(), so a
// completed print reads the same way whether in 俳句 or ポエム mode
// (unlike notifyNewImage()'s mode-colored blink, which is deliberately
// mode-dependent).
void setGreenLit(bool on) { neopixelWrite(kPin, 0, on ? kBrightness : 0, 0); }

void setWhiteLit(bool on) { neopixelWrite(kPin, on ? kBrightness : 0, on ? kBrightness : 0, on ? kBrightness : 0); }

void applyBaseState() { setWhiteLit(cameraPending || galleryNonEmpty); }

void setBlinkLit(bool on) {
    if (blinkColor == BlinkColor::kGreen) {
        setGreenLit(on);
    } else {
        setModeColorLit(on);
    }
}

void startBlink(uint8_t count, BlinkColor color, unsigned long onMs = kBlinkOnMs) {
    blinkColor = color;
    blinkOnMs = onMs;
    blinking = true;
    blinkLit = true;
    setBlinkLit(true);
    blinkHalfStepsLeft = count * 2 - 1;  // remaining: off,on,off,on,...,off
    blinkNextMs = millis() + onMs;
}

}  // namespace

void begin() { setWhiteLit(false); }

void notifyNewImage() { startBlink(kNewImageBlinkCount, BlinkColor::kModeColor); }

void notifyPrintDone() { startBlink(kPrintDoneBlinkCount, BlinkColor::kGreen); }

void notifyShutterOk() { startBlink(kShutterBlinkCount, BlinkColor::kGreen, kShutterBlinkOnMs); }

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
    setBlinkLit(blinkLit);
    blinkHalfStepsLeft--;
    if (blinkHalfStepsLeft == 0) {
        blinking = false;
        applyBaseState();
        return;
    }
    blinkNextMs = millis() + (blinkLit ? blinkOnMs : kBlinkOffMs);
}

}  // namespace Led
