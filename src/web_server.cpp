#include "web_server.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <WebServer.h>

#include "camera_link.h"
#include "caption.h"
#include "clock.h"
#include "gallery.h"
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

uint16_t pendingWidth = 0;
uint16_t pendingHeight = 0;
bool imageInProgress = false;
uint16_t uploadGalleryId = 0;
uint16_t uploadBandHeight = 0;  // extra caption rows appended after this upload's image, 0 if none

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
                Caption::stamp(band, Printer::kPrintWidthBytes, Clock::nowHHMM().c_str());
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

void handleCameraStatus() {
    CameraLink::Status s = CameraLink::status();
    String json = "{";
    String modeStr = s.mode == CameraLink::Mode::kPreview     ? "preview"
                     : s.mode == CameraLink::Mode::kAutoHaiku ? "autoHaiku"
                                                               : "auto";
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
    if (m != "auto" && m != "preview" && m != "autoHaiku") {
        sendPlain(400, "mode must be 'auto', 'preview', or 'autoHaiku'");
        return;
    }
    Serial.printf("[web] camera mode set to '%s'\n", m.c_str());
    CameraLink::Mode mode = m == "preview"     ? CameraLink::Mode::kPreview
                             : m == "autoHaiku" ? CameraLink::Mode::kAutoHaiku
                                                 : CameraLink::Mode::kAuto;
    CameraLink::setMode(mode);
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

}  // namespace

void begin() {
    LittleFS.begin(true);

    m5paperPrefs.begin("m5web_paper", false);
    m5paperPollMs = m5paperPrefs.getULong("pollMs", kDefaultM5PaperPollMs);
    m5paperAutoRefresh = m5paperPrefs.getBool("autoRefresh", false);

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
    server.on("/api/camera/status", HTTP_GET, handleCameraStatus);
    server.on("/api/camera/mode", HTTP_POST, handleCameraModeSet);
    server.on("/api/camera/settings", HTTP_POST, handleCameraSettings);
    server.on("/api/camera/print", HTTP_POST, handleCameraPrint);
    server.on("/api/camera/discard", HTTP_POST, handleCameraDiscard);
    server.on("/api/camera/rotate", HTTP_POST, handleCameraRotate);
    server.on("/api/camera/rotate-default", HTTP_POST, handleCameraRotateDefault);
    server.on("/api/camera/frame", HTTP_GET, handleCameraFrame);
    server.on("/api/gallery", HTTP_GET, handleGalleryList);
    server.on("/api/gallery/frame", HTTP_GET, handleGalleryFrame);
    server.on("/api/gallery/print", HTTP_POST, handleGalleryPrint);
    server.on("/api/gallery/delete", HTTP_POST, handleGalleryDelete);
    server.on("/api/m5paper/settings", HTTP_GET, handleM5PaperSettingsGet);
    server.on("/api/m5paper/settings", HTTP_POST, handleM5PaperSettingsSet);
    server.on("/api/openai/settings", HTTP_GET, handleOpenAISettingsGet);
    server.on("/api/openai/settings", HTTP_POST, handleOpenAISettingsSet);

    server.begin();
}

void loop() { server.handleClient(); }

}  // namespace WebServer_
