#include "web_server.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>

#include "cams3_remote.h"
#include "camera_link.h"
#include "caption.h"
#include "clock.h"
#include "gallery.h"
#include "haiku.h"
#include "jpeg_capture.h"
#include "jpeg_print.h"
#include "led.h"
#include "openai.h"
#include "printer.h"
#include "wifi_manager.h"

namespace WebServer_ {

namespace {

constexpr uint16_t kMaxHeightDots = 2000;  // ~250mm; keeps jobs to a sane length
constexpr size_t kMaxQrLength = 300;  // sanity cap; printer's symbol buffer is limited

// When autoRefresh is on, M5Paper polls /api/gallery on this interval to
// notice new/removed photos — see ../m5paper/m5paper.ino's settings poll.
// autoRefresh defaults to off: M5Paper then only re-syncs the gallery on a
// button press instead of a timer. Both persisted so they survive a
// reboot, same pattern as camera_link.cpp's brightness/contrast prefs.
constexpr unsigned long kDefaultM5PaperPollMs = 1000;
constexpr unsigned long kMinM5PaperPollMs = 250;
constexpr unsigned long kMaxM5PaperPollMs = 60000;

WebServer server(80);
Preferences m5paperPrefs;
unsigned long m5paperPollMs = kDefaultM5PaperPollMs;
bool m5paperAutoRefresh = false;

// URL for the optional small QR-code watermark composited into the
// bottom-right corner of a print — see data/index.html's
// compositeQrWatermark(). Purely a browser-side concern (this board just
// persists the string), same pattern as Haiku's fields — empty (the
// default) means no watermark is drawn.
Preferences qrWatermarkPrefs;
String qrWatermarkUrl;

// URL for the optional external photo-forwarding webhook — see
// data/index.html's forwardPhotoIfConfigured(). Purely a browser-side
// concern like the QR watermark URL above (this board just persists the
// string and hands it back); empty (the default) means nothing is sent.
Preferences forwardPrefs;
String forwardUrl;

uint16_t pendingWidth = 0;
uint16_t pendingHeight = 0;
// Place name from the uploading browser's own Geolocation-derived badge
// (see data/index.html's initLocationBadge()/printImageBtn handler) — a
// browser-side concern like the QR watermark URL above, just passed
// through per-request instead of persisted, since it's only meaningful at
// the moment of this specific print. Appended after the time in the
// printed caption band (see Caption::stamp() call below) when non-empty;
// "" (the default — also the M5StickV/gallery-reprint caption paths,
// which never have a browser location to attach) omits it entirely, same
// as every other optional caption piece.
constexpr size_t kMaxLocationLen = 40;  // printed at a small fixed font size on a 384-dot-wide band
String pendingLocation;
bool imageInProgress = false;
uint16_t uploadGalleryId = 0;
uint16_t uploadBandHeight = 0;  // extra caption rows appended after this upload's image, 0 if none

// /api/print/photo (+ /api/print/photo/url) — lets an external caller
// (not this project's own browser page) send an arbitrary JPEG and have
// it printed the same way an uploaded photo is, without needing to
// replicate the browser's own resize/dither step first. Streamed
// straight to a LittleFS temp file (never buffered whole in RAM — a
// phone-camera JPEG can be several MB) and capped well below that, then
// handed to JpegPrint::printFromFile() once the upload completes. See
// jpeg_print.h for why this whole feature is flagged
// untested-on-real-hardware — confirmed on real hardware so far: a
// bodyless POST straight to /api/print/photo (this route, registered
// with an upload handler) resets the connection instead of responding,
// which is why the URL-fetch variant (also bodyless — just query params,
// no file) lives on the separate /api/print/photo/url route instead, see
// handleExternalPhotoByUrl() below.
constexpr const char *kExternalPhotoTmpPath = "/tmp_ext_photo.jpg";
constexpr size_t kMaxExternalPhotoBytes = 400 * 1024;  // callers must pre-resize/compress past this
bool externalPhotoOk = false;
size_t externalPhotoBytesWritten = 0;
File externalPhotoFile;

void sendPlain(int code, const String &body) {
    server.send(code, "text/plain", body);
}

// Minimal JSON string escaping for text that isn't a compile-time literal
// (the M5StickV's detection label over UART, the saved OpenAI API key) —
// keeps a stray quote, backslash, or newline from producing invalid JSON.
String jsonEscape(const char *s) {
    String out;
    for (const char *p = s; *p; p++) {
        if (*p == '"' || *p == '\\') {
            out += '\\';
            out += *p;
        } else if (*p == '\n') {
            out += "\\n";
        } else if ((unsigned char)*p >= 0x20) {
            out += *p;
        }
    }
    return out;
}

void handleRoot() {
    File f = LittleFS.open("/index.html", "r");
    if (!f) {
        sendPlain(500, "index.html missing — did you run `pio run -t uploadfs`?");
        return;
    }
    server.streamFile(f, "text/html");
    f.close();
}

const char *modeName() {
    switch (WifiManager::mode()) {
        case WifiManager::Mode::kStation: return "station";
        case WifiManager::Mode::kAccessPoint: return "ap";
        default: return "connecting";
    }
}

void handleStatus() {
    String json = "{";
    json += "\"mode\":\"" + String(modeName()) + "\",";
    json += "\"connected\":" + String(WifiManager::isConnected() ? "true" : "false") + ",";
    json += "\"ip\":\"" + WifiManager::localIP() + "\",";
    json += "\"ssid\":\"" + WifiManager::ssid() + "\",";
    json += "\"apSsid\":\"" + WifiManager::apSSID() + "\",";
    json += "\"printWidthDots\":" + String(Printer::kPrintWidthDots) + ",";
    json += "\"maxHeightDots\":" + String(kMaxHeightDots);
    json += "}";
    server.send(200, "application/json", json);
}

void handleWifiScan() {
    server.send(200, "application/json", WifiManager::scanNetworksJson());
}

void handleWifiConnect() {
    if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
        sendPlain(400, "ssid required");
        return;
    }
    String ssid = server.arg("ssid");
    String password = server.hasArg("password") ? server.arg("password") : "";
    bool ok = WifiManager::connect(ssid, password);
    sendPlain(ok ? 200 : 400, ok ? "OK" : "Failed to join that network");
}

void handlePrintText() {
    if (!server.hasArg("text") || server.arg("text").length() == 0) {
        sendPlain(400, "text required");
        return;
    }
    Serial.printf("[web] print text (%u chars)\n", server.arg("text").length());
    Printer::reset();
    Printer::printText(server.arg("text"));
    Printer::newLine(3);
    sendPlain(200, "OK");
}

void handlePrintTest() {
    Serial.println("[web] print test page");
    Printer::printTestPage();
    sendPlain(200, "OK");
}

void handlePrintQr() {
    if (!server.hasArg("url") || server.arg("url").length() == 0) {
        sendPlain(400, "url required");
        return;
    }
    String data = server.arg("url");
    if (data.length() > kMaxQrLength) {
        sendPlain(400, "url too long (max " + String(kMaxQrLength) + " chars)");
        return;
    }
    Serial.printf("[web] print QR: %s\n", data.c_str());
    Printer::reset();
    Printer::printQRCode(data);
    sendPlain(200, "OK");
}

void handleImageBegin() {
    if (!server.hasArg("w") || !server.hasArg("h")) {
        sendPlain(400, "w and h required");
        return;
    }
    uint16_t w = server.arg("w").toInt();
    uint16_t h = server.arg("h").toInt();
    if (w != Printer::kPrintWidthDots || h == 0 || h > kMaxHeightDots) {
        sendPlain(400, "width must be " + String(Printer::kPrintWidthDots) +
                           ", height 1-" + String(kMaxHeightDots));
        return;
    }
    pendingWidth = w;
    pendingHeight = h;
    pendingLocation = server.hasArg("location") ? server.arg("location") : "";
    if (pendingLocation.length() > kMaxLocationLen) pendingLocation = pendingLocation.substring(0, kMaxLocationLen);
    Serial.printf("[web] image reserved: %ux%u\n", w, h);
    sendPlain(200, "OK");
}

void handleImageUploadComplete() {
    if (pendingWidth == 0) {
        sendPlain(400, "call /api/print/image/begin first");
        return;
    }
    Serial.printf("[web] image printed: %ux%u\n", pendingWidth, pendingHeight);
    pendingWidth = 0;
    pendingHeight = 0;
    pendingLocation = "";
    sendPlain(200, "printed");
}

void handleImageUploadChunk() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        imageInProgress = (pendingWidth != 0 && pendingHeight != 0);
        if (imageInProgress) {
            Printer::reset();
            // A print-time timestamp is appended as extra rows after the
            // image (see Caption) — decided upfront so beginRaster()'s
            // declared height already accounts for it; phone uploads have
            // no detection label, so this is skipped entirely if the
            // clock hasn't synced yet rather than printing a blank band.
            uploadBandHeight = (Clock::isSynced() && pendingHeight > Caption::kBandHeight) ? Caption::kBandHeight : 0;
            Printer::beginRaster(pendingWidth, pendingHeight + uploadBandHeight);
            // Streamed straight to flash alongside the printer feed so a
            // tall (up to kMaxHeightDots) image never has to be buffered
            // whole in RAM just to also save it to the gallery.
            uploadGalleryId = Gallery::beginSave(pendingHeight);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (imageInProgress) {
            Printer::feedRasterChunk(upload.buf, upload.currentSize);
            if (uploadGalleryId != 0 && !Gallery::feedSave(upload.buf, upload.currentSize)) {
                Gallery::cancelSave();
                uploadGalleryId = 0;
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (imageInProgress) {
            if (uploadBandHeight > 0) {
                uint8_t band[Printer::kPrintWidthBytes * Caption::kBandHeight];
                String captionText = Clock::nowDateTime();
                if (pendingLocation.length() > 0) captionText += " " + pendingLocation;
                Caption::stamp(band, Printer::kPrintWidthBytes, captionText.c_str());
                Printer::feedRasterChunk(band, (size_t)Printer::kPrintWidthBytes * uploadBandHeight);
            }
            Printer::endRaster();
            if (uploadGalleryId != 0) {
                Gallery::endSave();
                Led::notifyNewImage();
            }
            imageInProgress = false;
        }
    }
}

// See kExternalPhotoTmpPath's doc comment above. label/location come from
// query-string args (available immediately, unlike multipart form fields,
// which the ESP32 WebServer library only finishes parsing once the whole
// request — including the file body — has been read), matching how
// handleImageBegin() takes `location` the same way for the phone-upload
// path.
void handleExternalPhotoChunk() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        externalPhotoBytesWritten = 0;
        externalPhotoFile = LittleFS.open(kExternalPhotoTmpPath, "w");
        externalPhotoOk = (bool)externalPhotoFile;
        if (!externalPhotoOk) Serial.println("[web] external photo: failed to open temp file");
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (externalPhotoOk) {
            externalPhotoBytesWritten += upload.currentSize;
            if (externalPhotoBytesWritten > kMaxExternalPhotoBytes) {
                Serial.println("[web] external photo: exceeded size cap, aborting");
                externalPhotoOk = false;
                externalPhotoFile.close();
                LittleFS.remove(kExternalPhotoTmpPath);
            } else {
                externalPhotoFile.write(upload.buf, upload.currentSize);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (externalPhotoFile) externalPhotoFile.close();
    }
}

// Deliberately a *separate* route from /api/print/photo (see below), not
// a branch inside handleExternalPhotoComplete() keyed on `url` — the
// ESP32 WebServer library treats any route registered with an upload
// handler (server.on(uri, method, fn, uploadFn), used for the multipart
// path below) as expecting a multipart/form-data body, and a request
// with no such body (exactly what this `url`-only call is — no file, no
// Content-Type) can hang/reset the connection instead of reaching `fn`
// at all. Confirmed against real hardware: a bodyless POST straight to
// /api/print/photo (e.g. testing connectivity with `curl -X POST
// .../api/print/photo` and no data) reset the connection instead of
// returning a response. Registered as a plain server.on(uri, method, fn)
// (no upload handler at all) so it can never hit that code path.
void handleExternalPhotoByUrl() {
    String label = server.hasArg("label") ? server.arg("label") : "";
    String location = server.hasArg("location") ? server.arg("location") : "";
    if (!server.hasArg("url")) {
        sendPlain(400, "url required");
        return;
    }
    String error;
    bool ok = JpegPrint::fetchAndPrint(server.arg("url"), label, location, error);
    if (!ok) {
        Serial.printf("[web] external photo (url): %s\n", error.c_str());
        sendPlain(400, error);
        return;
    }
    Serial.println("[web] external photo (url) printed");
    sendPlain(200, "printed");
}

void handleExternalPhotoComplete() {
    String label = server.hasArg("label") ? server.arg("label") : "";
    String location = server.hasArg("location") ? server.arg("location") : "";
    String error;

    if (!externalPhotoOk) {
        sendPlain(413, "upload failed or exceeded " + String(kMaxExternalPhotoBytes / 1024) +
                           "KB — please resize/compress the photo before sending");
        return;
    }
    bool ok = JpegPrint::printFromFile(kExternalPhotoTmpPath, label, location, error);
    LittleFS.remove(kExternalPhotoTmpPath);
    if (!ok) {
        Serial.printf("[web] external photo: %s\n", error.c_str());
        sendPlain(400, error);
        return;
    }
    Serial.println("[web] external photo printed");
    sendPlain(200, "printed");
}

void handleCameraStatus() {
    CameraLink::Status s = CameraLink::status();
    String json = "{";
    String modeStr = s.mode == CameraLink::Mode::kPreview ? "preview" : "auto";
    json += "\"mode\":\"" + modeStr + "\",";
    json += "\"frameReady\":" + String(s.frameReady ? "true" : "false") + ",";
    json += "\"pendingPrint\":" + String(s.pendingPrint ? "true" : "false") + ",";
    json += "\"width\":" + String(s.width) + ",";
    json += "\"height\":" + String(s.height) + ",";
    json += "\"frameSeq\":" + String(s.frameSeq) + ",";
    json += "\"brightness\":" + String(s.brightness) + ",";
    json += "\"contrast\":" + String(s.contrast) + ",";
    json += "\"rotationDeg\":" + String(s.rotationDeg) + ",";
    json += "\"label\":\"" + jsonEscape(s.label) + "\"";
    json += "}";
    server.send(200, "application/json", json);
}

void handleCameraSettings() {
    if (!server.hasArg("brightness") || !server.hasArg("contrast")) {
        sendPlain(400, "brightness and contrast required");
        return;
    }
    CameraLink::setAdjust((int)server.arg("brightness").toInt(), (int)server.arg("contrast").toInt());
    sendPlain(200, "OK");
}

void handleCameraModeSet() {
    if (!server.hasArg("mode")) {
        sendPlain(400, "mode required");
        return;
    }
    String m = server.arg("mode");
    if (m != "auto" && m != "preview") {
        sendPlain(400, "mode must be 'auto' or 'preview'");
        return;
    }
    Serial.printf("[web] camera mode set to '%s'\n", m.c_str());
    CameraLink::setMode(m == "preview" ? CameraLink::Mode::kPreview : CameraLink::Mode::kAuto);
    sendPlain(200, "OK");
}

void handleCameraPrint() {
    if (!CameraLink::printLastFrame()) {
        sendPlain(400, "no camera frame yet");
        return;
    }
    Serial.println("[web] camera frame printed");
    sendPlain(200, "OK");
}

void handleCameraDiscard() {
    CameraLink::discardPending();
    Serial.println("[web] camera frame discarded");
    sendPlain(200, "OK");
}

void handleCameraRotate() {
    if (!CameraLink::rotate()) {
        sendPlain(400, "no camera frame yet");
        return;
    }
    Serial.println("[web] camera frame rotated 90deg CW");
    sendPlain(200, "OK");
}

void handleCameraRotateDefault() {
    CameraLink::rotateDefaultBy90();
    Serial.println("[web] camera default rotation +90deg");
    sendPlain(200, "OK");
}

void handleCameraFrame() {
    const uint8_t *data = CameraLink::frameData();
    size_t len = CameraLink::frameDataLen();
    if (!data || len == 0) {
        sendPlain(404, "no frame yet");
        return;
    }
    CameraLink::Status s = CameraLink::status();
    server.sendHeader("X-Frame-Width", String(s.width));
    server.sendHeader("X-Frame-Height", String(s.height));
    server.setContentLength(len);
    server.send(200, "application/octet-stream", "");  // headers + empty body
    // Body written directly via the underlying client rather than
    // server.sendContent(), since `data` is raw binary (embedded 0x00
    // bytes) and Arduino's String isn't safe to route it through —
    // WiFiClient::write(const uint8_t*, size_t) is the same Print-class
    // primitive already used for HardwareSerial writes in printer.cpp.
    server.client().write(data, len);
}

// GET /capture — deliberately no /api prefix, matching the plain
// "snapshot URL" convention generic camera integrations (Home
// Assistant's generic camera platform, motion-detection tools, etc.)
// expect: hit a URL, get an image/jpeg body back, nothing else involved.
// See jpeg_capture.h for what this actually contains (a downscaled,
// JPEG-compressed version of the same dithered black/white bitmap
// /api/camera/frame serves raw) and its untested-on-real-hardware
// caveat.
void handleCapture() {
    const uint8_t *buf = nullptr;
    size_t len = 0;
    String error;
    if (!JpegCapture::encodeCurrentFrame(buf, len, error)) {
        sendPlain(404, error);
        return;
    }
    server.setContentLength(len);
    server.send(200, "image/jpeg", "");  // headers + empty body
    server.client().write(buf, len);     // same raw-binary-write pattern as handleCameraFrame() above
}

void handleGalleryList() {
    Gallery::Entry entries[Gallery::kMaxEntries];
    size_t count = Gallery::list(entries, Gallery::kMaxEntries);

    String json = "{\"maxEntries\":" + String((unsigned)Gallery::kMaxEntries) + ",\"entries\":[";
    for (size_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"id\":" + String(entries[i].id) + ",\"height\":" + String(entries[i].height) +
                ",\"bytes\":" + String((unsigned)entries[i].bytes) +
                ",\"label\":\"" + jsonEscape(entries[i].label) + "\"" +
                ",\"savedAt\":\"" + jsonEscape(entries[i].savedAt) + "\"}";
    }
    json += "]}";
    server.send(200, "application/json", json);
}

void handleGalleryFrame() {
    if (!server.hasArg("id")) {
        sendPlain(400, "id required");
        return;
    }
    uint16_t id = (uint16_t)server.arg("id").toInt();
    String path;
    uint16_t height = 0;
    if (!Gallery::frameInfo(id, path, height)) {
        sendPlain(404, "not found");
        return;
    }
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendPlain(500, "failed to open file");
        return;
    }
    server.sendHeader("X-Frame-Width", String(Printer::kPrintWidthDots));
    server.sendHeader("X-Frame-Height", String(height));
    server.setContentLength(f.size());
    server.send(200, "application/octet-stream", "");
    uint8_t chunk[512];
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));
        if (n <= 0) break;
        server.client().write(chunk, (size_t)n);
    }
    f.close();
}

