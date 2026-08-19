#include "openai.h"

#include <Preferences.h>

namespace OpenAI {
namespace {

Preferences prefs;
String apiKey;

}  // namespace

void begin() {
    prefs.begin("m5web_openai", false);
    apiKey = prefs.getString("apiKey", "");
}

bool hasKey() { return apiKey.length() > 0; }

void setKey(const String &key) {
    apiKey = key;
    prefs.putString("apiKey", apiKey);
    Serial.printf("[openai] API key %s\n", apiKey.length() > 0 ? "set" : "cleared");
}

String getKey() { return apiKey; }

}  // namespace OpenAI
