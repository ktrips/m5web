#pragma once

#include <Arduino.h>

// Decodes a JPEG file already sitting in LittleFS (see web_server.cpp's
// handleExternalPhotoChunk(), which streams an uploaded photo straight to
// flash rather than buffering it in RAM) and prints it — resized to
// Printer::kPrintWidthDots wide (aspect-preserving), adjusted by the same
// persisted 「デフォルト: 明るさ・コントラスト」 values (CameraLink::status())
// M5StickV frames and phone-uploaded photos are, dithered, with the same
// kind of date/time + optional caption every other print path stamps —
// then saves it to the gallery. Never holds more than one ~16-row decode
// band in RAM at a time (see jpeg_print.cpp's band-buffer design) rather
// than buffering the whole decoded image.
//
// Exists for external callers with an arbitrary-size/format photo — not
// already resized/dithered into Printer::kPrintWidthDots-wide 1bpp the
// way the m5webページ's own upload flow does client-side (browser Canvas)
// before calling /api/print/image. See /api/print/photo in
// web_server.cpp.
//
// UNTESTED ON REAL HARDWARE: JPEG decoding on this ESP32-PICO-D4 (no
// PSRAM) is exactly the kind of heavy lifting this project has otherwise
// deliberately avoided on-device — see openai.* / data/index.html's
// history of moving OpenAI's HTTPS calls to the browser after repeated
// on-device heap failures during TLS handshakes. This module carries
// that same risk forward deliberately, at the user's explicit request,
// rather than requiring a browser in the loop. If it fails with an
// out-of-memory error on real hardware, the fix is almost certainly to
// lower kDecodeWidthCap (jpeg_print.cpp) further; if TJpg_Decoder's
// actual installed-version API differs from what's called here
// (getFsJpgSize()/drawFsJpg()/setJpgScale()/setCallback(), all against a
// generic fs::FS — this couldn't be verified without real hardware),
// that's the other likely adjustment point.
namespace JpegPrint {

// `path` must already exist in LittleFS as a complete, closed file (the
// caller still owns deleting it afterward, success or failure either
// way). `label`/`location` are optional caption pieces — label e.g. a
// short caller-supplied tag (like M5StickV's on-device detection label),
// location e.g. a caller-supplied place name; both "" means the caption
// is just the timestamp (itself omitted too if the clock hasn't synced
// yet). Returns false with a human-readable reason in `error` on any
// failure: not a valid JPEG, an aspect ratio too tall for a
// Printer::kPrintWidthDots-wide print, out of memory, or a decode error.
bool printFromFile(const String &path, const String &label, const String &location, String &error);

// Fetches `url` via a plain HTTP GET, streams the response straight to a
// LittleFS temp file (capped at the same size the direct-upload path
// enforces, never buffered whole in RAM), then hands it to
// printFromFile() exactly as if it had arrived via the multipart upload
// path — see web_server.cpp's /api/print/photo, which offers both this
// (a `url` query param) and the direct-upload path. Deletes its temp
// file when done either way.
//
// `url` must start with "http://" — https:// is rejected outright, with
// a clear error explaining why: this board's TLS/heap headroom is
// exactly what forced OpenAI's own HTTPS calls onto the browser instead
// (see openai.* / the 俳句生成 section's history) — a WiFiClientSecure
// fetch here would carry the same risk. Host the photo over plain
// http:// (e.g. a LAN file server) if the source only serves https://.
bool fetchAndPrint(const String &url, const String &label, const String &location, String &error);

}  // namespace JpegPrint
