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
再起動し、再びAPモードに入る。接続済みの状態から別のネットワークに切り替えたいだけなら、
リセットせずに設定タブの「Wi-Fi設定」カードからも変更できる（後述「使い方」参照）。

## 使い方

`http://m5web.local`（またはIPアドレス）を開くと、ヘッダーと接続状況の下に「プリント」
「設定」の2タブがある。

### プリントタブ

- **M5StickVカメラ**: UART直結したM5StickVで撮った写真を確認・印刷する（詳細は
  [M5StickVカメラ連携](#m5stickvカメラ連携)）。
- **画像を印刷**: 写真を選択すると自動的に印刷幅（384dot=58mm幅, 203dpi）に合わせてリサイズし、
  誤差拡散（Floyd–Steinberg）・網掛け・単純2値化のいずれかで白黒ビットマップに変換する。
  明るさ・コントラストは設定タブの「デフォルト」値から始まり、写真ごとに個別調整もできる。
  反転・90°回転にも対応。プレビューを確認してから「この画像を印刷」を押す。
  変換はすべてブラウザ側（Canvas）で行われるため、ATOM側の負荷やメモリ消費は最小限。
- **ギャラリー**: M5StickVカメラで撮った写真・アップロードした写真は、印刷時に自動的に
  ATOM Lite本体のフラッシュ（LittleFS）にも保存され、「ギャラリー」カードに一覧表示される。
  各写真はサムネイルと（あれば）検出ラベル・保存日付を表示のうえ「再印刷」「削除」ができる。
  最大20枚まで保存され、いっぱいになると新しい保存はスキップされる（シリアルログに記録）ので、
  不要な写真は「削除」ボタンで消すこと。
- **Put TEXT for print**: テキストエリアに入力した文章をそのまま印刷する（動作確認・簡易メモ用）。
- **QRコードを印刷**: URLなどの文字列をQRコードにして印刷する。QR自体の生成はブラウザ側ではなく
  プリンター内蔵の2Dシンボル生成機能（GS ( k コマンド）で行う。

### 設定タブ

- **デフォルト: 明るさ・コントラスト**: M5StickVカメラ・写真アップロードの両方に使われる
  初期値（-100〜100、既定0）。ATOM Lite本体のNVSに保存され再起動後も保持される。M5StickV
  カメラは**次に受信するフレームから**この値がサーバー側で適用される（既に受信・表示中の
  1枚には遡って反映されない）。写真アップロードは**次に選択する写真の初期値**として使われ、
  選択後は「画像を印刷」カードで個別に調整できる（この個別調整はデフォルト値自体を書き換えない）。
- **M5StickV印刷モード**: 事後閲覧方式／プレビュー確認方式の切り替え（詳細は後述
  「Web上での確認・印刷モード」参照）。
- **Wi-Fi設定**: SSID・パスワードを入力して「保存」すると、接続中でも別のネットワークへの
  切り替えができる（`/api/wifi`は初回セットアップ時のAPモード用カードと共用）。接続に
  成功すればそのまま新しいネットワークで動作し、失敗すれば自動でAP設定モードに戻るため、
  誤ったパスワードを入れても本機にアクセスできなくなることはない。接続中のSSIDは開いた
  時点で自動的に入力欄へプリセットされる（パスワードは保存されないため毎回入力が必要）。

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
3. M5StickVのボタン(A)を押すと、画面を白く光らせ内蔵RGB LEDも白色点灯・シャッター音を鳴らして
   撮影→384dot幅にリサイズ→ATOM Lite側へ送信。送信が完了するとRGB LEDが緑色に一瞬点灯して消灯する
   （白=処理中、緑=送信完了の合図）。届いた画像はATOM Lite側で明るさ・コントラスト調整と
   ディザリング（誤差拡散、`src/dither.cpp`）を経てRAMに保持され、m5webページの
   「M5StickVカメラ」カードで確認できる（M5StickV側はグレースケール画像を送るだけでよい）。
   シャッター音はベストエフォートで、`shutter.wav`が無い/読めない場合もエラーにせず撮影・送信は
   続行される。

### カメラの向き

M5StickVはセンサーが本体に対して物理的に回転した状態で実装されているため、そのままだと
LED側を上にして構えた時に撮影される写真が横倒しになる（実機で確認済み・時計回りに90°回転が
必要）。本来は`sensor.set_transpose()`でセンサー側で（ほぼノーコストで）補正したかったが、
今回のMaixPyビルドには存在せず`AttributeError`になったため、`m5web_capture.py`の
`send_frame()`内でソフトウェア回転している。

- **メモリ制約により、リサイズ＋回転済みの画像バッファを一度に丸ごと作ることができない**。
  実機で384×512（約196KB）程度の中間バッファを確保しようとしただけで
  `MemoryError: memory allocation failed`が発生することを確認済み（このボードの
  MicroPythonヒープはかなり小さい/断片化しているらしく、640×480=307KBのセンサーの
  フレームバッファ自体は問題なく存在するのに、そこから384×512程度の派生コピーを
  作ろうとすると足りない）。そのため`send_frame()`は384バイトの行バッファ1つだけを
  使い回し、`img.get_pixel(x, y)`で元のフレームバッファから直接1ピクセルずつ読みながら
  ATOM Lite側へ1行ずつストリーミング送信する（リサイズと回転の座標変換を1回のピクセル
  参照にまとめてある）。約20万回の`get_pixel()`呼び出しになるぶん遅くはなるが、
  どのみちシャッター音・UART転送（M5StickV-ATOM Lite間は115200bps、384×512dotで
  約17秒程度）で数秒〜数十秒かかる処理なので、メモリを使い切って落ちるよりはこちらを
  優先した。
- **補正されるのはATOM Lite側へ転送される写真のみ**。M5StickV自身のライブプレビュー画面は
  向きを直していない（毎フレーム回転すると重すぎるため）ので、構えている間の画面表示は
  そのまま（横倒しの向き）で、撮影・送信した写真だけ正しい向きになる。
- 別の回転が必要になった場合は、`send_frame()`内の座標変換式を直接書き換えること
  （関数のdocstringに式の導出根拠を書いてある）。

### 顔検出

M5StickV側でHaar cascade（`image.HaarCascade("frontalface")`、MaixPy標準搭載・別途kmodelファイル不要）
による顔検出を常時実行する。

- **ライブプレビュー**: 画面に顔が写っていれば、その周りに白い枠を表示する（速度確保のため
  縮小コピー上で検出→座標を元解像度に拡大して描画。`FACE_DETECT_EVERY_N_FRAMES`フレームごとに
  再検出し、間のフレームは直前の枠を使い回す）。
- **撮影時**: シャッターを押した瞬間のフレームでもう一度検出し、「Face」（複数人なら
  「Face x2」など）というラベルを生成。ATOM Lite側への画像転送時に、この文字列を画像データと
  一緒に送る（顔が写っていなければラベルは空文字）。何も検出できなかった場合はラベルなしで
  送信されるだけで、撮影・印刷自体は通常どおり続行される。
- ATOM Lite側で受信したラベルは、m5webページの「M5StickVカメラ」カードで画像の下に
  「検出: Face」のように表示され、ギャラリーに保存された写真にもキャプションとして残る
  （`src/gallery.cpp`がPreferences (NVS) に写真IDごとに保存）。印刷時にもこのラベルが
  使われる（後述「印刷時のキャプション」参照）。

顔以外の判定はまだ実装していない（K210のKPUを使った物体分類などを追加すれば、同じラベル欄に
乗せて拡張できる設計にはしてある）。

### 印刷時のキャプション（時刻・検出ラベル）

すべての印刷（M5StickVカメラの自動印刷・「もう一度印刷」・ギャラリーからの再印刷・写真
アップロード印刷）で、`src/caption.*`が「印刷した時刻」（＋M5StickVカメラ経由なら検出
ラベルも）を画像に直接焼き込んでから印刷する（例:「14:32」「FACE 14:32」）。

- **画像の一番下に、白帯＋黒文字で"追加"される**（既存のピクセルを上書きするのではなく、
  印刷する行数そのものを帯の分だけ増やす）。写真の一部を覆い隠すことがない
  （例えば顔が画像の下端近くに写っていても、キャプションがその顔と重ならない）。
- フォントは`src/font.*`の自前5x7ドットマトリクス（半角英数字のみ、小文字は大文字化して
  描画、対応外の文字は空白になる）。
- ATOM Lite側のRAM/LittleFS上の元データや、Web UI・ギャラリーに表示される画像は一切
  書き換えない。印刷ジョブに送る直前に、その場でキャプション帯を生成して追加するだけ。
- **時刻はNTPで取得**（`src/clock.cpp`、JST固定・サマータイムなし）。Wi-Fi接続成功時に
  バックグラウンドで同期を開始するため、起動直後や同期が完了する前（数秒程度）は時刻
  キャプションを省略する（検出ラベルがあればラベルだけは付く）。AP設定モードのまま
  （インターネット未接続）の場合は同期できないため、時刻キャプションは常に省略される。
- 画像の高さがキャプション帯（18dot）以下の極端に小さい写真では、キャプション自体を
  省略する。

### Web上での確認・印刷モード

m5webページの設定タブ「M5StickV印刷モード」カードで、受信した写真の扱いを2種類から選べる
（設定はATOM Lite本体のNVSに保存され再起動後も保持される）。

- **事後閲覧方式（デフォルト）**: 受信次第すぐ印刷し、直近の1枚をプリントタブの
  「M5StickVカメラ」カードで確認できる（印刷前の確認・キャンセルはできない）。
- **プレビュー確認方式**: 受信してもすぐには印刷せず、「M5StickVカメラ」カードに表示して
  「印刷」または「破棄」を選ぶまで待機する。

どちらのモードでも、直近の1枚は常にATOM Lite側のRAMに残っているので、「M5StickVカメラ」
カードの「印刷」（確認待ちでなければ「もう一度印刷」ボタンになる）で**何度でも再印刷**できる。
加えて、**ATOM Lite本体のボタンを短く押す**と、そのときの直近フレームをWeb UIを開かずに
再印刷できる（5秒以上長押しするとWi-Fi設定リセットになるので、短押しであること）。

いずれのモードでも、フレーム全体（ディザリング後の1bppビットマップ）をATOM Lite側のRAMに
保持するため、カメラ経由の1枚あたりの最大高さは800dot（約100mm）に制限している
（`src/camera_link.cpp`の`kMaxHeightDots`、写真アップロードAPIの2000dotとは別の上限）。

明るさ・コントラストの初期設定は設定タブの「デフォルト: 明るさ・コントラスト」カードにまとめて
あり（写真アップロードと共通、詳細は前述「設定タブ」参照）、こちらも**次に受信するフレームから**
M5StickV側でグレースケール→ディザリング変換する前に適用される。既に受信・表示中の1枚には
遡って反映されない。

### ATOM Lite本体のRGB LED

ATOM Lite本体にも内蔵RGB LED（G27, SK6812）があり、M5StickV側のLED（白=処理中/緑=送信完了）とは
独立して、`src/led.*`が以下のように点灯制御する。

- **緑点滅5回**: 新しい写真がギャラリーに保存されたとき（M5StickVカメラ撮影・写真アップロード
  印刷のどちらでも、自動印刷/プレビュー確認方式を問わず発生）。
- **緑点灯（点滅なし・印刷できる写真があるあいだずっと点灯）**: 次のいずれかが真のあいだ点灯し続ける。
  - ギャラリーに写真が1枚以上保存されている（＝いつでも再印刷できる状態。起動時に既存の保存枚数を
    見て復元されるため、再起動をまたいでも正しく点灯する）。
  - プレビュー確認方式で、印刷するかどうかの確認待ちの写真がある（「印刷」または「破棄」で解消）。
  
  ギャラリーが空になり、かつ確認待ちの写真も無くなったときのみ消灯する。

### 通信プロトコル

```
"M5PV" (4 byte) | width u16 LE | height u16 LE | labelLen u8 | label (labelLen byte, UTF-8)
| width*height byte グレースケール(行優先) | checksum 1 byte
```
`width`は384固定（`Printer::kPrintWidthDots`と一致必須）。`label`は顔検出などの結果を表す短い文字列
（例: `"Face"`）で、何も無ければ`labelLen`=0で省略される（`src/camera_link.h`の`kMaxLabelLen`=31バイト
上限、超過分はATOM Lite側で切り詰められるが、ストリームがズレないよう受信自体は宣言された長さぶん
必ず読み切る）。チェックサムはlabelバイト列＋全ピクセルバイトのmod 256の和。フレーム全体を受信・
ディザリングし終えてから印刷するかどうかを判断する設計のため、チェックサムが不一致だった場合は
事後閲覧方式であっても自動印刷せず、確認待ち状態にして印刷を止める。

### 既知の注意点

このカメラ連携機能は実機での動作確認ができていない（MaixPyの公開ドキュメント・実例を根拠に実装）。
特に以下は要確認:

- MaixPyの`img.to_bytes()`がグレースケール画像で行優先(row-major)のバイト列を返す前提で実装している。
  もし印刷結果が斜めにズレる等おかしければ、スクリプト内にコメントアウトしてある`get_pixel(x,y)`版の
  ループに切り替えること（遅いが確実）。
- `board_info.BUTTON_A`はM5StickVのボタンAを指す想定（Sipeed公式のMaixPyサンプルに準拠）。
- シャッター音は`board_info.SPK_SD`/`SPK_DIN`/`SPK_BCLK`/`SPK_LRCLK` + `I2S.DEVICE_1`という
  M5Stack公式サンプルのピン割り当てに準拠しているが、こちらも実機未確認。
- 内蔵RGB LEDは`board_info.LED_W`/`LED_G`（アクティブLow、`value(0)`で点灯）を使用。
  `fm.fpioa.GPIO2`/`GPIO3`に割り当てている（`GPIO0`はスピーカー、`GPIO1`はボタンAが使用中のため）。
- 顔検出（`image.HaarCascade("frontalface")`, `find_features(threshold=0.5, scale_factor=1.25)`）は
  実機で`MemoryError: Out of normal MicroPython Heap Memory!`が発生することを確認済み。原因は
  Haar cascade自体が入力画像サイズに比例した内部スクラッチバッファをヒープから確保するため
  （K210のMicroPythonヒープは小さい）。対策として`detect_faces()`は縮小コピー上でのみ実行し、
  `MemoryError`も捕捉して「顔なし」として処理を継続する（撮影・印刷自体は止まらない）。
  ただし最初に縮小しすぎ（`FACE_DETECT_DIVISOR_LIVE`=8, `_CAPTURE`=5 → 80×60/128×96）て
  クラッシュは止まったが顔が全く検出されなくなったため、現在は`_LIVE`=4, `_CAPTURE`=3
  （160×120/213×160）に戻してある。`MemoryError`のログ（シリアルモニタに
  `face detection skipped (out of heap)`と出る）が再発する場合は各`FACE_DETECT_DIVISOR_*`を
  大きく、逆に検出が弱い場合は小さくして調整すること（ライブ検出時は毎フレーム
  `live face detect: N found`もログに出るので、検出自体が動いているか確認できる）。
  誤検出/未検出の多さは`threshold`/`scale_factor`でも調整する。

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
  gallery.*            印刷した写真をLittleFSに保存・一覧・削除・再印刷する履歴機能
  led.*                ATOM Lite内蔵RGB LEDの点灯制御（新着通知の点滅・確認待ちの点灯）
  font.*               5x7ドットマトリクスフォント（半角英数字のみ）の描画
  caption.*            fontを使って印刷時刻・検出ラベルを印刷画像に追加する（白帯+黒文字）
  clock.*              NTPによる時刻同期（JST固定）。captionの時刻表示・ギャラリーの保存日付に使用
data/
  index.html          m5web本体（UI + Canvas画像変換, 外部CDN依存なし・単一ファイル）
arduino/m5web/
  m5web.ino + 同名の*.h/*.cpp/data/  Arduino IDE用の手動同期コピー（上記と同内容）
maixpy/
  m5web_capture.py    M5StickV側スクリプト（撮影→シャッター音→リサイズ→UART送信）
  shutter.wav          シャッター音（16kHz/mono/16bit, 180ms、m5web_capture.pyが再生）
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
| GET | `/api/camera/status` | M5StickVカメラの状態JSON (`mode`,`frameReady`,`pendingPrint`,`width`,`height`,`frameSeq`,`brightness`,`contrast`,`label`) |
| POST | `/api/camera/mode` | `mode`=`auto`または`preview` (form) で確認モードを切り替え（再起動後も保持） |
| POST | `/api/camera/settings` | `brightness`,`contrast` (form, -100〜100) で次フレームからの既定調整値を設定（再起動後も保持） |
| GET | `/api/camera/frame` | 直近フレームの1bpp生ビットマップ（`X-Frame-Width`/`X-Frame-Height`ヘッダ付き） |
| POST | `/api/camera/print` | 直近フレームを印刷（確認待ちならそれを確定、そうでなければ再印刷） |
| POST | `/api/camera/discard` | プレビュー確認方式で確認待ちのフレームを破棄 |
| GET | `/api/gallery` | 保存済み写真の一覧JSON (`maxEntries`, `entries[]`=`id`,`height`,`bytes`,`label`,`savedAt`) |
| GET | `/api/gallery/frame` | `id` (query) で指定した写真の1bpp生ビットマップ（`X-Frame-Width`/`X-Frame-Height`ヘッダ付き） |
| POST | `/api/gallery/print` | `id` (query) で指定した写真を再印刷 |
| POST | `/api/gallery/delete` | `id` (query) で指定した写真をLittleFSから削除 |

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
