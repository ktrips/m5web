#include "web_server.h"

#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>

#include "camS3_actions.h"
#include "caption.h"
#include "clock.h"
#include "data_html.h"
#include "default_adjust.h"
#include "esp_camera.h"
#include "gallery.h"
#include "haiku.h"
#include "jpeg_print.h"
#include "openai.h"
#include "own_camera.h"
#include "printer.h"
#include "printer_mode.h"
#include "storage.h"

// Route summary — always registered, regardless of PrinterMode::mode()
// (own_camera.h's 撮影方式 setting is likewise independent of PrinterMode —
// see its doc comment: preview-mode capture/confirm works the same way
// whichever connection mode is active, so these routes can't be gated on
// kDirect the way the printer/gallery/storage ones below are):
//   GET  /                      camS3 web UI (direct: kIndexHtml, see
//                                data_html.h; via_atom: kLightPage, a
//                                smaller embedded page below — neither
//                                needs a filesystem to serve)
//   GET  /api/status             mode, wifi (incl. ssid), camera, print-width
//   GET  /api/mode                {"mode":"direct"|"via_atom","atomHost":"..."}
//   POST /api/mode                switch mode (+ atomHost) — reboots
//   POST /api/camera/capture     take a photo — kAuto commits immediately
//                                 (prints locally or forwards to atomHost()
//                                 depending on PrinterMode), kPreview holds
//                                 it for confirmation instead
//   GET  /shutter                 legacy alias for the above (see camS3.ino's
//                                 original file header) — kept for existing
//                                 curl/automation callers
//   GET/POST /api/camera/mode     撮影方式, auto|preview — same shape as
//                                  m5web's own /api/camera/mode for M5StickV
//   GET  /api/camera/status, /api/camera/frame   pending preview frame
//   POST /api/camera/print, /api/camera/discard  confirm/discard it
//   GET  /api/camera/live         one fresh JPEG straight off the sensor,
//                                  for the web UI's リアルタイムプレビュー
//                                  toggle to poll (see handleCameraLive())
//   GET/POST /api/default_adjust  default brightness/contrast (feeds every
//                                  JpegPrint decode, own captures included)
//   GET/POST /api/storage/settings  flash vs. SD card (own_camera.cpp's
//                                    preview capture always writes its temp
//                                    JPEG through Storage::fs())
//
// Registered only when PrinterMode::mode() == kDirect (CamS3 has no
// printer/gallery of its own otherwise, so these would have nothing to
// act on):
//   GET/POST /api/printer/settings    UART TX/RX pins
//   POST /api/print/text, /api/print/qr, /api/print/test
//   POST /api/print/image/begin, /api/print/image (phone-upload print, same
//        client-side-dithered-upload protocol as m5web)
//   POST /api/print/photo, /api/print/photo/url (arbitrary JPEG, same as
//        m5web's — also what a *different* CamS3 in kViaAtom mode, or any
//        other HTTP client, would call if pointed at this device instead)
//   GET  /api/gallery, /api/gallery/frame, /api/gallery/print,
//        /api/gallery/delete
//   GET/POST /api/haiku/settings, /api/openai/settings
//
// Deliberately NOT ported from m5web for this first CamS3 version (kept
// out to bound scope — see README.md): QR watermark / external-forward-URL
// settings, M5Paper polling settings, GET /capture (JPEG re-encode of the
// pending preview frame for a generic-camera-style snapshot URL — could
// reuse own_camera.h's preview frame the same way CameraLink's does on
// m5web, just not wired up yet; /api/camera/live above serves a similar
// need for live viewing instead).
namespace WebServer_ {

namespace {

constexpr uint16_t kMaxHeightDots = 2000;  // ~250mm; keeps jobs to a sane length
constexpr size_t kMaxQrLength = 300;
constexpr uint8_t kMaxGpioNum = 48;

WebServer server(80);

const char kLightPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>m5cam</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#222}
h1{font-size:22px;margin:4px 0 0;text-align:center;display:flex;align-items:center;justify-content:center;gap:8px}
.sub{text-align:center;color:#888;margin:0 0 16px;font-size:13px}
h3{margin:0 0 10px;display:flex;align-items:center;gap:6px}
.card{border:1px solid #ddd;border-radius:8px;padding:14px;margin:14px 0;background:#fafafa}
button{width:100%;padding:12px;margin-top:10px;font-size:16px;background:#3d8bfd;color:#fff;border:none;border-radius:6px;font-weight:600}
button.secondary{background:#e5e7eb;color:#222}
button:disabled{opacity:.5}
input[type=text],input[type=password]{width:100%;padding:10px;font-size:16px;box-sizing:border-box;margin-top:6px;border:1px solid #ccc;border-radius:6px}
label{display:block;margin-top:10px;font-size:14px;color:#555}
.msg{margin-top:10px;font-size:14px}
.meta{font-size:13px;color:#777}
.row{display:flex;align-items:center;justify-content:space-between;gap:8px;flex-wrap:wrap}
.badge{display:inline-flex;align-items:center;gap:6px;font-size:13px;padding:6px 10px;border-radius:20px;background:#eee;color:#555}
.dot{width:8px;height:8px;border-radius:50%;background:#999}
.dot.ok{background:#35c46a}
.check-line{display:flex;align-items:center;gap:8px;font-size:14px;margin-top:10px}
.check-line input{margin:0}
#liveImg{width:100%;border-radius:8px;margin-top:10px;background:#000;display:none}
#previewWrap{margin-top:10px;background:#fff;border-radius:8px;overflow:hidden;display:none;text-align:center}
#previewCanvas{max-width:100%;image-rendering:pixelated;display:block;margin:0 auto}
.btn-row{display:flex;gap:8px}
.btn-row>*{flex:1}
</style></head>
<body>
<h1>📷 m5cam</h1>
<p class="sub">CamS3ワイヤレスシャッター</p>

<div class="card">
  <div class="row">
    <span class="badge"><span class="dot" id="statusDot"></span><span id="statusText">確認中…</span></span>
    <span class="badge" id="ipBadge"></span>
  </div>
  <div class="row" style="margin-top:8px">
    <span class="badge" id="clockBadge">--:--:--</span>
    <span class="badge" id="locationBadge">位置情報を取得中…</span>
  </div>
  <p class="meta">現在の接続方式: <b id="modeLabel">確認中…</b></p>
</div>

<div class="card">
  <h3>📸 撮影</h3>
  <div class="check-line">
    <input type="checkbox" id="liveToggle">
    <label for="liveToggle" style="margin:0">リアルタイムプレビューを表示</label>
  </div>
  <img id="liveImg">
  <button id="shutterBtn">撮影する</button>
  <div class="msg" id="shutterMsg"></div>

  <div id="previewWrap"><canvas id="previewCanvas"></canvas></div>
  <p class="meta" id="previewMeta"></p>
  <div class="check-line">
    <input type="checkbox" id="autoPrintToggle">
    <label for="autoPrintToggle" style="margin:0">自動印刷（撮影後すぐにATOM Printerに送信、印刷）</label>
  </div>
  <div class="btn-row" id="previewActions" style="display:none">
    <button id="printBtn">🖨️ 印刷</button>
    <button class="secondary" id="discardBtn">🗑️ 破棄</button>
  </div>
  <div class="msg" id="previewMsg"></div>
</div>

<div class="card">
  <h3>🔀 接続方式</h3>
  <p class="meta">「ATOMモード」では、撮影した写真は設定したATOM(m5web.local等)へ送られ、
  印刷・保存・設定は全てATOM側で行われます。</p>
  <label>送信先（ATOM経由モードのみ使用）</label>
  <input type="text" id="atomHost" placeholder="m5web.local">
  <button id="viaAtomBtn">ATOM経由モードにする</button>
  <p class="meta">CamS3で全て撮影、保存、印刷、設定を行う場合は直接モードに切り替えて下さい
  （再起動します）。</p>
  <button class="secondary" id="directBtn">直接接続モードにする</button>
  <div class="msg" id="modeMsg"></div>
</div>

<div class="card">
  <h3>📶 Wi-Fi</h3>
  <label>SSID</label>
  <input type="text" id="wifiSsid" placeholder="ネットワーク名">
  <label>パスワード</label>
  <input type="password" id="wifiPass" placeholder="Wi-Fiパスワード">
  <button id="wifiJoinBtn">このネットワークに接続する</button>
  <p class="meta">うまくいかない場合は下のボタンで保存済みのWi-Fi情報を消去して再起動します。
  再起動後は camS3-setup-XXXX という名前のAPが立ち上がるので、スマホから接続して設定し直してください。</p>
  <button class="secondary" id="forgetWifiBtn">Wi-Fi設定を消去して再起動</button>
  <div class="msg" id="wifiMsg"></div>
</div>

<script>
function $(id){return document.getElementById(id);}
function showMsg(el,text){el.textContent=text;}

// ---------- status / clock / location ----------
async function refreshStatus(){
  try{
    const r=await fetch('/api/status');
    const s=await r.json();
    const dot=$('statusDot'), text=$('statusText'), ip=$('ipBadge');
    if(s.connected){ dot.className='dot ok'; text.textContent='Wi-Fi接続中: '+s.ssid; ip.textContent=s.ip; }
    else { dot.className='dot'; text.textContent='接続中…'; ip.textContent=''; }
  }catch(e){ $('statusText').textContent='通信エラー'; }
}
function updateClock(){
  $('clockBadge').textContent=new Date().toLocaleTimeString('ja-JP',{hour12:false});
}
updateClock(); setInterval(updateClock,1000);

// IP geolocation (ipapi.co, free/no key, city/prefecture-level accuracy)
// — used directly, not just as a fallback: navigator.geolocation needs a
// secure context (HTTPS/localhost), and m5cam.local is plain HTTP, so
// the browser's own Geolocation API is blocked outright before any
// permission prompt on most modern browsers. This works over plain HTTP
// (the fetch target is https://, which a plain http:// page is allowed
// to call — only the reverse is blocked as mixed content) and needs no
// permission.
async function initLocationBadge(){
  const badge=$('locationBadge');
  try{
    const r=await fetch('https://ipapi.co/json/');
    const data=await r.json();
    if(data.error) throw new Error(data.reason||'lookup failed');
    const text=[data.region,data.city].filter(Boolean).join(' ')||data.country_name||'';
    badge.textContent=text?text+'（IPからの概算）':'場所不明';
  }catch(e){ badge.textContent='位置情報の取得に失敗'; }
}
initLocationBadge();

// ---------- shutter sound ----------
// CamS3 itself has no speaker — see data_html.h's copy of this function
// for the full rationale (synthesized click, no embedded audio file;
// only fires for captures triggered from this page's own shutterBtn).
let shutterAudioCtx=null;
function playShutterSound(){
  try{
    if(!shutterAudioCtx) shutterAudioCtx=new (window.AudioContext||window.webkitAudioContext)();
    const ctx=shutterAudioCtx;
    const duration=0.09;
    const bufferSize=Math.floor(ctx.sampleRate*duration);
    const buffer=ctx.createBuffer(1,bufferSize,ctx.sampleRate);
    const data=buffer.getChannelData(0);
    for(let i=0;i<bufferSize;i++){
      const t=i/bufferSize;
      const envelope=Math.pow(1-t,4);
      data[i]=(Math.random()*2-1)*envelope;
    }
    const source=ctx.createBufferSource();
    source.buffer=buffer;
    const filter=ctx.createBiquadFilter();
    filter.type='highpass'; filter.frequency.value=2500;
    source.connect(filter); filter.connect(ctx.destination);
    source.start();
  }catch(e){}
}

// ---------- live view ----------
// Paused for the duration of an actual capture (see shutterBtn's handler
// below) — this board's WebServer handles one request at a time, so a
// live-view poll landing mid-capture would otherwise just queue up and
// compete with the capture for the same single-threaded camera access.
let liveTimer=null;
function startLivePolling(){
  liveTimer=setInterval(()=>{ $('liveImg').src='/api/camera/live?t='+Date.now(); },600);
  $('liveImg').src='/api/camera/live?t='+Date.now();
}
function pauseLivePolling(){
  if(liveTimer){ clearInterval(liveTimer); liveTimer=null; }
}
$('liveToggle').addEventListener('change',(e)=>{
  if(e.target.checked){
    $('liveImg').style.display='block';
    startLivePolling();
  }else{
    pauseLivePolling();
    $('liveImg').style.display='none'; $('liveImg').removeAttribute('src');
  }
});

// ---------- shutter ----------
$('shutterBtn').addEventListener('click', async ()=>{
  const liveWasOn = $('liveToggle').checked && liveTimer;
  playShutterSound();
  pauseLivePolling();
  $('shutterBtn').disabled=true;
  showMsg($('shutterMsg'),'撮影中…');
  try{
    const r=await fetch('/api/camera/capture',{method:'POST'});
    const text=await r.text();
    showMsg($('shutterMsg'), r.ok ? '完了: '+text : '失敗: '+text);
    if(r.ok) refreshPreview();
  }catch(e){
    showMsg($('shutterMsg'),'通信エラー: '+e.message);
  }finally{
    $('shutterBtn').disabled=false;
    if(liveWasOn) startLivePolling();
  }
});

// ---------- preview (プレビュー確認方式) ----------
function unpackAndDraw(canvas,buf,w,h){
  const bytesPerRow=w/8;
  canvas.width=w; canvas.height=h;
  const ctx=canvas.getContext('2d');
  const img=ctx.createImageData(w,h);
  for(let y=0;y<h;y++){
    for(let x=0;x<w;x++){
      const byte=buf[y*bytesPerRow+(x>>3)];
      const black=(byte>>(7-(x&7)))&1;
      const v=black?0:255;
      const i=(y*w+x)*4;
      img.data[i]=v; img.data[i+1]=v; img.data[i+2]=v; img.data[i+3]=255;
    }
  }
  ctx.putImageData(img,0,0);
  canvas.style.width=Math.min(360,w*2)+'px';
}
let lastSeq=-1;
async function refreshPreview(){
  try{
    const r=await fetch('/api/camera/status');
    const s=await r.json();
    $('autoPrintToggle').checked = s.mode==='auto';
    if(!s.frameReady){
      $('previewActions').style.display='none';
      $('previewWrap').style.display='none';
      $('previewMeta').textContent='';
      return;
    }
    $('previewActions').style.display = s.pendingPrint ? 'flex' : 'none';
    $('previewMeta').textContent = s.pendingPrint
      ? `確認待ち: ${s.width}×${s.height}dot`
      : `最後に印刷: ${s.width}×${s.height}dot`;
    if(s.frameSeq!==lastSeq){
      lastSeq=s.frameSeq;
      const fr=await fetch('/api/camera/frame');
      if(fr.ok){
        const buf=new Uint8Array(await fr.arrayBuffer());
        unpackAndDraw($('previewCanvas'),buf,s.width,s.height);
        $('previewWrap').style.display='block';
      }
    }
  }catch(e){}
}
$('autoPrintToggle').addEventListener('change',async (e)=>{
  try{
    await fetch('/api/camera/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'mode='+(e.target.checked?'auto':'preview')});
    refreshPreview();
  }catch(err){}
});
$('printBtn').addEventListener('click', async ()=>{
  const msg=$('previewMsg');
  $('printBtn').disabled=true;
  try{
    const r=await fetch('/api/camera/print',{method:'POST'});
    if(!r.ok) throw new Error(await r.text());
    showMsg(msg,'印刷しました');
    refreshPreview();
  }catch(e){ showMsg(msg,'失敗しました: '+e.message); }
  finally{ $('printBtn').disabled=false; }
});
$('discardBtn').addEventListener('click', async ()=>{
  try{
    await fetch('/api/camera/discard',{method:'POST'});
    showMsg($('previewMsg'),'破棄しました');
    refreshPreview();
  }catch(e){ showMsg($('previewMsg'),'失敗しました: '+e.message); }
});

// ---------- connection mode ----------
async function refreshMode(){
  try{
    const r=await fetch('/api/mode');
    const s=await r.json();
    $('modeLabel').textContent = s.mode==='direct' ? '直接接続' : 'ATOM経由';
    if(!$('atomHost').dataset.touched) $('atomHost').value = s.atomHost;
  }catch(e){}
}
$('atomHost').addEventListener('input',()=>{$('atomHost').dataset.touched='1';});
$('viaAtomBtn').addEventListener('click', async ()=>{
  showMsg($('modeMsg'),'切り替え中…（再起動します）');
  await fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'mode=via_atom&atomHost='+encodeURIComponent($('atomHost').value||'m5web.local')});
});
$('directBtn').addEventListener('click', async ()=>{
  showMsg($('modeMsg'),'切り替え中…（再起動します）');
  await fetch('/api/mode',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
    body:'mode=direct'});
});

// ---------- Wi-Fi ----------
$('wifiJoinBtn').addEventListener('click', async ()=>{
  const msg=$('wifiMsg');
  const ssid=$('wifiSsid').value.trim();
  if(!ssid){showMsg(msg,'SSIDを入力してください');return;}
  showMsg(msg,'接続中…');
  try{
    const r=await fetch('/api/wifi',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},
      body:'ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent($('wifiPass').value)});
    showMsg(msg, r.ok ? '接続しました' : '失敗: '+await r.text());
  }catch(e){
    showMsg(msg,'通信エラー: '+e.message);
  }
});
$('forgetWifiBtn').addEventListener('click', async ()=>{
  if(!confirm('Wi-Fi設定を消去して再起動します。よろしいですか？')) return;
  showMsg($('wifiMsg'),'消去して再起動します…');
  await fetch('/api/wifi/forget',{method:'POST'});
});

refreshStatus(); setInterval(refreshStatus,5000);
refreshMode(); setInterval(refreshMode,5000);
refreshPreview(); setInterval(refreshPreview,3000);
</script>
</body></html>)HTML";

void sendPlain(int code, const String &body) { server.send(code, "text/plain", body); }

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

// ---- always-on routes ----

void handleRoot() {
    if (PrinterMode::mode() == PrinterMode::Mode::kViaAtom) {
        server.send_P(200, "text/html", kLightPage);
        return;
    }
    // Direct-connect mode's full page is embedded (kIndexHtml, see
    // data_html.h) rather than read from Storage::fs() — no separate
    // filesystem upload step needed (see storage.h's doc comment for why).
    server.send_P(200, "text/html", kIndexHtml);
}

void handleStatus() {
    bool direct = PrinterMode::mode() == PrinterMode::Mode::kDirect;
    String json = "{";
    json += "\"mode\":\"" + String(direct ? "direct" : "via_atom") + "\",";
    json += "\"atomHost\":\"" + jsonEscape(PrinterMode::atomHost().c_str()) + "\",";
    json += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
    json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
    json += "\"ssid\":\"" + jsonEscape(WiFi.SSID().c_str()) + "\",";
    json += "\"printWidthDots\":" + String(Printer::kPrintWidthDots) + ",";
    json += "\"maxHeightDots\":" + String(kMaxHeightDots);
    json += "}";
    server.send(200, "application/json", json);
}

void handleModeGet() {
    bool direct = PrinterMode::mode() == PrinterMode::Mode::kDirect;
    String json = "{\"mode\":\"" + String(direct ? "direct" : "via_atom") + "\",\"atomHost\":\"" +
                  jsonEscape(PrinterMode::atomHost().c_str()) + "\"}";
    server.send(200, "application/json", json);
}

void handleModeSet() {
    if (!server.hasArg("mode")) {
        sendPlain(400, "mode required (direct|via_atom)");
        return;
    }
    String m = server.arg("mode");
    if (m != "direct" && m != "via_atom") {
        sendPlain(400, "mode must be 'direct' or 'via_atom'");
        return;
    }
    if (server.hasArg("atomHost")) PrinterMode::setAtomHost(server.arg("atomHost"));
    sendPlain(200, "OK — restarting");
    server.client().flush();
    PrinterMode::setMode(m == "direct" ? PrinterMode::Mode::kDirect : PrinterMode::Mode::kViaAtom);  // reboots, doesn't return
}

void handleCaptureRequest() {
    Serial.println("[web] /api/camera/capture (or /shutter) request received");
    setLed(true);  // on for the whole capture+print/forward — see camS3.ino's file header LED note
    String resultMsg;
    bool ok;
    if (OwnCamera::mode() == OwnCamera::Mode::kPreview) {
        Serial.println("[web] dispatching: captureForPreview");
        ok = OwnCamera::captureForPreview(resultMsg);
        if (ok) resultMsg = "captured — open the 撮影 card to review and print";
    } else {
        Serial.println("[web] dispatching: captureAndCommitNow");
        ok = OwnCamera::captureAndCommitNow(resultMsg);
        if (ok) resultMsg = "printed";
    }
    Serial.printf("[web] capture dispatch returned ok=%d msg=%s\n", ok, resultMsg.c_str());
    setLed(false);
    blinkLed(ok ? 1 : 3, ok ? 400 : 120, 120);  // same pattern as camS3.ino's original handleTrigger()
    server.send(ok ? 200 : 502, "text/plain", resultMsg);
}

void handleOwnCameraStatus() {
    OwnCamera::Status s = OwnCamera::status();
    String json = "{";
    json += "\"mode\":\"" + String(s.mode == OwnCamera::Mode::kPreview ? "preview" : "auto") + "\",";
    json += "\"frameReady\":" + String(s.frameReady ? "true" : "false") + ",";
    json += "\"pendingPrint\":" + String(s.pendingPrint ? "true" : "false") + ",";
    json += "\"width\":" + String(s.width) + ",";
    json += "\"height\":" + String(s.height) + ",";
    json += "\"frameSeq\":" + String(s.frameSeq);
    json += "}";
    server.send(200, "application/json", json);
}

void handleOwnCameraModeGet() {
    server.send(200, "application/json",
                String("{\"mode\":\"") + (OwnCamera::mode() == OwnCamera::Mode::kPreview ? "preview" : "auto") + "\"}");
}

void handleOwnCameraModeSet() {
    if (!server.hasArg("mode")) {
        sendPlain(400, "mode required");
        return;
    }
    String m = server.arg("mode");
    if (m != "auto" && m != "preview") {
        sendPlain(400, "mode must be 'auto' or 'preview'");
        return;
    }
    OwnCamera::setMode(m == "preview" ? OwnCamera::Mode::kPreview : OwnCamera::Mode::kAuto);
    sendPlain(200, "OK");
}

void handleOwnCameraFrame() {
    const uint8_t *data = OwnCamera::frameData();
    size_t len = OwnCamera::frameDataLen();
    if (!data || len == 0) {
        sendPlain(404, "no frame yet");
        return;
    }
    OwnCamera::Status s = OwnCamera::status();
    server.sendHeader("X-Frame-Width", String(s.width));
    server.sendHeader("X-Frame-Height", String(s.height));
    server.setContentLength(len);
    server.send(200, "application/octet-stream", "");
    server.client().write(data, len);
}

// Live viewfinder — a single fresh JPEG straight off the camera (no
// decode/dither/temp-file), for the web UI's リアルタイムプレビュー toggle
// to poll on an interval (see data_html.h/kLightPage's JS). Deliberately
// a plain snapshot poll rather than a true MJPEG push stream: this board
// already accepts blocking single-threaded network calls everywhere else
// (see e.g. camS3.ino's sendJpegBytesToAtom(), openai.cpp) rather than
// adding concurrency, and a second task/core hammering
// esp_camera_fb_get() while a real capture might be in flight on the
// main one would risk racing the camera driver — this way there's still
// only ever one task calling it.
void handleCameraLive() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        sendPlain(503, "camera not ready");
        return;
    }
    server.setContentLength(fb->len);
    server.send(200, "image/jpeg", "");
    server.client().write(fb->buf, fb->len);
    esp_camera_fb_return(fb);
}

void handleOwnCameraPrint() {
    String error;
    if (!OwnCamera::confirmPrint(error)) {
        sendPlain(400, error.length() ? error : "no pending frame");
        return;
    }
    sendPlain(200, "OK");
}

void handleOwnCameraDiscard() {
    OwnCamera::discardPending();
    sendPlain(200, "OK");
}

// Mirrors camS3.ino's own wifiPrefs (same "camS3"/"ssid"/"pass" keys) —
// a second Preferences handle onto the same NVS namespace, so clearing it
// here is visible to ensureWifiConnected() on the next boot without
// needing a shared global.
void handleWifiForget() {
    Preferences wifiPrefs;
    wifiPrefs.begin("camS3", false);
    wifiPrefs.clear();
    wifiPrefs.end();
    sendPlain(200, "OK — restarting");
    server.client().flush();
    delay(200);
    ESP.restart();
}

void handleWifiScan() { server.send(200, "application/json", wifiScanJson()); }

void handleWifiConnect() {
    if (!server.hasArg("ssid") || server.arg("ssid").length() == 0) {
        sendPlain(400, "ssid required");
        return;
    }
    String ssid = server.arg("ssid");
    String password = server.hasArg("password") ? server.arg("password") : "";
    bool ok = wifiConnectAndSave(ssid, password);
    sendPlain(ok ? 200 : 400, ok ? "OK" : "Failed to join that network");
}

// ---- direct-mode-only routes ----

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

void handleStorageSettingsGet() {
    bool sd = Storage::backend() == Storage::Backend::kSdCard;
    String json = "{\"backend\":\"" + String(sd ? "sd" : "flash") + "\",\"sdAvailable\":" +
                  String(Storage::sdAvailable() ? "true" : "false") + "}";
    server.send(200, "application/json", json);
}

void handleStorageSettingsSet() {
    if (!server.hasArg("backend")) {
        sendPlain(400, "backend required (flash|sd)");
        return;
    }
    String b = server.arg("backend");
    if (b != "flash" && b != "sd") {
        sendPlain(400, "backend must be 'flash' or 'sd'");
        return;
    }
    bool ok = Storage::setBackend(b == "sd" ? Storage::Backend::kSdCard : Storage::Backend::kFlash);
    sendPlain(ok ? 200 : 400, ok ? "OK" : "SD card not available");
}

void handleDefaultAdjustGet() {
    String json = "{\"brightness\":" + String(DefaultAdjust::brightness()) +
                  ",\"contrast\":" + String(DefaultAdjust::contrast()) + "}";
    server.send(200, "application/json", json);
}

void handleDefaultAdjustSet() {
    if (!server.hasArg("brightness") || !server.hasArg("contrast")) {
        sendPlain(400, "brightness and contrast required");
        return;
    }
    DefaultAdjust::setAdjust(server.arg("brightness").toInt(), server.arg("contrast").toInt());
    sendPlain(200, "OK");
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
    bool showUrl = server.hasArg("showUrl") && server.arg("showUrl") == "1";
    Printer::reset();
    Printer::printQRCode(data);
    if (showUrl) Printer::printText(data);
    sendPlain(200, "OK");
}

uint16_t pendingWidth = 0;
uint16_t pendingHeight = 0;
String pendingLocation;
constexpr size_t kMaxLocationLen = 40;
bool imageInProgress = false;
uint16_t uploadGalleryId = 0;
uint16_t uploadBandHeight = 0;

void handleImageBegin() {
    if (!server.hasArg("w") || !server.hasArg("h")) {
        sendPlain(400, "w and h required");
        return;
    }
    uint16_t w = server.arg("w").toInt();
    uint16_t h = server.arg("h").toInt();
    if (w != Printer::kPrintWidthDots || h == 0 || h > kMaxHeightDots) {
        sendPlain(400, "width must be " + String(Printer::kPrintWidthDots) + ", height 1-" + String(kMaxHeightDots));
        return;
    }
    pendingWidth = w;
    pendingHeight = h;
    pendingLocation = server.hasArg("location") ? server.arg("location") : "";
    if (pendingLocation.length() > kMaxLocationLen) pendingLocation = pendingLocation.substring(0, kMaxLocationLen);
    sendPlain(200, "OK");
}

void handleImageUploadComplete() {
    if (pendingWidth == 0) {
        sendPlain(400, "call /api/print/image/begin first");
        return;
    }
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
            uploadBandHeight = (Clock::isSynced() && pendingHeight > Caption::kBandHeight) ? Caption::kBandHeight : 0;
            Printer::beginRaster(pendingWidth, pendingHeight + uploadBandHeight);
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
            if (uploadGalleryId != 0) Gallery::endSave();
            imageInProgress = false;
        }
    }
}

constexpr const char *kExternalPhotoTmpPath = "/tmp_ext_photo.jpg";
constexpr size_t kMaxExternalPhotoBytes = 400 * 1024;
bool externalPhotoOk = false;
size_t externalPhotoBytesWritten = 0;
File externalPhotoFile;

void handleExternalPhotoChunk() {
    HTTPUpload &upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        externalPhotoBytesWritten = 0;
        externalPhotoFile = Storage::fs().open(kExternalPhotoTmpPath, "w");
        externalPhotoOk = (bool)externalPhotoFile;
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (externalPhotoOk) {
            externalPhotoBytesWritten += upload.currentSize;
            if (externalPhotoBytesWritten > kMaxExternalPhotoBytes) {
                externalPhotoOk = false;
                externalPhotoFile.close();
                Storage::fs().remove(kExternalPhotoTmpPath);
            } else {
                externalPhotoFile.write(upload.buf, upload.currentSize);
            }
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (externalPhotoFile) externalPhotoFile.close();
    }
}

void handleExternalPhotoByUrl() {
    String label = server.hasArg("label") ? server.arg("label") : "";
    String location = server.hasArg("location") ? server.arg("location") : "";
    if (!server.hasArg("url")) {
        sendPlain(400, "url required");
        return;
    }
    String error;
    bool ok = JpegPrint::fetchAndPrint(server.arg("url"), label, location, error);
    sendPlain(ok ? 200 : 400, ok ? "printed" : error);
}

void handleExternalPhotoComplete() {
    String label = server.hasArg("label") ? server.arg("label") : "";
    String location = server.hasArg("location") ? server.arg("location") : "";
    String error;
    if (!externalPhotoOk) {
        sendPlain(413, "upload failed or exceeded " + String(kMaxExternalPhotoBytes / 1024) + "KB");
        return;
    }
    bool ok = JpegPrint::printFromFile(kExternalPhotoTmpPath, label, location, error);
    Storage::fs().remove(kExternalPhotoTmpPath);
    sendPlain(ok ? 200 : 400, ok ? "printed" : error);
}

void handleGalleryList() {
    Gallery::Entry entries[Gallery::kMaxEntries];
    size_t count = Gallery::list(entries, Gallery::kMaxEntries);
    String json = "{\"maxEntries\":" + String((unsigned)Gallery::kMaxEntries) + ",\"entries\":[";
    for (size_t i = 0; i < count; i++) {
        if (i > 0) json += ",";
        json += "{\"id\":" + String(entries[i].id) + ",\"height\":" + String(entries[i].height) +
                ",\"bytes\":" + String((unsigned)entries[i].bytes) + ",\"label\":\"" +
                jsonEscape(entries[i].label) + "\",\"savedAt\":\"" + jsonEscape(entries[i].savedAt) + "\"}";
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
    File f = Storage::fs().open(path, "r");
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
    if (!Gallery::print((uint16_t)server.arg("id").toInt())) {
        sendPlain(404, "not found");
        return;
    }
    sendPlain(200, "OK");
}

// "Print whatever's most recent" — used by ATOM Lite's own button
// double-click (see ../../src/cams3_remote.*), which has no way to know
// individual gallery ids or whether a preview capture is awaiting
// confirmation. Prefers a pending OwnCamera preview frame (confirming it,
// same as pressing 「印刷」 on the 撮影 card would) over reprinting the
// newest already-saved gallery entry, since a pending frame is presumably
// the more recent/relevant one.
void handleGalleryReprintLatest() {
    if (OwnCamera::status().pendingPrint) {
        String error;
        if (OwnCamera::confirmPrint(error)) {
            sendPlain(200, "OK");
            return;
        }
        // fall through to the gallery, in case confirmPrint() somehow failed
    }
    Gallery::Entry newest;
    if (Gallery::list(&newest, 1) == 0) {
        sendPlain(404, "nothing to print");
        return;
    }
    if (!Gallery::print(newest.id)) {
        sendPlain(404, "not found");
        return;
    }
    sendPlain(200, "OK");
}

void handleGalleryDelete() {
    if (!server.hasArg("id")) {
        sendPlain(400, "id required");
        return;
    }
    if (!Gallery::remove((uint16_t)server.arg("id").toInt())) {
        sendPlain(404, "not found");
        return;
    }
    sendPlain(200, "OK");
}

void handleHaikuSettingsGet() {
    String json = "{\"poemType\":\"" + jsonEscape(Haiku::poemType().c_str()) + "\"" + ",\"author\":\"" +
                  jsonEscape(Haiku::author().c_str()) + "\"" + ",\"autoMode\":\"" +
                  jsonEscape(Haiku::autoMode().c_str()) + "\"}";
    server.send(200, "application/json", json);
}

void handleHaikuSettingsSet() {
    if (server.hasArg("poemType")) Haiku::setPoemType(server.arg("poemType"));
    if (server.hasArg("author")) Haiku::setAuthor(server.arg("author"));
    if (server.hasArg("autoMode")) Haiku::setAutoMode(server.arg("autoMode"));
    sendPlain(200, "OK");
}

void handleOpenAISettingsGet() {
    bool configured = OpenAI::hasKey();
    String json = "{\"configured\":" + String(configured ? "true" : "false") + "}";
    if (configured) json = "{\"configured\":true,\"apiKey\":\"" + jsonEscape(OpenAI::getKey().c_str()) + "\"}";
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
    server.on("/", HTTP_GET, handleRoot);
    server.onNotFound(handleRoot);
    server.on("/api/status", HTTP_GET, handleStatus);
    server.on("/api/mode", HTTP_GET, handleModeGet);
    server.on("/api/mode", HTTP_POST, handleModeSet);
    server.on("/api/camera/capture", HTTP_POST, handleCaptureRequest);
    server.on("/shutter", HTTP_GET, handleCaptureRequest);
    server.on("/api/wifi/forget", HTTP_POST, handleWifiForget);
    server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
    server.on("/api/wifi", HTTP_POST, handleWifiConnect);
    // 撮影方式 (auto/preview) and the pending-preview-frame routes apply
    // regardless of PrinterMode — see own_camera.h — so they're
    // registered unconditionally, unlike the direct-connect-only routes
    // below (which need a printer/gallery/storage this device only has
    // in kDirect mode).
    server.on("/api/camera/status", HTTP_GET, handleOwnCameraStatus);
    server.on("/api/camera/mode", HTTP_GET, handleOwnCameraModeGet);
    server.on("/api/camera/mode", HTTP_POST, handleOwnCameraModeSet);
    server.on("/api/camera/frame", HTTP_GET, handleOwnCameraFrame);
    server.on("/api/camera/print", HTTP_POST, handleOwnCameraPrint);
    server.on("/api/camera/discard", HTTP_POST, handleOwnCameraDiscard);
    server.on("/api/camera/live", HTTP_GET, handleCameraLive);
    // Also mode-independent — DefaultAdjust feeds every JpegPrint decode
    // (own captures and, in direct mode, uploaded/external photos alike).
    server.on("/api/default_adjust", HTTP_GET, handleDefaultAdjustGet);
    server.on("/api/default_adjust", HTTP_POST, handleDefaultAdjustSet);
    // Storage backend choice matters in kViaAtom mode too — own_camera.cpp's
    // preview-mode capture always writes its temp JPEG through
    // Storage::fs(), even when the eventual commit forwards to ATOM
    // rather than printing locally.
    server.on("/api/storage/settings", HTTP_GET, handleStorageSettingsGet);
    server.on("/api/storage/settings", HTTP_POST, handleStorageSettingsSet);

    if (PrinterMode::mode() == PrinterMode::Mode::kDirect) {
        server.on("/api/printer/settings", HTTP_GET, handlePrinterSettingsGet);
        server.on("/api/printer/settings", HTTP_POST, handlePrinterSettingsSet);
        server.on("/api/print/text", HTTP_POST, handlePrintText);
        server.on("/api/print/qr", HTTP_POST, handlePrintQr);
        server.on("/api/print/test", HTTP_POST, handlePrintTest);
        server.on("/api/print/image/begin", HTTP_POST, handleImageBegin);
        server.on("/api/print/image", HTTP_POST, handleImageUploadComplete, handleImageUploadChunk);
        server.on("/api/print/photo", HTTP_POST, handleExternalPhotoComplete, handleExternalPhotoChunk);
        server.on("/api/print/photo/url", HTTP_POST, handleExternalPhotoByUrl);
        server.on("/api/gallery", HTTP_GET, handleGalleryList);
        server.on("/api/gallery/frame", HTTP_GET, handleGalleryFrame);
        server.on("/api/gallery/print", HTTP_POST, handleGalleryPrint);
        server.on("/api/gallery/reprint-latest", HTTP_POST, handleGalleryReprintLatest);
        server.on("/api/gallery/delete", HTTP_POST, handleGalleryDelete);
        server.on("/api/haiku/settings", HTTP_GET, handleHaikuSettingsGet);
        server.on("/api/haiku/settings", HTTP_POST, handleHaikuSettingsSet);
        server.on("/api/openai/settings", HTTP_GET, handleOpenAISettingsGet);
        server.on("/api/openai/settings", HTTP_POST, handleOpenAISettingsSet);
    }

    server.begin();
}

void loop() { server.handleClient(); }

}  // namespace WebServer_
