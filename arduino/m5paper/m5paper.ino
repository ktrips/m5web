// m5paper.ino — M5PaperColor companion display for m5web's photo gallery.
//
// Browses m5web's gallery (every printed photo — M5StickV camera captures
// and phone uploads alike, see ../../src/gallery.*) from M5PaperColor's
// e-ink screen + physical buttons, instead of (or alongside) the phone
// browser's "ギャラリー" card on the m5web page. Talks to the same ATOM
// Lite HTTP API over WiFi — M5PaperColor has its own WiFi radio (unlike
// the M5StickV, which has none and so uses a direct UART link instead;
// see ../../src/camera_link.* for that path). No changes to the ATOM Lite
// firmware are needed: this is purely a new client of the already-existing
// /api/gallery* endpoints (see ../../README.md's API table).
//
// BtnA/BtnB step through the gallery (newest-first, same order as the web
// UI), BtnC prints whichever entry is currently shown.
//
// Target hardware: M5PaperColor (ESP32-S3, ~4" E Ink Spectra 6 color
// panel, 3 physical buttons — BtnA/BtnB/BtnC). This is a DIFFERENT board
// from the original monochrome M5Paper and uses a different library stack
// (M5Unified/M5GFX, not M5EPD) — do not confuse the two.
//
// WiFi setup: no credentials are hardcoded in this file. On first boot (or
// whenever there's no saved network, or the saved one can't be reached),
// this opens its own AP ("m5paper-setup-XXXX") with a small captive-portal
// web page — connect a phone to that AP, pick/enter the real network and
// password there, and it's saved to NVS (Preferences) for future boots.
// Mirrors src/wifi_manager.cpp's approach on the ATOM Lite side.
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
// DNSServer/WebServer/Preferences ship with the ESP32 Arduino core.
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
//
// Known limitation: there's currently no in-app way to switch to a
// *different* already-reachable network — only to recover when the saved
// one can't be joined (that falls back into AP setup mode automatically).
// To force reconfiguration otherwise, erase NVS (or add a "forget WiFi"
// trigger — see WifiManager::forgetAndRestart() in
// src/wifi_manager.cpp for the ATOM Lite's equivalent, held via a
// long button-press).

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>

// The ATOM Lite's mDNS hostname. ESP32 Arduino's HTTPClient generally
// resolves ".local" fine, but if it doesn't on your setup, replace this
// with the ATOM's IP address instead (see README.md / pio device monitor).
const char *M5WEB_HOST = "m5web.local";

constexpr unsigned long POLL_INTERVAL_MS = 3000;

// Fixed by the 58mm print head (Printer::kPrintWidthDots) — every saved
// gallery entry is this wide; /api/gallery only reports each entry's
// height, since width is always the same.
constexpr uint16_t PRINT_WIDTH = 384;

// Matches Gallery::kMaxEntries (src/gallery.h) — the gallery list never
// has more entries than this, so a fixed-size array covers every case.
constexpr int kMaxEntries = 20;

// Matches CameraLink::kMaxHeightDots (src/camera_link.cpp) * 384/8 dots —
// the tallest frame the ATOM Lite will ever save to the gallery, so a
// single fixed-size buffer covers every case without needing to grow it.
constexpr size_t kFrameBufMaxLen = 48 * 800;

M5Canvas canvas(&M5.Display);

// ---- layout (landscape — see the orientation caveat above) ----
constexpr int SCREEN_W = 600;
constexpr int SCREEN_H = 400;
constexpr int MARGIN = 16;
constexpr int META_H = 24;
constexpr int LEGEND_H = 32;

// ---- WiFi setup (AP mode + captive portal) ----
constexpr uint8_t kDnsPort = 53;
const IPAddress kApIP(192, 168, 4, 1);
Preferences wifiPrefs;
DNSServer dnsServer;
WebServer setupServer(80);
String apSsid;

// ---- app state: gallery browsing ----
struct GalleryEntry {
    uint16_t id;
    uint16_t height;
    String label;
    String savedAt;  // "MM/DD HH:MM", or "" if the ATOM's clock hadn't synced when this was saved
};
GalleryEntry entries[kMaxEntries];
int entryCount = 0;
int selectedIndex = 0;

uint16_t loadedEntryId = 0xFFFF;  // id currently held in frameBuf, or 0xFFFF (no valid id) if none
uint8_t *frameBuf = nullptr;
String statusMsg;

