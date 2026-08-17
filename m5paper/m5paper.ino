// m5paper.ino — M5Paper companion display for m5web's M5StickV camera
// preview/print workflow.
//
// Lets you review the M5StickV's pending camera frame and print/discard it
// from M5Paper's e-ink screen + touchscreen, instead of (or alongside) the
// phone browser's "M5StickVカメラ" card on the m5web page. Talks to the
// same ATOM Lite HTTP API over WiFi — M5Paper has its own WiFi radio
// (unlike the M5StickV, which has none and so uses a direct UART link
// instead; see ../src/camera_link.* for that path). No changes to the
// ATOM Lite firmware are needed: this is purely a new client of the
// already-existing /api/camera/* endpoints (see ../README.md's API table).
//
// IMPORTANT: this REPLACES whatever firmware is currently on the M5Paper
// when flashed — including the factory-stock "AP mode + image upload"
// app, since a device can only run one firmware image at a time. This
// sketch does not attempt to reimplement that stock app. If you still
// want that capability, reflash the stock firmware when you need it (or
// use a second M5Paper unit dedicated to this sketch).
//
// Requires two libraries via Arduino Library Manager: "M5EPD" (M5Stack)
// and "ArduinoJson" (Benoit Blanchon).
//
// NOTE: written against M5EPD's documented API but not tested on real
// M5Paper hardware. Likely spots to double-check/adjust if something's
// off:
//   - Touch coordinates vs. display rotation: the canvas is drawn rotated
//     90° (portrait, 540x960 — see setup()'s M5.EPD.SetRotation(90)), but
//     M5.TP.readFinger() may report raw/native (960x540 landscape) panel
//     coordinates depending on your M5EPD library version, which would
//     make handleTouch()'s button hit-testing miss. If taps don't
//     register where expected, that's the first thing to check — either
//     swap/remap p.x/p.y here, or find whichever M5EPD touch-rotation
//     call your version exposes (see M5Stack's own M5EPD BasicTouch.ino
//     example for the touch API in general, tp_finger_t included).
//   - Canvas color polarity (0=white / 15=black is assumed below — a
//     4-bit grayscale value per M5EPD_Canvas convention).
//   - EPD update mode constants (UPDATE_MODE_GC16 etc.) come from
//     M5EPD.h; full-quality GC16 is used throughout since this app
//     updates rarely (on poll changes / button taps), not continuously,
//     so the flash/ghosting cost of a full refresh isn't a concern here.

#include <M5EPD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---- configuration: edit before flashing ----
const char *WIFI_SSID = "YOUR_WIFI_SSID";
const char *WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
// The ATOM Lite's mDNS hostname. ESP32 Arduino's HTTPClient generally
// resolves ".local" fine, but if it doesn't on your setup, replace this
// with the ATOM's IP address instead (see README.md / pio device monitor).
const char *M5WEB_HOST = "m5web.local";

constexpr unsigned long POLL_INTERVAL_MS = 3000;

// Matches CameraLink::kMaxHeightDots (src/camera_link.cpp) * 384/8 dots —
// the largest frame the ATOM Lite will ever report, so a single
// fixed-size buffer covers every case without needing to grow it.
constexpr size_t kFrameBufMaxLen = 48 * 800;

M5EPD_Canvas canvas(&M5.EPD);

// ---- layout (portrait: M5Paper is 540x960 rotated 90°) ----
constexpr int SCREEN_W = 540;
constexpr int SCREEN_H = 960;
constexpr int MARGIN = 20;
constexpr int BUTTON_H = 90;
constexpr int BUTTON_GAP = 16;

struct Rect {
    int x, y, w, h;
};
const Rect kPrintBtn = {MARGIN, SCREEN_H - MARGIN - BUTTON_H, SCREEN_W - MARGIN * 2, BUTTON_H};
const Rect kDiscardBtn = {MARGIN, SCREEN_H - MARGIN - BUTTON_H * 2 - BUTTON_GAP, SCREEN_W - MARGIN * 2, BUTTON_H};

