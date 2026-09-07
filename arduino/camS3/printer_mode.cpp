#include "printer_mode.h"

#include <Preferences.h>

namespace PrinterMode {

namespace {
Preferences prefs;
Mode currentMode = Mode::kViaAtom;
String currentAtomHost = "m5web.local";
}  // namespace

void begin() {
    prefs.begin("m5cam_mode", false);
    currentMode = prefs.getUChar("mode", (uint8_t)Mode::kViaAtom) == (uint8_t)Mode::kDirect ? Mode::kDirect
                                                                                              : Mode::kViaAtom;
    currentAtomHost = prefs.getString("atomHost", "m5web.local");
}

Mode mode() { return currentMode; }

void setMode(Mode m) {
    prefs.putUChar("mode", (uint8_t)m);
    Serial.printf("[printer_mode] mode set to %s — restarting\n", m == Mode::kDirect ? "direct" : "via_atom");
    delay(200);  // let the log line and the HTTP response that triggered this actually go out first
    ESP.restart();
}

String atomHost() { return currentAtomHost; }

void setAtomHost(const String &host) {
    currentAtomHost = host.length() > 0 ? host : "m5web.local";
    prefs.putString("atomHost", currentAtomHost);
}

}  // namespace PrinterMode
