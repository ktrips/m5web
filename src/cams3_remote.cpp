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

// See cams3_remote.h's ConnMode doc comment / README.md's「ATOM Lite⇔CamS3
// 直結シャッター」section for the wiring this pin is for. Chosen from the
// ATOM Lite's otherwise-unused header pins — G32/G26 are CameraLink's UART
// to the M5StickV, G23/G33 default to the printer UART (both configurable,
// see printer.h), G27 drives the onboard LED, G39 is the button itself.
constexpr uint8_t kWiredTriggerPin = 25;
constexpr unsigned long kWiredPulseMs = 250;  // same duration atomMatrix.ino uses, for the same reason

Preferences prefs;
String currentHost = kDefaultHost;
ConnMode currentConnMode = ConnMode::kWifi;

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

const char *connModeName(ConnMode m) { return m == ConnMode::kWired ? "wired" : "wifi"; }
ConnMode connModeFromName(const String &s) { return s == "wired" ? ConnMode::kWired : ConnMode::kWifi; }

}  // namespace

void begin() {
    prefs.begin("m5web_cams3", false);
    currentHost = prefs.getString("host", kDefaultHost);
    if (currentHost.length() == 0) currentHost = kDefaultHost;
    currentConnMode = connModeFromName(prefs.getString("connMode", "wifi"));

    pinMode(kWiredTriggerPin, OUTPUT);
    digitalWrite(kWiredTriggerPin, HIGH);  // idle high — CamS3's GPIO0 is INPUT_PULLUP, active LOW
}

String host() { return currentHost; }

void setHost(const String &h) {
    currentHost = h.length() > 0 ? h : kDefaultHost;
    prefs.putString("host", currentHost);
}

ConnMode connMode() { return currentConnMode; }

void setConnMode(ConnMode m) {
    currentConnMode = m;
    prefs.putString("connMode", connModeName(m));
    Serial.printf("[cams3_remote] connection mode set to %s\n", connModeName(m));
}

bool triggerShutter() {
    if (currentConnMode == ConnMode::kWired) {
        Serial.println("[cams3_remote] wired mode: pulsing trigger pin LOW");
        digitalWrite(kWiredTriggerPin, LOW);
        delay(kWiredPulseMs);
        digitalWrite(kWiredTriggerPin, HIGH);
        return true;  // see ConnMode's doc comment — no way to confirm CamS3 actually captured
    }
    return postOrGet("/shutter", false);
}

bool reprintLatest() { return postOrGet("/api/gallery/reprint-latest", true); }

}  // namespace CamS3Remote
