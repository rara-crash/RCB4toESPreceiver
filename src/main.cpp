// RCB4toESPreceiver - M5Stamp Pico を KRR-5FH に擬態させて RCB-4 にボタン情報を返す
// (動作実績のある D1mini + PS4Controller 版を StampPad に移植したもの)
//
// 配線 (M5Stamp Pico)
//   G25 : RCB-4 → Stamp (要求)   8E1, 非反転
//   G26 : Stamp → RCB-4 (応答)   8E1, 反転
//   G32 : 3ステートバッファのイネーブル (1.25Mbps 送信中だけ LOW)
//   G22 : 未使用 (INPUT)
//   RCB-4 とは ICS と同じ半二重の 1 本線を共有している (自分の応答も G25 に返ってくる)
//
// 通信
//   RCB-4 は約 10ms ごとに BF 7F 00 07 を送ってくる。応答は 18 バイト
//   (ヘッダ 3F 7F 00 07 + ボタン2・アナログ4・チェックサム1 の計 7 バイトを上位/下位ニブルに分けたもの)
//   RCB-4 が応答を待つ時間は非常に短く、要求の直後 (十数μs 以内) に送り始めないと無視される
//   (無視されるとサーボが脱力したまま・モーションが再生されない。エラーは出ない)
//
// タスク構成
//   コア0: Bluetooth (StampPad/BTGamepad 内部)
//   コア1: commTask  … RCB-4 の要求を待ち受けて応答 (最優先・受信待ちの間は CPU を使わない)
//          padTask   … StampPad 内部 250Hz
//          control() … 50Hz。PS(HOME) でライトバー色の切り替えだけ行う
#include <Arduino.h>
#include <StampPad.h>
#include "driver/uart.h"
#include "hal/uart_ll.h"

StampPad stamp;

#define RCB_RX Serial2  // RCB-4 → Stamp
#define RCB_TX Serial1  // Stamp → RCB-4
static constexpr int PIN_RX = 25;
static constexpr int PIN_TX = 26;
static constexpr int PIN_BUF_EN = 32;

static constexpr uint32_t BAUD_HIGH = 1250000;
static constexpr uint32_t BAUD_LOW = 115200;
static constexpr uint32_t DETECT_MS = 1000;    // 1.25Mbps でヘッダを探す時間
static constexpr uint32_t REDETECT_MS = 3000;  // この間要求が来なければ検出し直す

static const uint8_t REQ[4] = {0xBF, 0x7F, 0x00, 0x07};

static uint32_t curBaud = 0;

// ---------------------------------------------------------------------------
// 応答パケット
// ---------------------------------------------------------------------------
// スティック -128..127 → 0..127 (中立 64)。0 付近は不安定なので切り捨てる
static uint8_t analogValue(int8_t v) { return (abs(v) > 9) ? (uint8_t)(v / 2 + 64) : 64; }

static void buildPacket(const StampInput& in, uint8_t pkt[18]) {
  uint8_t b1 = 0, b2 = 0;
  uint8_t analog[4] = {64, 64, 64, 64};

  if (in.active()) {  // 未接続・通信途絶時は中立を返す
    if (in.button(GPButton::Square)) b1 |= 0x01;
    if (in.button(GPButton::L1)) b1 |= 0x02;
    if (in.button(GPButton::L2)) b1 |= 0x04;
    if (in.button(GPButton::R1)) b1 |= 0x08;
    if (in.button(GPButton::R2)) b1 |= 0x10;

    if (in.button(GPButton::Up)) b2 |= 0x01;
    if (in.button(GPButton::Down)) b2 |= 0x02;
    if (in.button(GPButton::Right)) b2 |= 0x04;
    if (in.button(GPButton::Left)) b2 |= 0x08;
    if (in.button(GPButton::Options)) b2 |= 0x01 | 0x02;  // L3 = 上下同時押し
    if (in.button(GPButton::Share)) b2 |= 0x04 | 0x08;    // R3 = 左右同時押し
    if (in.button(GPButton::Triangle)) b2 |= 0x10;
    if (in.button(GPButton::Cross)) b2 |= 0x20;
    if (in.button(GPButton::Circle)) b2 |= 0x40;

    analog[0] = analogValue(in.s.ly);
    analog[1] = analogValue(in.s.lx);
    analog[2] = analogValue(in.s.ry);
    analog[3] = analogValue(in.s.rx);
  }

  uint8_t sum = (0x80 + b1 + b2 + analog[0] + analog[1] + analog[2] + analog[3]) & 0x7F;
  const uint8_t data[7] = {b1, b2, analog[0], analog[1], analog[2], analog[3], sum};

  pkt[0] = 0x3F;
  pkt[1] = 0x7F;
  pkt[2] = 0x00;
  pkt[3] = 0x07;
  for (int i = 0; i < 7; i++) {
    pkt[4 + i * 2] = data[i] >> 4;
    pkt[5 + i * 2] = data[i] & 0x0F;
  }
}

// ---------------------------------------------------------------------------
// UART
// ---------------------------------------------------------------------------
static void openPorts(uint32_t baud) {
  RCB_TX.end();
  RCB_RX.end();
  RCB_TX.begin(baud, SERIAL_8E1, -1, PIN_TX, true);
  RCB_RX.begin(baud, SERIAL_8E1, PIN_RX, -1, false);
  RCB_RX.setTimeout(2);  // readBytes の待ち時間 (この間 CPU は他のタスクへ)
  // 要求 (4 バイト) が揃った瞬間に受信バッファへ渡す。既定 (1.25Mbps だと 120 バイト) のままだと
  // 受信タイムアウト待ちで約 90μs 遅れて応答が間に合わない。1 にすると全バイトで割り込みが入りコア1 が詰まる。
  // それ以外のデータ (サーボの通信など) は 1 文字分の無通信で渡される
  RCB_RX.setRxFIFOFull(sizeof(REQ));
  RCB_RX.setRxTimeout(1);
  curBaud = baud;
}

