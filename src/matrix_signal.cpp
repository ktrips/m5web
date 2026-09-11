#include "matrix_signal.h"

namespace MatrixSignal {

namespace {
// See matrix_signal.h's doc comment for the wiring these are for. Chosen
// from the ATOM Lite's remaining free extension-header pins (G19 is also
// free but may be physically wired to the printer TTL connector's CTS pin
// even though this firmware doesn't use it — see README.md's hardware
// notes — so G21/G22 are the safer pick).
constexpr uint8_t kFlashPin = 21;
constexpr uint8_t kSuccessPin = 22;
constexpr unsigned long kSuccessPulseMs = 100;
}  // namespace

void begin() {
    pinMode(kFlashPin, OUTPUT);
    pinMode(kSuccessPin, OUTPUT);
    digitalWrite(kFlashPin, LOW);
    digitalWrite(kSuccessPin, LOW);
}

void setFlash(bool on) { digitalWrite(kFlashPin, on ? HIGH : LOW); }

void pulseSuccess() {
    digitalWrite(kSuccessPin, HIGH);
    delay(kSuccessPulseMs);
    digitalWrite(kSuccessPin, LOW);
}

}  // namespace MatrixSignal