String apiUrl(const String &path) { return String("http://") + M5WEB_HOST + path; }

// Fetches /api/gallery into `out`/`count` (newest-first, same order the
// ATOM already sorts it in). Leaves `out`/`count` untouched on failure.
bool fetchGalleryList(GalleryEntry *out, int &count) {
    HTTPClient http;
    if (!http.begin(apiUrl("/api/gallery"))) {
        Serial.println("[api] GET /api/gallery: http.begin() failed");
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[api] GET /api/gallery failed: code=%d\n", code);
        http.end();
        return false;
    }
    String body = http.getString();
    http.end();

    DynamicJsonDocument doc(6144);  // up to kMaxEntries objects w/ label+savedAt strings — generous headroom
    if (deserializeJson(doc, body) != DeserializationError::Ok) {
        Serial.println("[api] gallery JSON parse failed");
        return false;
    }

    int n = 0;
    for (JsonObject e : doc["entries"].as<JsonArray>()) {
        if (n >= kMaxEntries) break;
        out[n].id = e["id"] | 0;
        out[n].height = e["height"] | 0;
        out[n].label = String((const char *)(e["label"] | ""));
        out[n].savedAt = String((const char *)(e["savedAt"] | ""));
        n++;
    }
    count = n;
    Serial.printf("[api] gallery: %d entries\n", count);
    return true;
}

