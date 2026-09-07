#pragma once

#include <Arduino.h>

// Storage for the OpenAI API key used by the "俳句を作る" feature. The
// actual OpenAI network call happens entirely in the browser (see
// data/index.html's generateHaikuFromCanvas()), not on this board — the
// ATOM Lite (no PSRAM) can't reliably complete a TLS handshake to a
// CDN-fronted host like api.openai.com while it's also running as a
// WebServer/WiFi/camera-link stack (mbedTLS's handshake setup needs a
// contiguous heap block this board can't consistently produce; see git
// history for the debugging that led here). The browser has no such
// constraint, so it fetches the key from GET /api/openai/settings and
// calls OpenAI directly. This module just persists the key (NVS/
// Preferences) and hands it back to web_server.cpp for that endpoint.
namespace OpenAI {

void begin();

// True once an API key has been saved via setKey().
bool hasKey();

// Persisted across reboots (NVS/Preferences). An empty string clears it.
void setKey(const String &key);

// The raw key, or an empty string if none is set. Unlike the Wi-Fi
// password (write-only), this is exposed to the web UI on purpose — the
// browser needs it to call OpenAI directly.
String getKey();

}  // namespace OpenAI