// 受信データをまとめて読む。来ていなければ最大 2ms 待つ
//   1 バイトずつ読むと連続受信で CPU を使い切り、他のタスクが動けなくなる
static size_t readChunk(uint8_t* buf, size_t cap) {
  if (RCB_RX.readBytes(buf, 1) != 1) return 0;
  size_t n = 1;
  int avail = RCB_RX.available();
  if (avail > 0) n += RCB_RX.read(buf + 1, min((size_t)avail, cap - 1));
  return n;
}

// 要求 BF 7F 00 07 を 1 バイトずつ照合する。受け切ったら true (チャンクをまたいでも判定できる)
static bool matchRequest(uint8_t c, uint8_t& matched) {
  if (c == REQ[matched]) {
    if (++matched == sizeof(REQ)) {
      matched = 0;
      return true;
    }
  } else {
    matched = (c == REQ[0]) ? 1 : 0;
  }
  return false;
}

// 1.25Mbps で要求が見つかれば 1.25Mbps、見つからなければ 115200
static void detectBaud() {
  openPorts(BAUD_HIGH);
  uint8_t buf[128];
  uint8_t matched = 0;
  uint32_t start = millis();
  while (millis() - start < DETECT_MS) {
    size_t n = readChunk(buf, sizeof(buf));
    for (size_t i = 0; i < n; i++) {
      if (matchRequest(buf[i], matched)) {
        Serial.println("[RCB] 1250000bps");
        return;
      }
    }
  }
  openPorts(BAUD_LOW);
  Serial.println("[RCB] 115200bps");
}

static void reply(const uint8_t pkt[18]) {
  bool high = (curBaud == BAUD_HIGH);
  if (high) digitalWrite(PIN_BUF_EN, LOW);  // 3ステートバッファを有効にする
  // ドライバ (uart_write_bytes) を通さず送信 FIFO (128B) に直接書く。ドライバ経由だと送信開始が遅れて間に合わない
  uart_ll_write_txfifo(UART_LL_GET_HW(1), pkt, 18);
  // 送信完了まで待ってからバッファを閉じる (1.25Mbps なら約 160μs)
  //   Serial1.flush() は while で空回りして CPU を握るので、割り込みで待つ IDF の関数を使う
  uart_wait_tx_done(UART_NUM_1, pdMS_TO_TICKS(5));
  if (high) digitalWrite(PIN_BUF_EN, HIGH);
}

static void commTask(void*) {
  detectBaud();
  uint32_t lastReq = millis();
  uint8_t buf[128], pkt[18];
  uint8_t matched = 0;
  // 応答は先に作っておき、要求が来たら送るだけにする (要求の直後に送り始めるため)
  buildPacket(stamp.peek(), pkt);

  for (;;) {
    size_t n = readChunk(buf, sizeof(buf));
    bool requested = false;
    for (size_t i = 0; i < n; i++) requested |= matchRequest(buf[i], matched);

    if (requested) {  // 1 チャンクに要求が複数あっても返すのは 1 回
      reply(pkt);
      lastReq = millis();
    } else if (millis() - lastReq > REDETECT_MS) {
      Serial.println("[RCB] no request, re-detecting");
      detectBaud();
      lastReq = millis();
      matched = 0;
    }
    // 受信のたび (無通信でも 2ms ごと) に作り直し、応答に載る入力の古さを最大 2ms 程度に抑える
    buildPacket(stamp.peek(), pkt);
  }
}

// ---------------------------------------------------------------------------
// ゲームパッド: PS(HOME) でライトバー色を切り替え
// ---------------------------------------------------------------------------
static const uint8_t COLORS[][3] = {
    {44, 255, 164}, {255, 50, 50}, {30, 30, 255}, {255, 255, 50}, {200, 200, 200},
};
static constexpr int COLOR_COUNT = sizeof(COLORS) / sizeof(COLORS[0]);
static int colorIndex = 0;

static void control(const StampInput& in) {
  if (in.wasPressed(GPButton::Home)) {
    colorIndex = (colorIndex + 1) % COLOR_COUNT;
    stamp.setLightbar(COLORS[colorIndex][0], COLORS[colorIndex][1], COLORS[colorIndex][2]);
  }
}

static void onConnect() { Serial.println("[PAD] connected"); }
static void onDisconnect() { Serial.println("[PAD] disconnected"); }  // 応答は自動で中立になる

void setup() {
  Serial.begin(115200);

  pinMode(22, INPUT);
  pinMode(PIN_BUF_EN, OUTPUT);
  digitalWrite(PIN_BUF_EN, HIGH);

  StampPadConfig cfg;
  cfg.gamepad.deviceName = "RCB4-5FH";
  cfg.lightbar[0] = COLORS[0][0];
  cfg.lightbar[1] = COLORS[0][1];
  cfg.lightbar[2] = COLORS[0][2];
  stamp.onConnect(onConnect);
  stamp.onDisconnect(onDisconnect);
  if (!stamp.begin(cfg)) {
    Serial.println("StampPad init failed");
    while (true) delay(1000);
  }
  stamp.startControlLoop(control, 20);  // 50Hz

  // 通信タスクは StampPad (優先度 2〜3) より上にして、要求が来たら必ず先に動くようにする
  xTaskCreatePinnedToCore(commTask, "rcbComm", 4096, nullptr, 5, nullptr, 1);
  Serial.println("ready");
}

void loop() { delay(1000); }
