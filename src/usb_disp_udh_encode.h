//
// ######################################################################
//
//    usb_disp_udh_encode - USB Display Host USB Encoder
//
//    NRZI/ビットスタッフィング符号化アルゴリズムは
//    Pico-PIO-USB (MIT License, Copyright (c) 2021 sekigon-gonnoc) 由来
//    オリジナル: https://github.com/sekigon-gonnoc/Pico-PIO-USB
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#ifndef USB_DISP_UDH_ENCODE_H_
#define USB_DISP_UDH_ENCODE_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// トークン EOP とデータ SOP の間に入れる J 駆動シンボル数 (usb_disp_udh_encode_txn 用)
// EOP シンボル自身が末尾に J を 1ビットタイム含むので、
// バス上のパケット間アイドルは (1 + この値) ビットタイムになる。
// USB 2.0 のホスト側パケット間隔は最小 2 / 最大 7.5 ビットタイム。
#ifndef USB_DISP_UDH_TXN_GAP_SYMS
  #define USB_DISP_UDH_TXN_GAP_SYMS 2   // → 3 ビットタイム (250ns @FS)
#endif

// ---- 出力バッファサイズの見積り ----
// 生 n バイト = 8n ビット。
// ビットスタッフは 6ビット毎に最大1個入るのでシンボル数は 8n + ceil(8n/6)。
// これにパケット末尾の EOP 1個を足す
#define USB_DISP_UDH_ENC_PKT_SYMS(n) \
    ((n) * 8u + ((n) * 8u + 5u) / 6u + 1u)
// 単独パケット (usb_disp_udh_encode_tx) の出力バイト数上限
#define USB_DISP_UDH_ENC_TX_BYTES(n) \
    ((USB_DISP_UDH_ENC_PKT_SYMS(n) + 1u + 3u) / 4u)
// トークン(4B) + ギャップ + データ(n バイト) を連結したときの上限
// (usb_disp_udh_encode_txn 用)
#define USB_DISP_UDH_ENC_TXN_BYTES(n)                     \
    ((USB_DISP_UDH_ENC_PKT_SYMS(4u) + USB_DISP_UDH_TXN_GAP_SYMS + \
      USB_DISP_UDH_ENC_PKT_SYMS(n) + 1u + 3u) / 4u)

// LUT 初期化 (一度だけ呼ぶ。1KB のテーブルを構築)
void usb_disp_udh_encode_init(void);

// data[0..len-1] をエンコードして out へ書く。
// 戻り値: 出力バイト数
// out には最低 (len*8+16)*7/6/4 + 4 バイト つまり len*2.4 + 8 バイト必要
uint8_t usb_disp_udh_encode_tx(const uint8_t *data, uint8_t len, uint8_t *out);

// トークンパケットとデータパケットを「パケット間ギャップ込みで」1本のシンボル列に連結してエンコードする
//  (OUT/SETUP トランザクション用)
//
// トークンとデータを別々に tx_start すると、両者の間隔が CPU の処理時間で決まってしまう
// USB 2.0 のトランザクション内パケット間隔は 2-7.5 ビットタイムなので、遅い側は完全に規格外
// 1本にまとめて 1回の DMA で流すことで、間隔を PIO のシンボルクロック律速
//  (= ハード律速の 3 ビットタイム) にする。
//
// 出力: [トークン ... EOP][J 駆動 × USB_DISP_UDH_TXN_GAP_SYMS]
//       [データ ... EOP][COMP][J パディング]
// EOP シンボルは 2箇所にあるので IRQ_TX_EOP は 2回立つ
// (1回目は usb_disp_udh_bus_tx_wait_eop、2回目は usb_disp_udh_bus_tx_wait_eop_and_arm で受ける)。
//
// 戻り値: 出力バイト数。
// out には (token_len + data_len) * 2.4 + 8 バイト程度が必要
uint16_t usb_disp_udh_encode_txn(const uint8_t *token, uint8_t token_len,
                                 const uint8_t *data, uint8_t data_len,
                                 uint8_t *out);

#ifdef __cplusplus
}
#endif

#endif // USB_DISP_UDH_ENCODE_H_

