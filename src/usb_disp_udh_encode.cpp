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

// Pico (RP2040/RP2350) 専用実装
#include "usb_disp.h"

#if USB_DISP_PORT_PICO

#include "pico.h"

#include "usb_disp_udh_encode.h"

// 2bit シンボル = usb_tx.pio の命令アドレス
//   0 (PC=0) irq nowait 0 side 0   [7] → SE0 2ビットタイム、続く PC=1 で J
//                                       1ビットタイム = USB の EOP そのもの
//   1 (PC=1) out pc,2 side J       [3] → J を 1ビットタイム駆動
//   2 (PC=2) set pindirs,0         [3] → 出力解放 (パケット完了)
//   3 (PC=3) out pc,2 side K       [3] → K を 1ビットタイム駆動
// (dpdm/dmdp で side の値は異なるが、電気的な J/K の割当は同じ)
#define USB_DISP_UDH_SYM_EOP  0u
#define USB_DISP_UDH_SYM_J    1u
#define USB_DISP_UDH_SYM_COMP 2u
#define USB_DISP_UDH_SYM_K    3u

// LUT エントリ: 1ニブル(4bit, LSB first)を符号化した結果
typedef struct {
    uint16_t sym;    // シンボル列 (最初のシンボルが上位側)
    uint8_t nbits;   // sym の有効ビット数 (2 × シンボル数, 8..12)
    uint8_t next;    // 次状態: bit0=NRZI状態, bit3:1=スタッフ残数
} usb_disp_udh_enc_lut_t;

// インデックス: ((stuff-1)*2 + state)*16 + nibble  (stuff: 1..6)
static usb_disp_udh_enc_lut_t s_lut[6 * 2 * 16];

static inline uint16_t lut_index(uint32_t state, uint32_t stuff,
                                 uint32_t nib) {
    return (uint16_t)(((stuff - 1u) * 2u + state) * 16u + nib);
}

void usb_disp_udh_encode_init(void) {
    for (uint32_t stuff0 = 1; stuff0 <= 6; stuff0++) {
        for (uint32_t state0 = 0; state0 <= 1; state0++) {
            for (uint32_t nib = 0; nib <= 0xF; nib++) {
                uint32_t state = state0;
                uint32_t stuff = stuff0;
                uint16_t sym = 0;
                uint8_t nbits = 0;

                // state 1 = ラインが J / state 0 = ラインが K
                for (uint8_t b = 0; b < 4; b++) {
                    if (nib & (1u << b)) {
                        // データ 1: レベル維持
                        sym = (uint16_t)((sym << 2) |
                                         (state ? USB_DISP_UDH_SYM_J
                                                : USB_DISP_UDH_SYM_K));
                        nbits += 2;
                        if (--stuff == 0) {
                            // スタッフビット挿入 (レベル反転)
                            sym = (uint16_t)((sym << 2) |
                                             (state ? USB_DISP_UDH_SYM_K
                                                    : USB_DISP_UDH_SYM_J));
                            nbits += 2;
                            state ^= 1;
                            stuff = 6;
                        }
                    } else {
                        // データ 0: レベル反転
                        sym = (uint16_t)((sym << 2) |
                                         (state ? USB_DISP_UDH_SYM_K
                                                : USB_DISP_UDH_SYM_J));
                        nbits += 2;
                        state ^= 1;
                        stuff = 6;
                    }
                }

                usb_disp_udh_enc_lut_t *e = &s_lut[lut_index(state0, stuff0, nib)];
                e->sym = sym;
                e->nbits = nbits;
                e->next = (uint8_t)(state | (stuff << 1));
            }
        }
    }
}

// シンボル列の組み立て状態
// 複数パケットを 1本のバッファへ連結できるよう
// アキュムレータを呼び出し間で持ち回る (accbits は常に 0/2/4/6)
typedef struct {
    uint32_t acc;      // シンボルビット列アキュムレータ (下位側に追記)
    uint32_t accbits;
    uint8_t *w;
} usb_disp_udh_enc_ctx_t;

// シンボルを 1個追記
static inline void __not_in_flash_func(enc_sym)(usb_disp_udh_enc_ctx_t *c,
                                                uint32_t sym) {
    c->acc = (c->acc << 2) | sym;
    c->accbits += 2;
    if (c->accbits >= 8) {
        *c->w++ = (uint8_t)(c->acc >> (c->accbits - 8));
        c->accbits -= 8;
    }
}

// 1パケット分 (SYNC..CRC) を NRZI + ビットスタッフして追記し、末尾にEOP シンボルを置く。
// NRZI 状態はパケット毎にリセットされる(パケット開始時のバスは必ず J = state 1)
static inline void __not_in_flash_func(enc_packet)(usb_disp_udh_enc_ctx_t *c,
                                                   const uint8_t *data,
                                                   uint8_t len) {
    uint32_t state = 1;    // NRZI 初期状態 (バスは J)
    uint32_t stuff = 6;    // スタッフカウンタ初期値

    for (uint32_t i = 0; i < len; i++) {
        uint32_t byte = data[i];

        // 下位ニブル → 上位ニブル (ビットは LSB first)
        for (uint8_t half = 0; half < 2; half++) {
            uint32_t nib = (byte >> (half * 4)) & 0xF;
            const usb_disp_udh_enc_lut_t *e = &s_lut[lut_index(state, stuff, nib)];
            c->acc = (c->acc << e->nbits) | e->sym;
            c->accbits += e->nbits;
            state = e->next & 1u;
            stuff = e->next >> 1;

            while (c->accbits >= 8) {
                *c->w++ = (uint8_t)(c->acc >> (c->accbits - 8));
                c->accbits -= 8;
            }
        }
    }

    enc_sym(c, USB_DISP_UDH_SYM_EOP);
}

// 終端: 出力解放 + バイト境界まで J パディング
static inline void __not_in_flash_func(enc_finish)(usb_disp_udh_enc_ctx_t *c) {
    enc_sym(c, USB_DISP_UDH_SYM_COMP);
    while (c->accbits) {
        enc_sym(c, USB_DISP_UDH_SYM_J);
    }
}

uint8_t __not_in_flash_func(usb_disp_udh_encode_tx)(const uint8_t *data, uint8_t len,
                                           uint8_t *out) {
    usb_disp_udh_enc_ctx_t c = {0, 0, out};
    enc_packet(&c, data, len);
    enc_finish(&c);
    return (uint8_t)(c.w - out);
}

uint16_t __not_in_flash_func(usb_disp_udh_encode_txn)(const uint8_t *token,
                                                      uint8_t token_len,
                                                      const uint8_t *data,
                                                      uint8_t data_len,
                                                      uint8_t *out) {
    usb_disp_udh_enc_ctx_t c = {0, 0, out};
    enc_packet(&c, token, token_len);
    // パケット間ギャップ:
    // バスを J で駆動したまま保持する。出力解放 (COMP) を挟まないのがポイント
    // シンボル列から attach (set pindirs,3) へは戻れないため、一度離すと次パケットを駆動できない
    for (uint8_t i = 0; i < USB_DISP_UDH_TXN_GAP_SYMS; i++) {
        enc_sym(&c, USB_DISP_UDH_SYM_J);
    }
    enc_packet(&c, data, data_len);
    enc_finish(&c);
    return (uint16_t)(c.w - out);
}

#endif