void handleGalleryPrint() {
    if (!server.hasArg("id")) {
        sendPlain(400, "id required");
        return;
    }
    uint16_t id = (uint16_t)server.arg("id").toInt();
    if (!Gallery::print(id)) {
        sendPlain(404, "not found");
        return;
    }
    Serial.printf("[web] gallery #%u printed\n", id);
    sendPlain(200, "OK");
}

void handleGalleryDelete() {
    if (!server.hasArg("id")) {
        sendPlain(400, "id required");
        return;
    }
    uint16_t id = (uint16_t)server.arg("id").toInt();
    if (!Gallery::remove(id)) {
        sendPlain(404, "not found");
        return;
    }
    Serial.printf("[web] gallery #%u deleted\n", id);
    sendPlain(200, "OK");
}

void handleM5PaperSettingsGet() {
    String json = "{\"autoRefresh\":" + String(m5paperAutoRefresh ? "true" : "false") +
                  ",\"pollIntervalMs\":" + String(m5paperPollMs) + "}";
    server.send(200, "application/json", json);
}

void handleM5PaperSettingsSet() {
    if (!server.hasArg("pollIntervalMs")) {
        sendPlain(400, "pollIntervalMs required");
        return;
    }
    long ms = server.arg("pollIntervalMs").toInt();
    if (ms < (long)kMinM5PaperPollMs || ms > (long)kMaxM5PaperPollMs) {
        sendPlain(400, "pollIntervalMs must be " + String(kMinM5PaperPollMs) + "-" + String(kMaxM5PaperPollMs));
        return;
    }
    m5paperPollMs = (unsigned long)ms;
    m5paperPrefs.putULong("pollMs", m5paperPollMs);

    if (server.hasArg("autoRefresh")) {
        m5paperAutoRefresh = server.arg("autoRefresh").toInt() != 0;
        m5paperPrefs.putBool("autoRefresh", m5paperAutoRefresh);
    }

    Serial.printf("[web] m5paper autoRefresh=%d pollIntervalMs=%lu\n", m5paperAutoRefresh, m5paperPollMs);
    sendPlain(200, "OK");
}

