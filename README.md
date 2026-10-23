# RCB4toESPreceiver

M5Stamp Pico を近藤科学の受信機 **KRR-5FH** に擬態させて、PS5 / PS4 / Switch / Xbox のコントローラーで RCB-4 のロボットを操縦するためのプログラムです。
RCB-4 からは KRR-5FH がつながっているように見えるので、RCB-4 側（HeartToHeart4 のモーションやシナリオ）は変更せずに使えます。

コントローラーとの通信には [BTGamepad / StampPad](https://github.com/rara-crash/BTGamepad) を使っています。

## できること

- RCB-4 からの問い合わせ（約 10ms ごと）に、ボタンとアナログスティックの状態を返す
- 1.25Mbps / 115200bps を自動判定する（RCB-4 を後から起動したり、リセットしたりしても復帰する）
- コントローラーが切れた・通信が途絶えたときは、全ボタン OFF・スティック中立を返す
- PS ボタン（HOME）でライトバーの色を切り替える

## 配線（M5Stamp Pico）

| ピン | 役割 |
|---|---|
| G25 | RCB-4 → Stamp（要求の受信） 8E1・非反転 |
| G26 | Stamp → RCB-4（応答の送信） 8E1・反転 |
| G32 | 3ステートバッファのイネーブル（1.25Mbps で送信中だけ LOW） |
| G22 | 未使用（INPUT） |

RCB-4 とは ICS と同じ半二重の 1 本線を共有します。

## ボタンの割り当て

| コントローラー | KRR-5FH |
|---|---|
| □ / L1 / L2 / R1 / R2 | □ / L1 / L2 / R1 / R2 |
| 十字キー | 十字キー |
| △ / × / ○ | △ / × / ○ |
| Options（+ / Menu） | 上下同時押し（L3） |
| Share（- / View） | 左右同時押し（R3） |
| 左スティック Y / X、右スティック Y / X | アナログ 1〜4（中立 64、±9 未満は中立扱い） |

ボタンは位置で対応します（Switch / Xbox では、□ の位置のボタンが □ になります）。

## 必要なもの

- [PlatformIO](https://platformio.org/)（`espressif32@6.5.0` = Arduino-ESP32 2.x に固定。BTGamepad が 3.x に未対応のため）
- BTGamepad / StampPad ライブラリ

```sh
git clone https://github.com/rara-crash/BTGamepad.git C:/pio-libs
```

`platformio.ini` の `lib_extra_dirs` を clone したフォルダに合わせてください（既定は `C:/Users/kotar/pio-libs`）。
**パスに日本語などが入っているとビルドに失敗します。** その場合は英数字だけのパスのジャンクションを作ってください。

```powershell
New-Item -ItemType Junction -Path C:\pio-libs -Target "<clone したフォルダ>"
```

## 使い方

1. 書き込んで RCB-4 につなぎ、電源を入れる
2. 本体ボタン（G39）を短く押す（LED が青点滅）
3. コントローラーをペアリングモードにする
   - PS5: Create + PS を長押し / PS4: Share + PS を長押し / Switch Pro: シンクロボタン / Xbox: ペアリングボタン
4. LED が緑になれば接続完了。2回目からは PS ボタンを押すだけでつながる

本体ボタンの長押し: 3 秒 = 今の機体に固定 / 固定解除（固定中は LED が青緑）、8 秒 = 登録をすべて消去。
同時につなげるコントローラーは 1 台です。ほかのコントローラーがつながらないときは、固定されていないか確認してください。

## 実装の注意（応答のタイミング）

RCB-4 が受信機の応答を待つ時間はとても短く、**要求を受けてから十数 µs 以内に送り始めないと応答が無視されます**。
無視されてもエラーは出ず、サーボが脱力したまま・モーションが再生されないだけなので、気づきにくい点に注意してください。
このプログラムでは次のようにして間に合わせています。

- 受信 FIFO のしきい値を要求の長さ（4 バイト）にして、要求が揃った瞬間に処理する（既定の 120 バイトだと約 90µs 遅れる。1 にすると割り込みが多すぎてコア 1 が詰まる）
- 応答パケットは先に作っておく
- 送信はドライバ（`uart_write_bytes`）を通さず、送信 FIFO に直接書く
- 送信完了は `Serial1.flush()`（空回りで待つ）ではなく `uart_wait_tx_done()`（割り込みで待つ）で待つ

## ライセンス

MIT License（[LICENSE](LICENSE)）
