//
// ######################################################################
//
//    usb_disp_udh_bus - USB Display Host Bus Layer
//
//    Pico-PIO-USB (MIT License, Copyright (c) 2021 sekigon-gonnoc)
//    pio_usb.c / usb_crc.c から移植・簡略化
//    オリジナル: https://github.com/sekigon-gonnoc/Pico-PIO-USB
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#ifndef USB_DISP_UDH_BUS_H_
#define USB_DISP_UDH_BUS_H_

#include <stdint.h>
#include <stdbool.h>

#include "hardware/pio.h"

#ifdef __cplusplus
extern "C" {
#endif

// USB パケット定数
enum {
    USB_DISP_UDH_USB_SYNC   = 0x80,
    USB_DISP_UDH_PID_OUT    = 0xE1,
    USB_DISP_UDH_PID_IN     = 0x69,
    USB_DISP_UDH_PID_SOF    = 0xA5,
    USB_DISP_UDH_PID_SETUP  = 0x2D,
    USB_DISP_UDH_PID_DATA0  = 0xC3,
    USB_DISP_UDH_PID_DATA1  = 0x4B,
    USB_DISP_UDH_PID_ACK    = 0xD2,
    USB_DISP_UDH_PID_NAK    = 0x5A,
    USB_DISP_UDH_PID_STALL  = 0x1E,
};

// D+/D- ラインステート
typedef enum {
    USB_DISP_UDH_LINE_SE0     = 0,
    USB_DISP_UDH_LINE_FS_IDLE = 1,   // D+ プルアップ (Full-Speed デバイス)
    USB_DISP_UDH_LINE_LS_IDLE = 2,   // D- プルアップ (Low-Speed デバイス)
    USB_DISP_UDH_LINE_SE1     = 3,
} usb_disp_udh_line_t;

typedef struct {
    PIO pio;           // このポートが専有する PIO ブロック
    uint8_t sm_tx;     // TX (SM 番号 0..3)
    uint8_t sm_rx;     // NRZI デコーダ
    uint8_t sm_eop;    // エッジ/EOP 検出
    uint8_t tx_ch;     // TX DMA チャネル (0..15)

    uint8_t offset_tx;   // PIO プログラムオフセット (0..31)
    uint8_t offset_rx;
    uint8_t offset_eop;

    uint16_t tx_start_instr;   // PIO 命令は 16bit
    uint16_t tx_reset_instr;
    uint16_t rx_reset_instr;
    uint16_t rx_reset_instr2;

    uint8_t pin_dp;
    uint8_t pin_dm;

    uint8_t rx_buf[128];   // 受信パケットバッファ (SYNC, PID, data..., CRC)
} usb_disp_udh_bus_t;

// 全ポート共通の一度きり初期化 (CRC テーブル / エンコーダ LUT / DMA 優先度)
void usb_disp_udh_bus_global_init(void);

// ポート初期化
// pio に TX/RX プログラムをロードし SM3個 + DMA1ch を確保する。
// pio は他用途と共有していない専用ブロックを渡すこと。
// pin_dp/pin_dm は隣接 GPIO であること。
// 戻り値: PIO プログラムを配置できなければ false。
bool usb_disp_udh_bus_init(usb_disp_udh_bus_t *b, PIO pio, uint8_t pin_dp, uint8_t pin_dm);

// 再接続時の TX ステートマシンリセット
void usb_disp_udh_bus_reconfigure(usb_disp_udh_bus_t *b);

// D+/D- ラインステート取得 (RP2350-E9 エラッタ対策込み)
usb_disp_udh_line_t usb_disp_udh_bus_line_state(usb_disp_udh_bus_t *b);

// 放電付きラインステート取得 (E9 の入力ラッチ固着でも真値が読める)。
// バスに -5µs の SE0 グリッチを出すため接続確立前の検出専用。
usb_disp_udh_line_t usb_disp_udh_bus_line_state_forced(usb_disp_udh_bus_t *b);

// 放電後の立ち上がりプロファイル
// 最初に High が読めたサンプル番号 (-250ns/step) を返す。窓内に無ければ -1。
int8_t usb_disp_udh_bus_line_rise_profile(usb_disp_udh_bus_t *b, usb_disp_udh_line_t *line_out,
                              uint8_t max_samples);

// パッドの叩き直し (接続検出が長引く時の E9 ラッチ/残留電荷対策)
void usb_disp_udh_bus_pad_kick(usb_disp_udh_bus_t *b);

// ---- TX ----
void usb_disp_udh_bus_tx_start(usb_disp_udh_bus_t *b, uint8_t *encoded, uint16_t len);
void usb_disp_udh_bus_tx_wait_eop(usb_disp_udh_bus_t *b);
// EOP 待ち + 受信アーミングを単一レジスタ書き込みで遅延最小。
// トランザクション最後の送信パケットにはこちらを使う。
// (usb_disp_udh_encode_txn の連結バッファでは EOP がトークン末尾と
//  データ末尾で 1回ずつ立つ。1回目は tx_wait_eop、2回目にこれを使う)
void usb_disp_udh_bus_tx_wait_eop_and_arm(usb_disp_udh_bus_t *b);

// ---- RX ----
void usb_disp_udh_bus_prepare_receive(usb_disp_udh_bus_t *b);
void usb_disp_udh_bus_start_receive(usb_disp_udh_bus_t *b);
uint8_t usb_disp_udh_bus_wait_handshake(usb_disp_udh_bus_t *b);
// 受信データ長 (CRC 除く 0..124) を返す。エラー/タイムアウトは -1
int16_t usb_disp_udh_bus_receive_packet_and_ack(usb_disp_udh_bus_t *b);
void usb_disp_udh_bus_rx_disable(usb_disp_udh_bus_t *b);

// ---- ハンドシェイク受信の失敗内訳カウンタ (切り分け用、既定 OFF) ----
// -DUSB_DISP_UDH_HS_DIAG=1 でビルドすると wait_handshake が結果を分類して数える。
// Resend (TXN_ERROR) が増えたときに「デバイスが応答していない」のか
// 「自分の送信を受信してしまっている」のかを切り分けるためのもの。
//   no_start が多い       → ACK を取りこぼしている (受信アームが遅い。トランザクション中に CPU 仕事を挟んでいないか)
//   pid_own/sof/data が多い → エッジ検出器がパーク位置を外れ、自分の送信を受信している (no_start の二次被害)
//
#ifndef USB_DISP_UDH_HS_DIAG
  #define USB_DISP_UDH_HS_DIAG 0
#endif
#if USB_DISP_UDH_HS_DIAG
typedef struct {
    uint32_t total;      // wait_handshake 呼び出し回数
    uint32_t no_start;   // 3µs 以内に SOP (立ち下がり) を検出できず
    uint32_t short_pkt;  // SOP は来たが 2バイト揃わず
    uint32_t bad_sync;   // 先頭が SYNC(0x80) でない
    uint32_t pid_ack, pid_nak;
    uint32_t pid_own;    // 自分の OUT トークンを読んだ (自己受信)
    uint32_t pid_sof;    // 自分の SOF を読んだ (自己受信)
    uint32_t pid_data;   // 自分のデータパケットを読んだ (自己受信)
    uint32_t pid_other;
    uint8_t  last_pid;
} usb_disp_udh_hs_diag_t;
extern usb_disp_udh_hs_diag_t g_udh_hs_diag;
#endif

// ---- CRC (ステートレス・共有) ----
uint8_t usb_disp_udh_crc5(uint16_t data11);
uint16_t usb_disp_udh_crc16(const uint8_t *data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif // USB_DISP_UDH_BUS_H_

