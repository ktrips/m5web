#include "cams3_remote.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClient.h>

namespace CamS3Remote {

namespace {
constexpr const char *kDefaultHost = "m5cam.local";

// Deliberately short: this is called synchronously from the ATOM Lite's
// own button handler (see main.cpp's checkButton()), which blocks the
// whole WebServer loop for the duration — unlike camS3.ino's own 20s
// capture-and-forward budget (CamS3 waiting out its own capture+print/
// forward, not a human standing at a physical button expecting a
// response). Long enough for a real capture+print round trip on a
// healthy network, short enough that "CamS3 is off/unreachable" doesn't
// leave the ATOM feeling stuck for 20 seconds.
constexpr unsigned long kTimeoutMs = 10000;

Preferences prefs;
String currentHost = kDefaultHost;

bool postOrGet(const String &path, bool usePost) {
    WiFiClient client;
    HTTPClient http;
    String url = "http://" + currentHost + path;
    if (!http.begin(client, url)) {
        Serial.printf("[cams3_remote] could not start request to %s\n", url.c_str());
        return false;
    }
    http.setTimeout(kTimeoutMs);
    int code = usePost ? http.POST((uint8_t *)nullptr, 0) : http.GET();
    http.end();
    if (code < 200 || code >= 300) {
        Serial.printf("[cams3_remote] %s -> HTTP %d\n", url.c_str(), code);
        return false;
    }
    return true;
}
}  // namespace

void begin() {
    prefs.begin("m5web_cams3", false);
    currentHost = prefs.getString("host", kDefaultHost);
    if (currentHost.length() == 0) currentHost = kDefaultHost;
}

String host() { return currentHost; }

void setHost(const String &h) {
    currentHost = h.length() > 0 ? h : kDefaultHost;
    prefs.putString("host", currentHost);
}

bool triggerShutter() { return postOrGet("/shutter", false); }

bool reprintLatest() { return postOrGet("/api/gallery/reprint-latest", true); }

}  // namespace CamS3Remote
