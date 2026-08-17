// m5paper.ino — M5PaperColor companion display for m5web's M5StickV camera
// preview/print workflow.
//
// Lets you review the M5StickV's pending camera frame and print/discard it
// from M5PaperColor's e-ink screen + physical buttons, instead of (or
// alongside) the phone browser's "M5StickVカメラ" card on the m5web page.
// Talks to the same ATOM Lite HTTP API over WiFi — M5PaperColor has its own
// WiFi radio (unlike the M5StickV, which has none and so uses a direct
// UART link instead; see ../../src/camera_link.* for that path). No
// changes to the ATOM Lite firmware are needed: this is purely a new
// client of the already-existing /api/camera/* endpoints (see
// ../../README.md's API table).
//
// Target hardware: M5PaperColor (ESP32-S3, ~4" E Ink Spectra 6 color
// panel, 3 physical buttons — BtnA/BtnB/BtnC). This is a DIFFERENT board
// from the original monochrome M5Paper and uses a different library stack
// (M5Unified/M5GFX, not M5EPD) — do not confuse the two.
//
// IMPORTANT: this REPLACES whatever firmware is currently on the
// M5PaperColor when flashed — including the factory-stock "AP mode +
// image upload" app, since a device can only run one firmware image at a
// time. This sketch does not attempt to reimplement that stock app. If
// you still want that capability, reflash the stock firmware when you
// need it (or use a second unit dedicated to this sketch).
//
// Requires three libraries via Arduino Library Manager: "M5Unified"
// (M5Stack), "M5GFX" (M5Stack), and "ArduinoJson" (Benoit Blanchon).
//
// NOTE: written against M5Stack's own documented M5PaperColor setup
// (docs.m5stack.com/en/arduino/papercolor/program and .../button) but not
// tested on real hardware. Likely spots to double-check/adjust:
//   - Screen resolution: assumed 600x400 landscape below; various sources
//     list it as "400x600"/"600x400" without being explicit about
//     orientation, so double check SCREEN_W/SCREEN_H match what you
//     actually see, and swap them if the image renders letterboxed wrong.
//   - This is a 6-color (Spectra) panel; only WHITE/BLACK are used here
//     since the source image is already a 1bpp monochrome print bitmap,
//     so no color-palette handling was needed.
//   - epd_mode_t::epd_quality is set once in setup() for full-quality
//     refresh; this app redraws rarely (on poll changes / button
//     presses), not continuously, so the extra refresh time isn't a
//     concern here the way it would be for an animated UI.

#include <M5Unified.h>
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

M5Canvas canvas(&M5.Display);

// ---- layout (landscape — see the orientation caveat above) ----
constexpr int SCREEN_W = 600;
constexpr int SCREEN_H = 400;
constexpr int MARGIN = 16;
constexpr int META_H = 24;
constexpr int LEGEND_H = 32;

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
            canvas.drawPixel(offX + x, offY + y, black ? BLACK : WHITE);
        }
    }
}

// Bottom legend showing what each physical button currently does — plain
// text, not a touch target (M5PaperColor has no touchscreen). Roughly
// left/middle/right under where BtnA/BtnB/BtnC sit, as a visual hint.
void drawLegend(const char *left, const char *middle, const char *right) {
    canvas.setTextSize(2);
    canvas.setTextColor(BLACK);
    int y = SCREEN_H - LEGEND_H + 6;

    if (left) canvas.drawString(left, MARGIN, y);
    if (middle) {
        int16_t tw = canvas.textWidth(middle);
        canvas.drawString(middle, SCREEN_W / 2 - tw / 2, y);
    }
    if (right) {
        int16_t tw = canvas.textWidth(right);
        canvas.drawString(right, SCREEN_W - MARGIN - tw, y);
    }
}

void render() {
    canvas.fillSprite(WHITE);

    if (!lastFrameReady) {
        canvas.setTextSize(3);
        canvas.setTextColor(BLACK);
        canvas.drawString("まだ写真がありません", MARGIN, SCREEN_H / 2 - 20);
        drawLegend(nullptr, "[B] 更新", nullptr);
        canvas.pushSprite(0, 0);
        return;
    }

    int imageAreaTop = MARGIN + META_H;
    int imageAreaH = SCREEN_H - LEGEND_H - imageAreaTop - MARGIN;
    drawFrame(frameBuf, lastWidth, lastHeight, MARGIN, imageAreaTop, SCREEN_W - MARGIN * 2, imageAreaH);

    canvas.setTextSize(2);
    canvas.setTextColor(BLACK);
    String meta = String(lastWidth) + "x" + String(lastHeight) + "dot";
    if (lastLabel.length() > 0) meta += "  検出: " + lastLabel;
    if (statusMsg.length() > 0) meta += "  " + statusMsg;
    canvas.drawString(meta, MARGIN, MARGIN);

    if (lastPendingPrint) {
        drawLegend("[A] 破棄", "[B] 更新", "[C] 印刷");
    } else {
        drawLegend(nullptr, "[B] 更新", "[C] もう一度印刷");
    }

    canvas.pushSprite(0, 0);
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

// BtnA = discard (only meaningful while pendingPrint), BtnB = force an
// immediate poll (instead of waiting up to POLL_INTERVAL_MS), BtnC =
// print / reprint. M5.update() must run each loop() first so
// .wasPressed() reflects this cycle's state.
void handleButtons() {
    if (M5.BtnC.wasPressed() && lastFrameReady) {
        statusMsg = "印刷しています…";
        render();
        bool ok = postAction("/api/camera/print");
        syncStatus();  // pendingPrint clears once printed; pick that up before rendering
        statusMsg = ok ? "印刷しました" : "印刷に失敗しました";
        render();
    } else if (M5.BtnA.wasPressed() && lastPendingPrint) {
        statusMsg = "破棄しています…";
        render();
        bool ok = postAction("/api/camera/discard");
        syncStatus();
        statusMsg = ok ? "破棄しました" : "破棄に失敗しました";
        render();
    } else if (M5.BtnB.wasPressed()) {
        statusMsg = "更新しています…";
        render();
        statusMsg = syncStatus() ? "" : "通信エラー";
        render();
    }
}

void connectWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    canvas.fillSprite(WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(BLACK);
    canvas.drawString("Wi-Fi接続中...", MARGIN, SCREEN_H / 2 - 20);
    canvas.pushSprite(0, 0);

    while (WiFi.status() != WL_CONNECTED) {
        delay(300);
    }
}

unsigned long lastPollMs = 0;

void setup() {
    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);

    canvas.createSprite(SCREEN_W, SCREEN_H);

    frameBuf = (uint8_t *)malloc(kFrameBufMaxLen);

    connectWifi();
    syncStatus();  // initial draw always renders, regardless of what "changed"
    render();
}

void loop() {
    M5.update();
    handleButtons();

    unsigned long now = millis();
    if (now - lastPollMs > POLL_INTERVAL_MS) {
        lastPollMs = now;
        pollStatus();
    }
}
