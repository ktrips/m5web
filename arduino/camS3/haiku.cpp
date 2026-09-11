#include "haiku.h"

#include <Preferences.h>

namespace Haiku {
namespace {

Preferences prefs;
String poemTypeValue;
String authorValue;
String autoModeValue;

}  // namespace

void begin() {
    prefs.begin("m5web_haiku", false);
    poemTypeValue = prefs.getString("poemType", "haiku");
    authorValue = prefs.getString("author", "");
    // A pre-existing "print" value (from before auto-print was removed —
    // see setAutoMode()) still generates, just no longer auto-prints; it's
    // not reset all the way to "none" so that migration doesn't silently
    // turn generation off for someone who had it on.
    String storedAutoMode = prefs.getString("autoMode", "none");
    autoModeValue = storedAutoMode == "generate" || storedAutoMode == "print" ? "generate" : "none";
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

}  // namespace Haiku