// Unlike the Wi-Fi password (write-only, see WifiManager), this hands the
// actual key back to the browser — the OpenAI call itself now runs
// client-side (see data/index.html's generateHaikuFromCanvas()) rather
// than from this board, since the ATOM Lite (no PSRAM) can't reliably
// complete a TLS handshake to a CDN-fronted host like api.openai.com
// while also running as a WebServer/WiFi/camera-link stack — mbedTLS's
// handshake setup needs a contiguous heap block this board couldn't
// consistently produce, even after image downscaling, connect-timeout
// tuning, and deferring the call out of the upload request (see git
// history for the debugging that led here). The browser has none of
// those constraints.
void handleOpenAISettingsGet() {
    bool configured = OpenAI::hasKey();
    String json = "{\"configured\":" + String(configured ? "true" : "false") + "}";
    if (configured) {
        json = "{\"configured\":true,\"apiKey\":\"" + jsonEscape(OpenAI::getKey().c_str()) + "\"}";
    }
    server.send(200, "application/json", json);
}

void handleOpenAISettingsSet() {
    if (!server.hasArg("apiKey")) {
        sendPlain(400, "apiKey required");
        return;
    }
    OpenAI::setKey(server.arg("apiKey"));
    sendPlain(200, "OK");
}

// Both fields are pure browser-side concerns (poem form and print-time
// author credit, see data/index.html) — this board just persists them so
// every device viewing this page's Web UI agrees, same as every other
// setting card.
void handleHaikuSettingsGet() {
    String json = "{\"poemType\":\"" + jsonEscape(Haiku::poemType().c_str()) + "\"" +
                  ",\"author\":\"" + jsonEscape(Haiku::author().c_str()) + "\"" +
                  ",\"autoMode\":\"" + jsonEscape(Haiku::autoMode().c_str()) + "\"}";
    server.send(200, "application/json", json);
}