// Reads exactly `bufLen` bytes of entry `id`'s raw 1bpp frame into `buf`.
// Returns false (short read) on any timeout/disconnect — caller should
// not trust `buf`'s contents in that case.
bool fetchGalleryFrame(uint16_t id, uint8_t *buf, size_t bufLen) {
    String path = "/api/gallery/frame?id=" + String(id);
    Serial.printf("[api] GET %s: fetching %u bytes...\n", path.c_str(), (unsigned)bufLen);
    HTTPClient http;
    if (!http.begin(apiUrl(path))) {
        Serial.printf("[api] GET %s: http.begin() failed\n", path.c_str());
        return false;
    }
    int code = http.GET();
    if (code != 200) {
        Serial.printf("[api] GET %s failed: code=%d\n", path.c_str(), code);
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
    bool ok = got == bufLen;
    Serial.printf("[api] frame fetch %s: got %u/%u bytes in %lums\n", ok ? "ok" : "SHORT", (unsigned)got,
                  (unsigned)bufLen, millis() - start);
    return ok;
}

bool printGalleryEntry(uint16_t id) {
    String path = "/api/gallery/print?id=" + String(id);
    HTTPClient http;
    if (!http.begin(apiUrl(path))) {
        Serial.printf("[api] POST %s: http.begin() failed\n", path.c_str());
        return false;
    }
    int code = http.POST("");
    http.end();
    Serial.printf("[api] POST %s -> code=%d\n", path.c_str(), code);
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

    if (entryCount == 0) {
        canvas.setTextSize(3);
        canvas.setTextColor(BLACK);
        canvas.drawString("ギャラリーに写真がありません", MARGIN, SCREEN_H / 2 - 40);
        canvas.setTextSize(2);
        canvas.drawString("M5StickVカメラで写真を撮るか、", MARGIN, SCREEN_H / 2 + 10);
        canvas.drawString("ウェブから画像をアップしてください。", MARGIN, SCREEN_H / 2 + 38);
        if (statusMsg.length() > 0) {
            canvas.drawString(statusMsg, MARGIN, SCREEN_H / 2 + 76);
        }
        canvas.pushSprite(0, 0);
        return;
    }

    GalleryEntry &sel = entries[selectedIndex];

    int imageAreaTop = MARGIN + META_H;
    int imageAreaH = SCREEN_H - LEGEND_H - imageAreaTop - MARGIN;
    if (loadedEntryId == sel.id) {
        drawFrame(frameBuf, PRINT_WIDTH, sel.height, MARGIN, imageAreaTop, SCREEN_W - MARGIN * 2, imageAreaH);
    } else {
        canvas.setTextSize(2);
        canvas.setTextColor(BLACK);
        canvas.drawString("読み込み中...", MARGIN, imageAreaTop + imageAreaH / 2 - 10);
    }

    canvas.setTextSize(2);
    canvas.setTextColor(BLACK);
    String meta = String(selectedIndex + 1) + "/" + String(entryCount) + "  #" + String(sel.id);
    if (sel.label.length() > 0) meta += "  " + sel.label;
    if (sel.savedAt.length() > 0) meta += "  " + sel.savedAt;
    canvas.drawString(meta, MARGIN, MARGIN);

    if (statusMsg.length() > 0) {
        canvas.setTextSize(2);
        canvas.drawString(statusMsg, MARGIN, SCREEN_H - LEGEND_H - META_H);
    }

    drawLegend("[A] 前へ", "[B] 次へ", "[C] 印刷");

    canvas.pushSprite(0, 0);
}

// Refetches the gallery list. Tries to keep the same entry selected across
// the refresh (matched by id, not index — the list is newest-first, so a
// new capture shifts every existing entry's index down by one), and
// clamps selectedIndex back into range if the list shrank (e.g. something
// was deleted from the phone). Leaves entries[]/entryCount/selectedIndex
// untouched on a network/parse failure.
bool syncGalleryList() {
    GalleryEntry fresh[kMaxEntries];
    int freshCount = 0;
    if (!fetchGalleryList(fresh, freshCount)) return false;

    uint16_t previouslySelectedId = (entryCount > 0) ? entries[selectedIndex].id : 0xFFFF;

    for (int i = 0; i < freshCount; i++) entries[i] = fresh[i];
    entryCount = freshCount;

    selectedIndex = 0;
    for (int i = 0; i < entryCount; i++) {
        if (entries[i].id == previouslySelectedId) {
            selectedIndex = i;
            break;
        }
    }
    return true;
}

// Fetches the selected entry's bitmap into frameBuf if it isn't already
// loaded there. Never touches statusMsg or calls render() — callers
// decide what message to show and when to redraw.
void ensureSelectedLoaded() {
    if (entryCount == 0) return;
    GalleryEntry &sel = entries[selectedIndex];
    if (loadedEntryId == sel.id) return;  // already have it

    size_t need = (size_t)(PRINT_WIDTH / 8) * sel.height;
    if (need == 0 || need > kFrameBufMaxLen) return;
    if (fetchGalleryFrame(sel.id, frameBuf, need)) loadedEntryId = sel.id;
}

// Periodic poll: only clears the status message and redraws if the list
// or the selected/loaded entry actually changed since the last poll, so
// an unrelated tick doesn't wipe out a message an action just set.
void pollGallery() {
    int prevCount = entryCount;
    uint16_t prevSelId = (entryCount > 0) ? entries[selectedIndex].id : 0xFFFF;
    uint16_t prevLoadedId = loadedEntryId;

    if (!syncGalleryList()) {
        statusMsg = "通信エラー";
        render();
        return;
    }
    ensureSelectedLoaded();

    uint16_t newSelId = (entryCount > 0) ? entries[selectedIndex].id : 0xFFFF;
    if (entryCount != prevCount || newSelId != prevSelId || loadedEntryId != prevLoadedId) {
        statusMsg = "";
        render();
    }
}

// BtnA = previous entry (toward index 0 = newest), BtnB = next entry
// (toward higher index = older), BtnC = print the selected entry.
// M5.update() must run each loop() first so .wasPressed() reflects this
// cycle's state.
void handleButtons() {
    if (entryCount == 0) return;

    if (M5.BtnA.wasPressed()) {
        selectedIndex = (selectedIndex - 1 + entryCount) % entryCount;
        Serial.printf("[btn] A pressed: prev -> %d/%d (#%u)\n", selectedIndex + 1, entryCount,
                      entries[selectedIndex].id);
        statusMsg = "";
        render();  // show the (possibly not-yet-loaded) selection immediately
        ensureSelectedLoaded();
        render();
    } else if (M5.BtnB.wasPressed()) {
        selectedIndex = (selectedIndex + 1) % entryCount;
        Serial.printf("[btn] B pressed: next -> %d/%d (#%u)\n", selectedIndex + 1, entryCount,
                      entries[selectedIndex].id);
        statusMsg = "";
        render();
        ensureSelectedLoaded();
        render();
    } else if (M5.BtnC.wasPressed()) {
        uint16_t id = entries[selectedIndex].id;
        Serial.printf("[btn] C pressed: print #%u\n", id);
        statusMsg = "印刷しています…";
        render();
        bool ok = printGalleryEntry(id);
        statusMsg = ok ? "印刷しました" : "印刷に失敗しました";
        render();
    }
}

// ---------------- WiFi: saved-credential connect + AP setup fallback ----------------

// Self-contained captive-portal page: network dropdown (populated via
// /api/wifi/scan), manual-SSID fallback, password field. Mirrors
// data/index.html's wifiCard, simplified (no shared CSS/JS to pull in —
// this only ever needs to run standalone, briefly, during setup).
const char kSetupPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>M5Paper Wi-Fi設定</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#222}
label{display:block;margin:14px 0 4px;font-size:14px;color:#555}
select,input[type=text],input[type=password]{width:100%;padding:10px;font-size:16px;box-sizing:border-box}
button{width:100%;padding:12px;margin-top:18px;font-size:16px;background:#3d8bfd;color:#fff;border:none;border-radius:6px}
#msg{margin-top:12px;font-size:14px}
.manual-line{display:flex;align-items:center;gap:6px;margin-top:10px;font-size:14px}
</style></head>
<body>
<h2>M5Paper Wi-Fi設定</h2>
<p>ATOM Lite（m5web）と同じWi-Fiネットワークを選んでください。</p>
<label>ネットワーク</label>
<select id="ssidSelect"><option value="">スキャン中…</option></select>
<div class="manual-line"><input type="checkbox" id="manual"><label for="manual" style="margin:0">SSIDを手入力する</label></div>
<input type="text" id="manualSsid" placeholder="SSID" style="display:none;margin-top:8px">
<label>パスワード</label>
<input type="password" id="pass">
<button id="btn">接続する</button>
<div id="msg"></div>
<script>
fetch('/api/wifi/scan').then(function(r){return r.json();}).then(function(list){
  var sel=document.getElementById('ssidSelect');
  sel.innerHTML='';
  if(list.length===0){sel.innerHTML='<option value="">見つかりませんでした</option>';}
  list.sort(function(a,b){return b.rssi-a.rssi;}).forEach(function(n){
    var o=document.createElement('option');
    o.value=n.ssid; o.textContent=n.ssid+(n.open?'':' [lock]');
    sel.appendChild(o);
  });
}).catch(function(){document.getElementById('ssidSelect').innerHTML='<option value="">スキャン失敗</option>';});
document.getElementById('manual').addEventListener('change',function(e){
  document.getElementById('manualSsid').style.display=e.target.checked?'block':'none';
  document.getElementById('ssidSelect').style.display=e.target.checked?'none':'block';
});
document.getElementById('btn').addEventListener('click',function(){
  var manual=document.getElementById('manual').checked;
  var ssid=manual?document.getElementById('manualSsid').value.trim():document.getElementById('ssidSelect').value;
  var pass=document.getElementById('pass').value;
  var msg=document.getElementById('msg');
  if(!ssid){msg.textContent='SSIDを入力してください';return;}
  msg.textContent='接続中…';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/api/wifi');
  xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
  xhr.onload=function(){
    if(xhr.status===200){msg.textContent='接続成功。iPhoneのWi-Fiを元に戻し、しばらくしてから画面を確認してください。';}
    else{msg.textContent='失敗: '+xhr.responseText;}
  };
  xhr.onerror=function(){msg.textContent='本機のAPが終了した可能性があります。';};
  xhr.send('ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass));
});
</script></body></html>)HTML";

void handleSetupRoot() { setupServer.send_P(200, "text/html", kSetupPage); }

// Matches src/wifi_manager.cpp's jsonEscape() — a raw SSID could in theory
// contain a quote/backslash, which would otherwise break the JSON.
String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void handleSetupScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    setupServer.send(200, "application/json", json);
}

// Tries connecting as a station, blocking up to timeoutMs. Leaves WiFi in
// whatever state WiFi.begin()/status() ends up in either way — caller
// decides what to do next (proceed, or fall back to AP setup).
bool tryStationConnect(const String &ssid, const String &password, unsigned long timeoutMs) {
    Serial.printf("[wifi] connecting to '%s'...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            Serial.printf("[wifi] connect to '%s' timed out\n", ssid.c_str());
            return false;
        }
        delay(200);
    }
    Serial.printf("[wifi] connected: ssid=%s ip=%s rssi=%ddBm\n", ssid.c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
}

// Simple centered-ish status screen for the various one-off connection
// states below (connecting/success/failure) — kept separate from
// renderSetupScreen() (which stays multi-line and is redrawn whenever we
// fall back to waiting for another attempt) and from render()'s normal
// app screen (which has its own layout).
void drawStatusScreen(const String &title, const String &line1, const String &line2 = "") {
    canvas.fillSprite(WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(BLACK);
    canvas.drawString(title, MARGIN, SCREEN_H / 2 - 60);
    canvas.setTextSize(2);
    if (line1.length() > 0) canvas.drawString(line1, MARGIN, SCREEN_H / 2);
    if (line2.length() > 0) canvas.drawString(line2, MARGIN, SCREEN_H / 2 + 30);
    canvas.pushSprite(0, 0);
}

void handleSetupConnect() {
    if (!setupServer.hasArg("ssid") || setupServer.arg("ssid").length() == 0) {
        setupServer.send(400, "text/plain", "ssid required");
        return;
    }
    String ssid = setupServer.arg("ssid");
    String password = setupServer.hasArg("password") ? setupServer.arg("password") : "";

    // The M5Paper's own screen otherwise sits frozen on the AP instructions
    // for the whole (up to 8s) connection attempt below, with no sign
    // anything is happening — show it here too, not just in the phone
    // browser's response.
    drawStatusScreen("接続中...", ssid);

    bool ok = tryStationConnect(ssid, password, 8000);
    if (ok) {
        wifiPrefs.putString("ssid", ssid);
        wifiPrefs.putString("pass", password);
        // No "success" screen drawn here — runSetupAP()'s caller
        // (ensureWifiConnected()) notices WL_CONNECTED right after this
        // returns and shows its own "接続完了" confirmation, so drawing
        // one here too would just be overwritten instantly.
    } else {
        drawStatusScreen("接続失敗", ssid, "パスワードを確認してください");
        delay(2000);
        renderSetupScreen();  // back to waiting for another attempt
    }
    // If this failed, WiFi.begin() may have left the radio disconnected —
    // runSetupAP()'s caller loop just keeps serving the setup page either
    // way (status still != WL_CONNECTED), so the AP itself is unaffected.
    setupServer.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Failed to join that network");
}

void renderSetupScreen() {
    canvas.fillSprite(WHITE);
    canvas.setTextSize(3);
    canvas.setTextColor(BLACK);
    canvas.drawString("Wi-Fi設定モード", MARGIN, MARGIN);
    canvas.setTextSize(2);
    canvas.drawString("1. Wi-Fiで次のネットワークに接続:", MARGIN, MARGIN + 60);
    canvas.setTextSize(3);
    canvas.drawString(apSsid, MARGIN, MARGIN + 90);
    canvas.setTextSize(2);
    canvas.drawString("(パスワードなし・オープンネットワーク)", MARGIN, MARGIN + 120);
    canvas.drawString("2. ブラウザで開く:", MARGIN, MARGIN + 155);
    canvas.setTextSize(3);
    canvas.drawString("http://" + kApIP.toString(), MARGIN, MARGIN + 185);
    canvas.setTextSize(2);
    canvas.drawString("接続先のWi-Fiを選んでパスワードを入力してください。", MARGIN, MARGIN + 245);
    canvas.pushSprite(0, 0);
}

// Starts the setup AP + captive-portal web server, and blocks (servicing
// DNS/HTTP requests) until a phone successfully submits working
// credentials via handleSetupConnect(). Returns with WiFi already
// connected as a station.
void runSetupAP() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char nameBuf[24];
    snprintf(nameBuf, sizeof(nameBuf), "m5paper-setup-%02X%02X", mac[4], mac[5]);
    apSsid = nameBuf;

    // A clean WIFI_OFF -> delay -> mode transition, and disabling power-save,
    // are both common fixes for the AP silently not broadcasting on ESP32 —
    // symptoms include this exact case (softAP() "succeeds" but the SSID
    // never actually shows up on a scan).
    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP_STA);  // AP for the phone to join + STA so we can try joining the real network
    WiFi.setSleep(false);

    bool apOk = WiFi.softAP(apSsid.c_str());
    dnsServer.start(kDnsPort, "*", kApIP);
    Serial.printf("[wifi] AP setup mode: softAP()=%s ssid=%s ip=%s mac=%s\n", apOk ? "ok" : "FAILED",
                  apSsid.c_str(), WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
    if (!apOk) {
        Serial.println("[wifi] softAP() failed — the setup network will not be visible; check antenna/power");
    }

    setupServer.on("/", HTTP_GET, handleSetupRoot);
    setupServer.onNotFound(handleSetupRoot);  // captive-portal probes land on the setup page too
    setupServer.on("/api/wifi/scan", HTTP_GET, handleSetupScan);
    setupServer.on("/api/wifi", HTTP_POST, handleSetupConnect);
    setupServer.begin();

    renderSetupScreen();

    while (WiFi.status() != WL_CONNECTED) {
        dnsServer.processNextRequest();
        setupServer.handleClient();
        delay(2);
    }

    setupServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

// Tries the saved network first (if any); falls back to runSetupAP() —
// which blocks until it has good credentials — so this always returns
// with WiFi connected. Either way, shows the final connection result
// (SSID/IP/RSSI) on screen briefly before the caller moves on to the
// normal app screen, so a fresh boot always visibly confirms what it
// connected to and how.
void ensureWifiConnected() {
    wifiPrefs.begin("m5paper", false);
    String savedSsid = wifiPrefs.getString("ssid", "");
    String savedPass = wifiPrefs.getString("pass", "");

    WiFi.mode(WIFI_STA);
    bool connected = false;
    if (savedSsid.length() > 0) {
        drawStatusScreen("Wi-Fi接続中...", savedSsid);
        connected = tryStationConnect(savedSsid, savedPass, 15000);
        if (!connected) {
            Serial.println("[wifi] saved network unreachable, entering setup mode");
            drawStatusScreen("接続できません", savedSsid, "設定モードに入ります");
            delay(2000);
        }
    } else {
        Serial.println("[wifi] no saved network, entering setup mode");
    }

    if (!connected) runSetupAP();

    drawStatusScreen("接続完了", "SSID: " + WiFi.SSID(),
                      "IP: " + WiFi.localIP().toString() + "  " + String(WiFi.RSSI()) + "dBm");
    delay(1500);
}

unsigned long lastPollMs = 0;

// Periodic snapshot of connection/data state, so opening the serial
// monitor mid-session immediately shows current status rather than only
// past event logs — same idea as main.cpp's printHeartbeat() on the ATOM.
constexpr unsigned long kHeartbeatIntervalMs = 10000;
unsigned long lastHeartbeatMs = 0;

void printHeartbeat() {
    unsigned long now = millis();
    if (now - lastHeartbeatMs < kHeartbeatIntervalMs) return;
    lastHeartbeatMs = now;

    Serial.println("---- m5paper status ----");
    Serial.printf("uptime: %lus  free heap: %u bytes\n", now / 1000, ESP.getFreeHeap());
    bool connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("wifi: %s ssid=%s ip=%s rssi=%ddBm\n", connected ? "connected" : "DISCONNECTED",
                  WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    if (entryCount > 0) {
        GalleryEntry &sel = entries[selectedIndex];
        Serial.printf("gallery: %d entries, selected=%d/%d id=#%u %ux%u label=\"%s\" savedAt=\"%s\" loaded=%d\n",
                      entryCount, selectedIndex + 1, entryCount, sel.id, PRINT_WIDTH, sel.height, sel.label.c_str(),
                      sel.savedAt.c_str(), loadedEntryId == sel.id);
    } else {
        Serial.println("gallery: 0 entries");
    }
    Serial.println("-------------------------");
}

void setup() {
    Serial.begin(115200);  // USB diagnostic log; open the serial monitor at 115200bps
    Serial.println("\n=== m5paper starting ===");

    auto cfg = M5.config();
    M5.begin(cfg);
    M5.Display.setEpdMode(epd_mode_t::epd_quality);

    canvas.createSprite(SCREEN_W, SCREEN_H);

    frameBuf = (uint8_t *)malloc(kFrameBufMaxLen);

    ensureWifiConnected();
    Serial.printf("[wifi] ready: ip=%s host=%s\n", WiFi.localIP().toString().c_str(), M5WEB_HOST);

    syncGalleryList();
    ensureSelectedLoaded();
    render();

    Serial.println("=== m5paper ready ===");
}

void loop() {
    M5.update();
    handleButtons();
    printHeartbeat();

    unsigned long now = millis();
    if (now - lastPollMs > POLL_INTERVAL_MS) {
        lastPollMs = now;
        pollGallery();
    }
}
