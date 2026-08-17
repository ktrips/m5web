#include "wifi_manager.h"

#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>

#include "clock.h"

namespace WifiManager {

namespace {
constexpr uint8_t kDnsPort = 53;
const IPAddress kApIP(192, 168, 4, 1);

Preferences prefs;
DNSServer dnsServer;
Mode currentMode = Mode::kConnecting;
String currentApSsid;
unsigned long lastReconnectAttemptMs = 0;

String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void startMDNS() {
    MDNS.end();
    if (MDNS.begin("m5web")) {
        MDNS.addService("http", "tcp", 80);
    }
}

void onStationConnected() {
    startMDNS();
    Clock::begin();  // non-blocking; syncs in the background over the next few seconds
}

void startAP() {
    WiFi.mode(WIFI_AP_STA);
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char apName[24];
    snprintf(apName, sizeof(apName), "m5web-setup-%02X%02X", mac[4], mac[5]);
    currentApSsid = apName;
    WiFi.softAP(apName);
    dnsServer.start(kDnsPort, "*", kApIP);
    currentMode = Mode::kAccessPoint;
    Serial.printf("[wifi] AP mode: ssid=%s ip=%s\n", apName, kApIP.toString().c_str());
}

bool tryStationConnect(const String &ssid, const String &password, unsigned long timeoutMs) {
    Serial.printf("[wifi] connecting to '%s'...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname("m5web");
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            Serial.printf("[wifi] connect to '%s' timed out\n", ssid.c_str());
            return false;
        }
        delay(200);
    }
    Serial.printf("[wifi] connected: ssid=%s ip=%s\n", ssid.c_str(), WiFi.localIP().toString().c_str());
    return true;
}

}  // namespace

void begin() {
    prefs.begin("m5web", false);
    String savedSsid = prefs.getString("ssid", "");
    String savedPass = prefs.getString("pass", "");

    if (savedSsid.length() > 0 && tryStationConnect(savedSsid, savedPass, 15000)) {
        currentMode = Mode::kStation;
        onStationConnected();
        return;
    }
    startAP();
}

void loop() {
    if (currentMode == Mode::kAccessPoint) {
        dnsServer.processNextRequest();
    } else if (currentMode == Mode::kStation && WiFi.status() != WL_CONNECTED) {
        if (millis() - lastReconnectAttemptMs > 10000) {
            lastReconnectAttemptMs = millis();
            Serial.println("[wifi] disconnected, reconnecting...");
            WiFi.reconnect();
        }
    }
}

Mode mode() { return currentMode; }

bool isConnected() {
    return currentMode == Mode::kStation && WiFi.status() == WL_CONNECTED;
}

String localIP() {
    if (currentMode == Mode::kStation) return WiFi.localIP().toString();
    return kApIP.toString();
}

String ssid() {
    if (currentMode == Mode::kStation) return WiFi.SSID();
    return "";
}

String apSSID() { return currentApSsid; }

String scanNetworksJson() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    return json;
}

bool connect(const String &newSsid, const String &newPassword) {
    if (!tryStationConnect(newSsid, newPassword, 8000)) {
        // Leave whatever mode we were in (usually AP) running so the user can retry.
        if (currentMode != Mode::kAccessPoint) startAP();
        return false;
    }
    prefs.putString("ssid", newSsid);
    prefs.putString("pass", newPassword);
    if (currentMode == Mode::kAccessPoint) {
        dnsServer.stop();
        WiFi.softAPdisconnect(true);
    }
    currentMode = Mode::kStation;
    onStationConnected();
    return true;
}

void forgetAndRestart() {
    Serial.println("[wifi] forgetting saved credentials, restarting...");
    prefs.remove("ssid");
    prefs.remove("pass");
    prefs.end();
    ESP.restart();
}

}  // namespace WifiManager