void handleHaikuSettingsSet() {
    if (server.hasArg("poemType")) Haiku::setPoemType(server.arg("poemType"));
    if (server.hasArg("author")) Haiku::setAuthor(server.arg("author"));
    if (server.hasArg("autoMode")) Haiku::setAutoMode(server.arg("autoMode"));
    sendPlain(200, "OK");
}

void handleQrWatermarkSettingsGet() {
    server.send(200, "application/json", "{\"url\":\"" + jsonEscape(qrWatermarkUrl.c_str()) + "\"}");
}

void handleQrWatermarkSettingsSet() {
    qrWatermarkUrl = server.hasArg("url") ? server.arg("url") : "";
    qrWatermarkPrefs.putString("url", qrWatermarkUrl);
    sendPlain(200, "OK");
}

void handleForwardSettingsGet() {
    server.send(200, "application/json", "{\"url\":\"" + jsonEscape(forwardUrl.c_str()) + "\"}");
}

void handleForwardSettingsSet() {
    forwardUrl = server.hasArg("url") ? server.arg("url") : "";
    forwardPrefs.putString("url", forwardUrl);
    sendPlain(200, "OK");
}

// GPIO numbers only — an ESP32-family chip's usable range never exceeds
// this, and it's a cheap sanity check before handing the value to
// HardwareSerial::begin() (an out-of-range pin there just silently fails
// to talk to the printer, which is much harder to debug than a 400 here).
constexpr uint8_t kMaxGpioNum = 48;