bool inRect(const Rect &r, int x, int y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// ---- state ----
bool lastFrameReady = false;
bool lastPendingPrint = false;
uint32_t lastFrameSeq = 0xFFFFFFFF;
String lastLabel;
uint16_t lastWidth = 0, lastHeight = 0;
uint8_t *frameBuf = nullptr;
String statusMsg;

String apiUrl(const char *path) { return String("http://") + M5WEB_HOST + path; }

bool fetchStatus(bool &frameReady, bool &pendingPrint, uint16_t &width, uint16_t &height, uint32_t &frameSeq,
                  String &label) {
    HTTPClient http;
    if (!http.begin(apiUrl("/api/camera/status"))) return false;
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok) return false;

    frameReady = doc["frameReady"] | false;
    pendingPrint = doc["pendingPrint"] | false;
    width = doc["width"] | 0;
    height = doc["height"] | 0;
    frameSeq = doc["frameSeq"] | 0;
    label = String((const char *)(doc["label"] | ""));
    return true;
}

// Reads exactly `bufLen` bytes of the raw 1bpp frame into `buf`. Returns
// false (short read) on any timeout/disconnect — caller should not trust
// `buf`'s contents in that case.
bool fetchFrame(uint8_t *buf, size_t bufLen) {
    HTTPClient http;
    if (!http.begin(apiUrl("/api/camera/frame"))) return false;
    int code = http.GET();
    if (code != 200) {
        http.end();
        return false;
    }
    WiFiClient *stream = http.getStreamPtr();
    size_t got = 0;
    unsigned long start = millis();
    while (got < bufLen && (millis() - start) < 15000) {
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) break;
            delay(2);
            continue;
        }
        size_t want = bufLen - got;
        if (avail < want) want = avail;
        int n = stream->readBytes(buf + got, want);
        if (n <= 0) break;
        got += (size_t)n;
    }
    http.end();
    return got == bufLen;
}

bool postAction(const char *path) {
    HTTPClient http;
    if (!http.begin(apiUrl(path))) return false;
    int code = http.POST("");
    http.end();
    return code == 200;
}

// Draws the packed 1bpp frame (MSB-first, 1=black) into `canvas`, nearest-
// neighbor scaled to fit within (destX, destY, destW, destH) — capped at
// 2x so a small frame doesn't get blown up past legibility — and centered
// within that box.
void drawFrame(const uint8_t *buf, uint16_t srcW, uint16_t srcH, int destX, int destY, int destW, int destH) {
    float scale = min(destW / (float)srcW, destH / (float)srcH);
    if (scale > 2.0f) scale = 2.0f;
    int drawW = (int)(srcW * scale);
    int drawH = (int)(srcH * scale);
    int offX = destX + (destW - drawW) / 2;
    int offY = destY + (destH - drawH) / 2;
    uint16_t bytesPerRow = srcW / 8;

    for (int y = 0; y < drawH; y++) {
        int sy = (int)(y / scale);
        if (sy >= srcH) sy = srcH - 1;
        const uint8_t *row = buf + (size_t)sy * bytesPerRow;
        for (int x = 0; x < drawW; x++) {
            int sx = (int)(x / scale);
            if (sx >= srcW) sx = srcW - 1;
            bool black = (row[sx >> 3] >> (7 - (sx & 7))) & 1;
            canvas.drawPixel(offX + x, offY + y, black ? 15 : 0);
        }
    }
}

void drawButton(const Rect &r, const char *text) {
    canvas.fillRect(r.x, r.y, r.w, r.h, 0);
    canvas.drawRect(r.x, r.y, r.w, r.h, 15);
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    int16_t tw = canvas.textWidth(text);
    canvas.drawString(text, r.x + (r.w - tw) / 2, r.y + r.h / 2 - 12);
}

