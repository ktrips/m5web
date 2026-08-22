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
    autoModeValue = prefs.getString("autoMode", "none");
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
    autoModeValue = (mode == "generate" || mode == "print") ? mode : "none";
    prefs.putString("autoMode", autoModeValue);
}

}  // namespace Haiku