void handlePrinterSettingsGet() {
    uint8_t tx = 0, rx = 0;
    Printer::currentPins(tx, rx);
    server.send(200, "application/json", "{\"txPin\":" + String(tx) + ",\"rxPin\":" + String(rx) + "}");
}

void handlePrinterSettingsSet() {
    if (!server.hasArg("txPin") || !server.hasArg("rxPin")) {
        sendPlain(400, "txPin and rxPin required");
        return;
    }
    int tx = server.arg("txPin").toInt();
    int rx = server.arg("rxPin").toInt();
    if (tx < 0 || tx > kMaxGpioNum || rx < 0 || rx > kMaxGpioNum) {
        sendPlain(400, "txPin/rxPin must be 0-" + String(kMaxGpioNum));
        return;
    }
    if (!Printer::setPins((uint8_t)tx, (uint8_t)rx)) {
        sendPlain(400, "txPin and rxPin must differ");
        return;
    }
    sendPlain(200, "OK");
}

void handleCamS3SettingsGet() {
    String connMode = CamS3Remote::connMode() == CamS3Remote::ConnMode::kWired ? "wired" : "wifi";
    server.send(200, "application/json",
                "{\"host\":\"" + jsonEscape(CamS3Remote::host().c_str()) + "\",\"connMode\":\"" + connMode + "\"}");
}

