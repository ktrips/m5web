#include "haiku.h"

#include <Preferences.h>

namespace Haiku {
namespace {

Preferences prefs;
String poemTypeValue;
String authorValue;
String autoModeValue;
bool autoPrintValue;

}  // namespace

void begin() {
    prefs.begin("m5web_haiku", false);
    poemTypeValue = prefs.getString("poemType", "haiku");
    authorValue = prefs.getString("author", "");
    // A pre-existing "print" value (from the original combined auto-print
    // mode, before it was split into this autoMode field and the separate
    // autoPrint flag below) still generates, and — if autoPrint itself was
    // never explicitly stored yet — seeds autoPrint's default to true too,
    // so migrating doesn't silently drop the auto-print behavior someone
    // already had turned on.
    String storedAutoMode = prefs.getString("autoMode", "none");
    bool hadLegacyAutoPrint = storedAutoMode == "print";
    autoModeValue = storedAutoMode == "generate" || hadLegacyAutoPrint ? "generate" : "none";
    autoPrintValue = prefs.getBool("autoPrint", hadLegacyAutoPrint);
}

String poemType() { return poemTypeValue; }

void setPoemType(const String &type) {
    poemTypeValue = type == "poem" ? "poem" : "haiku";
    prefs.putString("poemType", poemTypeValue);
}

String author() { return authorValue; }

void setAuthor(const String &name) {
    authorValue = name;
    prefs.putString("author", authorValue);
}

String autoMode() { return autoModeValue; }

void setAutoMode(const String &mode) {
    autoModeValue = mode == "generate" ? "generate" : "none";
    prefs.putString("autoMode", autoModeValue);
}

bool autoPrint() { return autoPrintValue; }

void setAutoPrint(bool on) {
    autoPrintValue = on;
    prefs.putBool("autoPrint", autoPrintValue);
}

}  // namespace Haiku
