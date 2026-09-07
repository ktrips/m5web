#pragma once

#include <Arduino.h>

// Small bridge header: functions implemented in camS3.ino itself (the
// Arduino IDE's automatic-prototype generation only covers the .ino file
// — a separate .cpp in the same sketch folder, like web_server.cpp, needs
// an explicit declaration to call them) that the web server needs access
// to for the kViaAtom capture path and shutter-feedback blinks.

// Sends an already-captured JPEG file (Storage::fs() path) to
// PrinterMode::atomHost()'s /api/print/photo — used by own_camera.cpp's
// captureAndCommitNow()/confirmPrint() whenever PrinterMode is kViaAtom.
bool sendJpegFileToAtom(const String &path, String &resultMsg);

void blinkLed(int times, int onMs, int offMs);
void setLed(bool on);

// Scans nearby networks; returns a JSON array like
// [{"ssid":"...","rssi":-50,"open":true}, ...] — same shape m5web's own
// WifiManager::scanNetworksJson() returns.
String wifiScanJson();

// Attempts to join ssid/password (blocking up to ~8s) and, on success,
// saves it to NVS for future boots. Does NOT reboot — same pattern as
// m5web's WifiManager::connect().
bool wifiConnectAndSave(const String &ssid, const String &password);