void handleCamS3SettingsSet() {
    if (server.hasArg("host")) CamS3Remote::setHost(server.arg("host"));
    if (server.hasArg("connMode")) {
        CamS3Remote::setConnMode(server.arg("connMode") == "wired" ? CamS3Remote::ConnMode::kWired
                                                                     : CamS3Remote::ConnMode::kWifi);
    }
    sendPlain(200, "OK");
}

}  // namespace

void begin() {
    LittleFS.begin(true);

    m5paperPrefs.begin("m5web_paper", false);
    m5paperPollMs = m5paperPrefs.getULong("pollMs", kDefaultM5PaperPollMs);
    m5paperAutoRefresh = m5paperPrefs.getBool("autoRefresh", false);

    qrWatermarkPrefs.begin("m5web_qrwm", false);
    qrWatermarkUrl = qrWatermarkPrefs.getString("url", "");

    forwardPrefs.begin("m5web_fwd", false);
    forwardUrl = forwardPrefs.getString("url", "");

    server.on("/", HTTP_GET, handleRoot);
    server.onNotFound(handleRoot);  // catch-all keeps AP captive-portal probes on the setup page

    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/api/wifi", HTTP_POST, handleWifiConnect);
    server.on("/api/print/text", HTTP_POST, handlePrintText);
    server.on("/api/print/qr", HTTP_POST, handlePrintQr);
    server.on("/api/print/test", HTTP_POST, handlePrintTest);
    server.on("/api/print/image/begin", HTTP_POST, handleImageBegin);
    server.on("/api/print/image", HTTP_POST, handleImageUploadComplete, handleImageUploadChunk);
    server.on("/api/print/photo", HTTP_POST, handleExternalPhotoComplete, handleExternalPhotoChunk);
    server.on("/api/print/photo/url", HTTP_POST, handleExternalPhotoByUrl);
    server.on("/api/camera/status", HTTP_GET, handleCameraStatus);
    server.on("/api/camera/mode", HTTP_POST, handleCameraModeSet);
    server.on("/api/camera/settings", HTTP_POST, handleCameraSettings);
    server.on("/api/camera/print", HTTP_POST, handleCameraPrint);
    server.on("/api/camera/discard", HTTP_POST, handleCameraDiscard);
    server.on("/api/camera/rotate", HTTP_POST, handleCameraRotate);
    server.on("/api/camera/rotate-default", HTTP_POST, handleCameraRotateDefault);
    server.on("/api/camera/frame", HTTP_GET, handleCameraFrame);
    server.on("/capture", HTTP_GET, handleCapture);
    server.on("/api/gallery", HTTP_GET, handleGalleryList);
    server.on("/api/gallery/frame", HTTP_GET, handleGalleryFrame);
    server.on("/api/gallery/print", HTTP_POST, handleGalleryPrint);
    server.on("/api/gallery/delete", HTTP_POST, handleGalleryDelete);
    server.on("/api/m5paper/settings", HTTP_GET, handleM5PaperSettingsGet);
    server.on("/api/m5paper/settings", HTTP_POST, handleM5PaperSettingsSet);
    server.on("/api/openai/settings", HTTP_GET, handleOpenAISettingsGet);
    server.on("/api/openai/settings", HTTP_POST, handleOpenAISettingsSet);
    server.on("/api/haiku/settings", HTTP_GET, handleHaikuSettingsGet);
    server.on("/api/haiku/settings", HTTP_POST, handleHaikuSettingsSet);
    server.on("/api/qrwatermark/settings", HTTP_GET, handleQrWatermarkSettingsGet);
    server.on("/api/qrwatermark/settings", HTTP_POST, handleQrWatermarkSettingsSet);
    server.on("/api/forward/settings", HTTP_GET, handleForwardSettingsGet);
    server.on("/api/forward/settings", HTTP_POST, handleForwardSettingsSet);
    server.on("/api/printer/settings", HTTP_GET, handlePrinterSettingsGet);
    server.on("/api/printer/settings", HTTP_POST, handlePrinterSettingsSet);
    server.on("/api/cams3/settings", HTTP_GET, handleCamS3SettingsGet);
    server.on("/api/cams3/settings", HTTP_POST, handleCamS3SettingsSet);

    server.begin();
}

void loop() { server.handleClient(); }

}  // namespace WebServer_
