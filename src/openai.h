#pragma once

#include <Arduino.h>

// Minimal client for OpenAI's Chat Completions API (vision-capable models),
// used to turn a photo into a haiku (5-7-5, with a season word) on request
// from the web UI. All the image work happens in the browser — every photo
// already gets drawn to a <canvas> there (camera preview, upload preview),
// so the browser just calls canvas.toDataURL("image/png"), strips the
// "data:image/png;base64," prefix, and POSTs the rest to /api/haiku; this
// module only adds the OpenAI request/response plumbing around that.
namespace OpenAI {

void begin();

// True once an API key has been saved via setKey() — never exposes the key
// itself back to callers (or the web UI), only whether one is configured,
// the same "write-only" treatment WifiManager gives the saved Wi-Fi
// password.
bool hasKey();

// Persisted across reboots (NVS/Preferences). An empty string clears it.
void setKey(const String &key);

// Sends `base64Png` (no "data:image/png;base64," prefix — this adds it) to
// OpenAI with a fixed prompt asking for one 5-7-5 haiku (with a season
// word) matching the image, and fills `haikuOut` with the trimmed result.
// Returns false and fills `errorOut` with a short, user-facing reason on
// any failure (no key configured, no image, network/HTTP error, or an
// unparseable response) — never throws, and always returns within the
// module's internal timeout.
bool generateHaiku(const String &base64Png, String &haikuOut, String &errorOut);

}  // namespace OpenAI
