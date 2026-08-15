#pragma once

#include <Arduino.h>

// Connects the ATOM to the user's WiFi (station mode) so the phone and the
// printer share one network, matching the reachable-from-iPhone use case.
// If no credentials are saved yet, or the saved network can't be reached, it
// falls back to hosting its own AP ("m5web-setup") with a captive portal so
// the phone can be used to supply credentials — same recovery pattern as
// M5Stack's stock ATOM-PRINTER firmware.
namespace WifiManager {

enum class Mode { kConnecting, kStation, kAccessPoint };

void begin();
void loop();  // call every iteration of the main loop

Mode mode();
bool isConnected();
String localIP();
String ssid();
String apSSID();  // SoftAP SSID, valid once AP mode has started

// Scans nearby networks; returns a JSON array: [{"ssid":"...","rssi":-50,"open":true}, ...]
String scanNetworksJson();

// Attempts to join `ssid`/`password`, saving on success and switching out of
// AP mode. Blocks up to ~8s while connecting.
bool connect(const String &ssid, const String &password);

// Erases saved credentials and reboots into AP setup mode.
void forgetAndRestart();

}  // namespace WifiManager
