# m5web

M5Stack **ATOM Printer Kit**（ATOM Lite + 58mm サーマルプリンター）用のファームウェア。
Wi-Fiに接続し、`m5web` というWebページをホストする。iPhoneなどからそのページを開いて
写真をアップロードすると、プリンター用の白黒ビットマップに変換してサーマルプリンターから
印刷する。テキスト・QRコードをそのまま印刷する機能もある。M5StickVカメラをUARTで直結して
撮った写真をそのまま印刷することもできる（[M5StickVカメラ連携](#m5stickvカメラ連携)）。

## ハードウェア

ATOM Printer Kit の標準配線（[公式ドキュメント](https://docs.m5stack.com/en/atom/atom_printer)）:

| ATOM Lite | プリンター |
|---|---|
| G23 (TX) | RX |
| G33 (RX) | TX |
| 5V/GND   | 電源 |

プリンター本体には別途 **DC12V 2.5A以上** のACアダプタが必要（USB給電のATOM Liteとは別系統）。
UARTは 9600bps 8N1 固定。

対応機種は ATOM Lite（`m5stack-atom`, ESP32-PICO-D4）を前提にしている。ATOMS3などピン配置が
異なる機種を使う場合は `src/printer.cpp` の `kRxPin`/`kTxPin` と `platformio.ini` の `board` を
変更すること。

## 書き込み手順

PlatformIO / Arduino IDE のどちらでも書き込める。中身は同じファームウェア。

### PlatformIO（推奨）

VSCode + PlatformIO拡張、または `pio` CLI を使う。

```bash
pio run -t upload      # ファームウェア書き込み
pio run -t uploadfs    # data/index.html (Web UI) を書き込み — 初回・更新時は必ず実行
pio device monitor      # シリアルログ確認（115200bps）
```

`uploadfs` を忘れると `/` にアクセスした際 "index.html missing" と表示される。

### Arduino IDE

スケッチは [`arduino/m5web/m5web.ino`](arduino/m5web/m5web.ino)（`src/`・`data/` と同内容のフラット構成コピー。
Arduino IDEはスケッチフォルダ直下にファイルを置く必要があるため、PlatformIO用の`src/`とは別に用意している。
片方を直したらもう片方にも反映すること）。

1. ボードマネージャURLに `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   を追加し、"esp32 by Espressif Systems" をインストール。
2. ボードに **M5Atom** を選択。
3. `ツール > Partition Scheme` はSPIFFS/LittleFS領域のあるもの（例: "Default 4MB with spiffs"）を選択。
4. `arduino/m5web/m5web.ino` を開いて スケッチ > マイコンボードに書き込む。
5. `data/index.html` はLittleFSへの書き込みが別途必要。Arduino IDE 2.xの場合は
   [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) プラグインを
   インストールし、コマンドパレットから "Upload LittleFS to Pico/ESP8266/ESP32" を実行する
   （IDE 1.8系なら同等の "ESP32 Sketch Data Upload" ツールを使う）。

## 初回セットアップ（Wi-Fi）

1. 書き込み直後はWi-Fi設定が無いため、本機が `m5web-setup-XXXX` という名前のオープンAPを
   立ち上げる。iPhoneのWi-Fi設定からこのAPに接続する。
2. ブラウザで `http://192.168.4.1` を開く（キャプティブポータルとして自動的に開くこともある）。
3. 表示された「Wi-Fi設定」カードから接続したいネットワークを選び、パスワードを入力して
   「接続する」を押す。
4. 接続に成功すると本機のAPは終了し、指定したWi-Fiにクライアントとして参加する。iPhoneのWi-Fi
   を元のネットワークに戻し、`http://m5web.local` を開く（mDNS対応）。IPアドレスは
   `pio device monitor` のログでも確認できる。

Wi-Fi設定をやり直したい場合は、ATOM本体のボタンを5秒以上長押しすると保存済みの設定を消去して
再起動し、再びAPモードに入る。

## 使い方

`http://m5web.local`（またはIPアドレス）を開くと以下ができる。

- **画像を印刷**: 写真を選択すると自動的に印刷幅（384dot=58mm幅, 203dpi）に合わせてリサイズし、
  誤差拡散（Floyd–Steinberg）・網掛け・単純2値化のいずれかで白黒ビットマップに変換する。
  明るさ・コントラスト・反転・90°回転にも対応。プレビューを確認してから「この画像を印刷」を押す。
  変換はすべてブラウザ側（Canvas）で行われるため、ATOM側の負荷やメモリ消費は最小限。
- **Put TEXT for print**: テキストエリアに入力した文章をそのまま印刷する（動作確認・簡易メモ用）。
- **QRコードを印刷**: URLなどの文字列をQRコードにして印刷する。QR自体の生成はブラウザ側ではなく
  プリンター内蔵の2Dシンボル生成機能（GS ( k コマンド）で行う。
- **M5StickVカメラ**: UART直結したM5StickVで撮った写真を確認・印刷する（詳細は
  [M5StickVカメラ連携](#m5stickvカメラ連携)）。

## M5StickVカメラ連携

M5StickV（Kendryte K210 AIカメラ）で撮った写真を、WiFiを介さず**直結シリアルで**そのまま印刷できる。
M5StickVにはWiFiが無いため、iPhoneからの利用とは別経路として用意している。

### 配線

Grove 4ピン（GND + 信号線2本のみ結線。VCC同士は繋がない — 両ボードとも別々に電源供給されるため）。

| M5StickV | ATOM Lite | 備考 |
|---|---|---|
| G34 (UART1 TX) | G32 (RX) | 画像データはこちらの一方向のみ |
| G35 (UART1 RX) | G26 (TX) | 現状未使用（将来のACK/ステータス返信用に配線だけしておく） |
| GND | GND | |

### 導入手順

1. [`maixpy/m5web_capture.py`](maixpy/m5web_capture.py) と [`maixpy/shutter.wav`](maixpy/shutter.wav)
   の両方をMaixPy IDE経由でM5StickVに書き込む。`shutter.wav`は`/flash/shutter.wav`に置く
   （SDカード運用なら`m5web_capture.py`冒頭の`SHUTTER_WAV`を`/sd/shutter.wav`に書き換える）。
   `m5web_capture.py`は`main.py`として保存するか、IDEから直接実行する。
2. ATOM Lite側は`CameraLink`モジュールが常時UART1(G26/G32, 115200bps)を待ち受けるので、
   ファームウェア側の追加設定は不要（通常のPlatformIO/Arduino書き込みのみでOK）。
3. M5StickVのボタン(A)を押すとシャッター音を鳴らして撮影→384dot幅にリサイズ→ATOM Lite側へ
   送信。届いた画像はATOM Lite側で明るさ・コントラスト調整とディザリング（誤差拡散、
   `src/dither.cpp`）を経てRAMに保持され、m5webページの「M5StickVカメラ」カードで確認できる
   （M5StickV側はグレースケール画像を送るだけでよい）。シャッター音はベストエフォートで、
   `shutter.wav`が無い/読めない場合もエラーにせず撮影・送信は続行される。

### Web上での確認・印刷モード

m5webページの「M5StickVカメラ」カードで、受信した写真の扱いを2種類から選べる（設定はATOM Lite
本体のNVSに保存され再起動後も保持される）。

- **事後閲覧方式（デフォルト）**: 受信次第すぐ印刷し、直近の1枚をWebページ上で確認できる
  （印刷前の確認・キャンセルはできない）。
- **プレビュー確認方式**: 受信してもすぐには印刷せず、Webページに表示して「印刷」または「破棄」を
  選ぶまで待機する。

いずれのモードでも、フレーム全体（ディザリング後の1bppビットマップ）をATOM Lite側のRAMに
保持するため、カメラ経由の1枚あたりの最大高さは800dot（約100mm）に制限している
（`src/camera_link.cpp`の`kMaxHeightDots`、写真アップロードAPIの2000dotとは別の上限）。

同じカードに「初期設定: 明るさ／コントラスト」スライダーがあり（-100〜100、既定0）、これも
NVSに保存される。ここで設定した値は**次に受信するフレームから**M5StickV側でグレースケール→
ディザリング変換する前に適用される（画像アップロードカードと同じ計算式）。既に受信・表示中の
1枚には遡って反映されない。

### 通信プロトコル

```
"M5PV" (4 byte) | width u16 LE | height u16 LE | width*height byte グレースケール(行優先) | checksum 1 byte
```
`width`は384固定（`Printer::kPrintWidthDots`と一致必須）。チェックサムは全ピクセルバイトのmod 256の和。
フレーム全体を受信・ディザリングし終えてから印刷するかどうかを判断する設計のため、チェックサムが
不一致だった場合は事後閲覧方式であっても自動印刷せず、確認待ち状態にして印刷を止める。

### 既知の注意点

このカメラ連携機能は実機での動作確認ができていない（MaixPyの公開ドキュメント・実例を根拠に実装）。
特に以下は要確認:

- MaixPyの`img.to_bytes()`がグレースケール画像で行優先(row-major)のバイト列を返す前提で実装している。
  もし印刷結果が斜めにズレる等おかしければ、スクリプト内にコメントアウトしてある`get_pixel(x,y)`版の
  ループに切り替えること（遅いが確実）。
- `board_info.BUTTON_A`はM5StickVのボタンAを指す想定（Sipeed公式のMaixPyサンプルに準拠）。
- シャッター音は`board_info.SPK_SD`/`SPK_DIN`/`SPK_BCLK`/`SPK_LRCLK` + `I2S.DEVICE_1`という
  M5Stack公式サンプルのピン割り当てに準拠しているが、こちらも実機未確認。

## 制限事項

- 印刷幅は58mmヘッド固定の384dot。
- 1回の印刷の高さは既定で最大2000dot（約250mm）。それを超える画像は下部が切り詰められる
  （`src/web_server.cpp` の `kMaxHeightDots` で変更可）。
- 9600bps固定のため、印刷サイズが大きいほど転送に時間がかかる（384×2000dotで概算50秒程度）。
- Wi-Fi設定用APはオープンネットワーク（パスワード無し）。設置環境に応じて
  `src/wifi_manager.cpp` の `WiFi.softAP(apName)` にパスワード引数を足すこと。

## 構成

```
platformio.ini
src/
  main.cpp           setup/loop の配線
  wifi_manager.*      Wi-Fi(STA)接続 / APフォールバック+キャプティブポータル / mDNS
  printer.*           UART経由でのESC/POS風ラスター送信・テキスト/QR送信（GS v 0, GS ( k）
  web_server.*        m5webのHTTP API (状態取得, Wi-Fi設定, 画像/テキスト/QR印刷)
  dither.*            誤差拡散(Floyd–Steinberg)の行ストリーミング実装（camera_linkが使用）
  camera_link.*        M5StickVからのUART直結カメラ画像受信 → バッファ保持 → 事後閲覧/プレビュー確認
data/
  index.html          m5web本体（UI + Canvas画像変換, 外部CDN依存なし・単一ファイル）
arduino/m5web/
  m5web.ino + 同名の*.h/*.cpp/data/  Arduino IDE用の手動同期コピー（上記と同内容）
maixpy/
  m5web_capture.py    M5StickV側スクリプト（撮影→シャッター音→リサイズ→UART送信）
  shutter.wav          シャッター音（8kHz/mono/16bit、m5web_capture.pyが再生）
```

## API

Web UIが使っているものと同じHTTP APIを、プログラムから直接叩ける。認証は無い（同一LAN内での
利用を前提）ので、`curl`やPythonの`requests`などから直接キックできる。ベースURLは
`http://m5web.local`（またはIPアドレス）。

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/api/status` | 接続状態のJSON (`mode`,`connected`,`ip`,`ssid`,`printWidthDots`,`maxHeightDots`) |
| GET | `/api/wifi/scan` | 近隣Wi-FiのJSON配列 |
| POST | `/api/wifi` | `ssid`,`password` (form) でWi-Fi接続 |
| POST | `/api/print/text` | `text` (form) をそのまま印刷 |
| POST | `/api/print/qr` | `url` (form) をQRコードにして印刷（変換はプリンター内蔵機能） |
| POST | `/api/print/test` | 配線確認用の固定テストページを印刷（UI上のボタンは無いが引き続き利用可） |
| POST | `/api/print/image/begin` | `w`,`h` (form) で次の画像サイズを予約 (wは384固定, h<=`maxHeightDots`) |
| POST | `/api/print/image` | multipart/form-dataで1bpp生ビットマップ本体をアップロード→即印刷 |
| GET | `/api/camera/status` | M5StickVカメラの状態JSON (`mode`,`frameReady`,`pendingPrint`,`width`,`height`,`frameSeq`,`brightness`,`contrast`) |
| POST | `/api/camera/mode` | `mode`=`auto`または`preview` (form) で確認モードを切り替え（再起動後も保持） |
| POST | `/api/camera/settings` | `brightness`,`contrast` (form, -100〜100) で次フレームからの既定調整値を設定（再起動後も保持） |
| GET | `/api/camera/frame` | 直近フレームの1bpp生ビットマップ（`X-Frame-Width`/`X-Frame-Height`ヘッダ付き） |
| POST | `/api/camera/print` | プレビュー確認方式で確認待ちのフレームを印刷 |
| POST | `/api/camera/discard` | プレビュー確認方式で確認待ちのフレームを破棄 |

### curl例

```bash
HOST=http://m5web.local

# テキスト印刷
curl -X POST "$HOST/api/print/text" --data-urlencode "text=Hello from a script"

# QRコード印刷
curl -X POST "$HOST/api/print/qr" --data-urlencode "url=https://example.com"

# テストページ
curl -X POST "$HOST/api/print/test"

# 状態確認
curl "$HOST/api/status"
```

### 画像印刷（プログラムから）

`/api/print/image`が受け取るのはPNG/JPEGではなく、**幅384dot固定・1bpp・MSB-first・1=黒**に
パックした生バイナリ（Web UIがブラウザのCanvasで作っているものと同じ形式）。Pythonから叩く例
（Pillowで384px幅にリサイズ→2値化→パック）:

```python
import requests
from PIL import Image

HOST = "http://m5web.local"
PRINT_WIDTH = 384

img = Image.open("photo.jpg").convert("L")
h = round(PRINT_WIDTH * img.height / img.width)
img = img.resize((PRINT_WIDTH, h)).convert("1")  # Pillowの標準ディザリングで2値化

packed = bytearray((PRINT_WIDTH // 8) * h)
px = img.load()
for y in range(h):
    for x in range(PRINT_WIDTH):
        if px[x, y] == 0:  # 0=黒
            packed[y * (PRINT_WIDTH // 8) + x // 8] |= 0x80 >> (x % 8)

requests.post(f"{HOST}/api/print/image/begin", data={"w": PRINT_WIDTH, "h": h}).raise_for_status()
requests.post(f"{HOST}/api/print/image", files={"file": ("image.bin", bytes(packed), "application/octet-stream")}).raise_for_status()
```
