#include "web_server.h"

#include <LittleFS.h>
#include <WebServer.h>

#include "camera_link.h"
#include "printer.h"
#include "wifi_manager.h"

namespace WebServer_ {

namespace {

constexpr uint16_t kMaxHeightDots = 2000;  // ~250mm; keeps jobs to a sane length
constexpr size_t kMaxQrLength = 300;  // sanity cap; printer's symbol buffer is limited

WebServer server(80);

uint16_t pendingWidth = 0;
uint16_t pendingHeight = 0;
bool imageInProgress = false;

void sendPlain(int code, const String &body) {
    server.send(code, "text/plain", body);
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
    Printer::reset();
    Printer::printText(server.arg("text"));
    Printer::newLine(3);
    sendPlain(200, "OK");
}

void handlePrintTest() {
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
    sendPlain(200, "OK");
}

void handleImageUploadComplete() {
    if (pendingWidth == 0) {
        sendPlain(400, "call /api/print/image/begin first");
        return;
    }
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
            Printer::beginRaster(pendingWidth, pendingHeight);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (imageInProgress) {
            Printer::feedRasterChunk(upload.buf, upload.currentSize);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (imageInProgress) {
            Printer::endRaster();
            imageInProgress = false;
        }
    }
}

void handleCameraStatus() {
    CameraLink::Status s = CameraLink::status();
    String json = "{";
    json += "\"mode\":\"" + String(s.mode == CameraLink::Mode::kPreview ? "preview" : "auto") + "\",";
    json += "\"frameReady\":" + String(s.frameReady ? "true" : "false") + ",";
    json += "\"pendingPrint\":" + String(s.pendingPrint ? "true" : "false") + ",";
    json += "\"width\":" + String(s.width) + ",";
    json += "\"height\":" + String(s.height) + ",";
    json += "\"frameSeq\":" + String(s.frameSeq);
    json += "}";
    server.send(200, "application/json", json);
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
    CameraLink::setMode(m == "preview" ? CameraLink::Mode::kPreview : CameraLink::Mode::kAuto);
    sendPlain(200, "OK");
}

void handleCameraPrint() {
    if (!CameraLink::confirmPrint()) {
        sendPlain(400, "no pending frame");
        return;
    }
    sendPlain(200, "OK");
}

void handleCameraDiscard() {
    CameraLink::discardPending();
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

}  // namespace

void begin() {
    LittleFS.begin(true);

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
    server.on("/api/camera/print", HTTP_POST, handleCameraPrint);
    server.on("/api/camera/discard", HTTP_POST, handleCameraDiscard);
    server.on("/api/camera/frame", HTTP_GET, handleCameraFrame);

    server.begin();
}

void loop() { server.handleClient(); }

}  // namespace WebServer_