void render() {
    canvas.fillCanvas(0);

    if (!lastFrameReady) {
        canvas.setTextSize(3);
        canvas.setTextColor(15);
        canvas.drawString("まだ写真がありません", MARGIN, SCREEN_H / 2 - 20);
        canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
        return;
    }

    int imageAreaTop = MARGIN + 40;
    int imageAreaH = kDiscardBtn.y - imageAreaTop - MARGIN;
    drawFrame(frameBuf, lastWidth, lastHeight, MARGIN, imageAreaTop, SCREEN_W - MARGIN * 2, imageAreaH);

    canvas.setTextSize(2);
    canvas.setTextColor(15);
    String meta = String(lastWidth) + "x" + String(lastHeight) + "dot";
    if (lastLabel.length() > 0) meta += "  検出: " + lastLabel;
    canvas.drawString(meta, MARGIN, MARGIN);

    if (lastPendingPrint) {
        drawButton(kDiscardBtn, "破棄");
        drawButton(kPrintBtn, "印刷");
    } else {
        drawButton(kPrintBtn, "もう一度印刷");
    }

    if (statusMsg.length() > 0) {
        canvas.setTextSize(2);
        canvas.setTextColor(15);
        canvas.drawString(statusMsg, MARGIN, kPrintBtn.y - 36);
    }

    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);
}

// Fetches status, updates the lastXxx state fields, and fetches a fresh
// frame image if frameSeq changed. Never touches statusMsg or calls
// render() itself — callers decide what message to show and when to
// redraw, so an action's confirmation message (e.g. "印刷しました")
// doesn't get clobbered by a status resync done right after it.
bool syncStatus() {
    bool frameReady, pendingPrint;
    uint16_t width, height;
    uint32_t frameSeq;
    String label;
    if (!fetchStatus(frameReady, pendingPrint, width, height, frameSeq, label)) return false;

    lastFrameReady = frameReady;
    lastPendingPrint = pendingPrint;
    lastLabel = label;

    if (frameReady && frameSeq != lastFrameSeq) {
        size_t need = (size_t)(width / 8) * height;
        if (need > 0 && need <= kFrameBufMaxLen && fetchFrame(frameBuf, need)) {
            lastWidth = width;
            lastHeight = height;
            lastFrameSeq = frameSeq;
        }
    }
    return true;
}

// Periodic poll: only clears the status message and redraws if something
// actually changed since the last poll, so an unrelated tick doesn't wipe
// out a message an action just set.
void pollStatus() {
    uint32_t prevSeq = lastFrameSeq;
    bool prevReady = lastFrameReady, prevPending = lastPendingPrint;

    if (!syncStatus()) {
        statusMsg = "通信エラー";
        render();
        return;
    }
    if (lastFrameSeq != prevSeq || lastFrameReady != prevReady || lastPendingPrint != prevPending) {
        statusMsg = "";
        render();
    }
}

void handleTouch() {
    if (!M5.TP.avail()) return;
    M5.TP.update();
    tp_finger_t p = M5.TP.readFinger(0);
    int x = p.x, y = p.y;

    if (lastFrameReady && inRect(kPrintBtn, x, y)) {
        statusMsg = "印刷しています…";
        render();
        bool ok = postAction("/api/camera/print");
        syncStatus();  // pendingPrint clears once printed; pick that up before rendering
        statusMsg = ok ? "印刷しました" : "印刷に失敗しました";
        render();
    } else if (lastPendingPrint && inRect(kDiscardBtn, x, y)) {
        statusMsg = "破棄しています…";
        render();
        bool ok = postAction("/api/camera/discard");
        syncStatus();
        statusMsg = ok ? "破棄しました" : "破棄に失敗しました";
        render();
    }
    delay(300);  // debounce against repeated touch reads for the same tap
}

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    canvas.fillCanvas(0);
    canvas.setTextSize(3);
    canvas.setTextColor(15);
    canvas.drawString("Wi-Fi接続中...", MARGIN, SCREEN_H / 2 - 20);
    canvas.pushCanvas(0, 0, UPDATE_MODE_GC16);

    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
    }
}

unsigned long lastPollMs = 0;

void setup() {
    M5.begin();
    M5.EPD.SetRotation(90);
    M5.EPD.Clear(true);
    canvas.createCanvas(SCREEN_W, SCREEN_H);

    frameBuf = (uint8_t *)malloc(kFrameBufMaxLen);

    connectWifi();
    syncStatus();  // initial draw always renders, regardless of what "changed"
    render();
}

void loop() {
    handleTouch();

    unsigned long now = millis();
    if (now - lastPollMs > POLL_INTERVAL_MS) {
        lastPollMs = now;
        pollStatus();
    }
}
