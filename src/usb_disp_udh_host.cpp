//
// ######################################################################
//
//    usb_disp_udh_host - USB Display Host FS-Host
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

// Pico (RP2040/RP2350) 専用実装
#include "usb_disp.h"

#if USB_DISP_PORT_PICO

#include <string.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#include "usb_disp_udh_bus.h"
#include "usb_disp_udh_encode.h"
#include "usb_disp_udh_host.h"

// ---------------------------------------------------------------
// 設定
// ---------------------------------------------------------------

#define USB_DISP_UDH_SOF_PERIOD_US    1000
#define USB_DISP_UDH_RESERVE_US       100    // フレーム末尾に残す時間
#define USB_DISP_UDH_RING_SIZE        16384  // バルクリングバッファ (2の冪) / ポート
#define USB_DISP_UDH_CTRL_TIMEOUT_US  500000
#define USB_DISP_UDH_DEBOUNCE_MS      50
#define USB_DISP_UDH_RESET_MS         15
#define USB_DISP_UDH_CTRL_RETRY       64
#define USB_DISP_UDH_DEAD_STREAK      200    // バルク連続無応答でデバイス消失判定
#define USB_DISP_UDH_WRITE_STALL_US   1000000
#define USB_DISP_UDH_WARMUP_US        100000 // リセット後 SOF のみ送る時間
#define USB_DISP_UDH_PROBE_US         1000000

// トランザクション結果
enum { TXN_ACK = 0, TXN_NAK, TXN_STALL, TXN_DUP, TXN_ERROR };
// コントロール転送結果
enum { CTRL_OK = 0, CTRL_STALL, CTRL_ERROR, CTRL_DISCONNECTED };

// ホストステート
enum { HS_DETECT = 0, HS_RESET, HS_WARMUP, HS_RUN };

// ---------------------------------------------------------------
// ホストインスタンス
// ---------------------------------------------------------------

// バルク OUT の生パケット最大長 (SYNC + PID + データ64B + CRC16)
#define USB_DISP_UDH_BULK_RAW_MAX  68

typedef struct {
    uint16_t raw_len;
    uint16_t enc_len;
    bool valid;
    // トークン + パケット間ギャップ + データ を連結したエンコード済み列
    uint8_t enc[USB_DISP_UDH_ENC_TXN_BYTES(USB_DISP_UDH_BULK_RAW_MAX)];
} usb_disp_udh_pkt_t;

typedef struct {
    volatile uint8_t req;     // 0:空き 1:要求
    volatile uint8_t result;  // CTRL_*
    uint8_t addr;             // 転送先デバイスアドレス
    uint8_t mps0;             // 転送先 EP0 最大パケット長
    uint8_t setup[8];
    uint8_t *buf;
    volatile uint16_t actual;
} usb_disp_udh_ctrl_mbox_t;

struct usb_disp_udh_host {
    bool in_use;
    usb_disp_udh_bus_t bus;

    volatile uint8_t state;             // usb_disp_udh_state_t
    uint8_t hs;                         // HS_*
    volatile bool ls_detected;
    volatile bool bus_ok;
    volatile bool reenum_req;
    volatile bool diag_hold;            // ライン診断中 (detect のサンプリングを保留)
    usb_disp_udh_dbg_counters_t dbg;    // 状態遷移カウンタ (切り分け用)

    // 転送対象
    volatile uint8_t addr;
    volatile uint8_t mps0;
    volatile uint8_t bulk_ep;
    volatile uint16_t bulk_mps;
    volatile bool reset_toggle_req;

    // SOF
    uint32_t next_sof;
    volatile uint32_t frame;
    uint8_t sof_enc[16];

    // タイマ
    uint32_t deadline;          // reset/warmup 終了時刻
    uint32_t sof_grace_until;   // リセット直後の SE0 切断判定の猶予期限
    uint32_t last_sample_us;    // detect サンプル時刻
    uint32_t dp_cnt, ls_cnt, sample_total;  // detect 投票
    uint32_t probe_at_us;
    uint32_t probe_fails;

    // 制御メールボックス
    usb_disp_udh_ctrl_mbox_t ctrl;

    // バルクリング
    uint8_t ring[USB_DISP_UDH_RING_SIZE];
    volatile uint32_t ring_head;
    volatile uint32_t ring_tail;
    volatile bool flush_req;

    // 送信パケットバッファ
    // (交互に使う。符号化はトランザクションの外で行う
    // 送信中に挟むと EOP 検知が遅れて ACK を落とす)
    usb_disp_udh_pkt_t pkt[2];
    uint8_t bulk_toggle;
    uint8_t pipeline_cur;   // 0/1 (バッファ番号)
    uint32_t err_streak;

    // 統計
    volatile uint64_t stat_bytes;
    volatile uint32_t stat_naks;
    volatile uint32_t stat_errors;
    volatile uint32_t stat_dead_resets;
    uint8_t last_hs;
};

static usb_disp_udh_host_t s_hosts[USB_DISP_UDH_MAX_PORTS];
static uint8_t s_nhosts = 0;
static void (*s_idle_cb)(void);

// ---------------------------------------------------------------
// リング操作
// ---------------------------------------------------------------

static inline uint32_t ring_count(usb_disp_udh_host_t *h) {
    return h->ring_head - h->ring_tail;
}
static inline uint32_t ring_space(usb_disp_udh_host_t *h) {
    return USB_DISP_UDH_RING_SIZE - ring_count(h);
}
static inline void __not_in_flash_func(ring_peek)(usb_disp_udh_host_t *h, uint32_t off,
                                                  uint8_t *dst, uint32_t len) {
    // memcpy はフラッシュ常駐なので使わない
    // (XIP 競合で符号化が遅延し受信アーミングを取りこぼす)
    uint32_t pos = (h->ring_tail + off) & (USB_DISP_UDH_RING_SIZE - 1);
    for (uint32_t i = 0; i < len; i++) {
        dst[i] = h->ring[(pos + i) & (USB_DISP_UDH_RING_SIZE - 1)];
    }
}

// ---------------------------------------------------------------
// トランザクション (h->bus を叩く)
// ---------------------------------------------------------------

// トークンパケット (SYNC, PID, addr/EP + CRC5) を組み立てる
static inline void __not_in_flash_func(build_token)(uint8_t out[4], uint8_t pid,
                                                    uint8_t addr, uint8_t ep_num) {
    uint16_t dat = (uint16_t)(((ep_num & 0xF) << 7) | (addr & 0x7F));
    uint8_t crc = usb_disp_udh_crc5(dat);
    out[0] = USB_DISP_UDH_USB_SYNC;
    out[1] = pid;
    out[2] = (uint8_t)(dat & 0xFF);
    out[3] = (uint8_t)((crc << 3) | ((dat >> 8) & 0x1F));
}

static inline uint8_t map_handshake(uint8_t hs) {
    if (hs == USB_DISP_UDH_PID_ACK) return TXN_ACK;
    if (hs == USB_DISP_UDH_PID_NAK) return TXN_NAK;
    if (hs == USB_DISP_UDH_PID_STALL) return TXN_STALL;
    return TXN_ERROR;
}

// SETUP トランザクション
// トークンとデータを連結して 1回の DMA で送る
static uint8_t __not_in_flash_func(txn_setup)(usb_disp_udh_host_t *h, uint8_t addr,
                                              const uint8_t s[8]) {
    uint8_t tok[4];
    uint8_t raw[12];
    static uint8_t enc[USB_DISP_UDH_ENC_TXN_BYTES(12)];
    build_token(tok, USB_DISP_UDH_PID_SETUP, addr, 0);
    raw[0] = USB_DISP_UDH_USB_SYNC;
    raw[1] = USB_DISP_UDH_PID_DATA0;
    memcpy(raw + 2, s, 8);
    uint16_t crc = usb_disp_udh_crc16(s, 8);
    raw[10] = (uint8_t)crc;
    raw[11] = (uint8_t)(crc >> 8);
    uint16_t enc_len = usb_disp_udh_encode_txn(tok, 4, raw, 12, enc);

    usb_disp_udh_bus_tx_start(&h->bus, enc, enc_len);
    usb_disp_udh_bus_tx_wait_eop(&h->bus);        // トークン末尾 EOP
    // 受信側のリセットはトークン送出後に行う
    // エッジ検出器がパーク状態を外れていると自分のトークンを受信してしまい、
    // その残骸をハンドシェイクとして読んでしまう (PID 0xE1 の誤読)
    usb_disp_udh_bus_prepare_receive(&h->bus);
    usb_disp_udh_bus_tx_wait_eop_and_arm(&h->bus);
    uint8_t hs = usb_disp_udh_bus_wait_handshake(&h->bus);
    usb_disp_udh_bus_rx_disable(&h->bus);
    return map_handshake(hs);
}

// OUT トランザクション (コントロール転送用。バルクは txn_bulk)
static uint8_t __not_in_flash_func(txn_out)(usb_disp_udh_host_t *h, uint8_t addr,
                                            uint8_t ep_num, const uint8_t *data,
                                            uint16_t len, uint8_t toggle) {
    uint8_t tok[4];
    uint8_t raw[USB_DISP_UDH_BULK_RAW_MAX];
    static uint8_t enc[USB_DISP_UDH_ENC_TXN_BYTES(USB_DISP_UDH_BULK_RAW_MAX)];
    build_token(tok, USB_DISP_UDH_PID_OUT, addr, ep_num);
    raw[0] = USB_DISP_UDH_USB_SYNC;
    raw[1] = toggle ? USB_DISP_UDH_PID_DATA1 : USB_DISP_UDH_PID_DATA0;
    if (len > 0) memcpy(raw + 2, data, len);
    uint16_t crc = usb_disp_udh_crc16(data, len);
    raw[2 + len] = (uint8_t)crc;
    raw[3 + len] = (uint8_t)(crc >> 8);
    uint16_t enc_len = usb_disp_udh_encode_txn(tok, 4, raw, (uint8_t)(len + 4),
                                               enc);

    usb_disp_udh_bus_tx_start(&h->bus, enc, enc_len);
    usb_disp_udh_bus_tx_wait_eop(&h->bus);          // トークン末尾 EOP
    usb_disp_udh_bus_prepare_receive(&h->bus);      // 自己受信の残骸を捨てる
    usb_disp_udh_bus_tx_wait_eop_and_arm(&h->bus);
    uint8_t hs = usb_disp_udh_bus_wait_handshake(&h->bus);
    usb_disp_udh_bus_rx_disable(&h->bus);
    return map_handshake(hs);
}

//  バルク用: 
//  トークン連結済みパケット (prep_pkt が構築) を 1回の DMA で送る。
//  EOP は 2回立つ:
//  1回目 = トークン末尾、2回目 = データ末尾。
//
//  ※ここでは CPU 仕事を一切挟まないこと。
//    Cortex-M0+ (RP2040 144MHz) では符号化がデータ送信より長くかかり、
//    データ末尾 EOP の検知 → 受信アーム が間に合わない。
//    すると ACK を丸ごと取りこぼし (no_start)、さらにエッジ検出器がパーク位置を
//    外れたまま次のトランザクションへ入って自分の送信を受信してしまう(bad_sync/自 PID 誤読)
//    「ACK → no_start → 誤読」の 3周期が固定化し、Resend が 66% に張り付く。
//    RP2350 (Cortex-M33 240MHz) では符号化が間に合うため症状が出ない。
static uint8_t __not_in_flash_func(txn_bulk)(usb_disp_udh_host_t *h,
                                             usb_disp_udh_pkt_t *pkt) {
    usb_disp_udh_bus_tx_start(&h->bus, pkt->enc, pkt->enc_len);
    usb_disp_udh_bus_tx_wait_eop(&h->bus);                  // トークン末尾 EOP (-3.4µs)
    usb_disp_udh_bus_prepare_receive(&h->bus);              // 自己受信の残骸を捨てる
    usb_disp_udh_bus_tx_wait_eop_and_arm(&h->bus);          // データ末尾 EOP + アーム
    uint8_t hs = usb_disp_udh_bus_wait_handshake(&h->bus);
    usb_disp_udh_bus_rx_disable(&h->bus);
    h->last_hs = hs;
    return map_handshake(hs);
}

// IN トランザクション。トークン単独送信のままでよい
// (この後はデバイスが送信側になるので、本ライブラリ側のパケット間隔問題の対象外)
static uint8_t __not_in_flash_func(txn_in)(usb_disp_udh_host_t *h, uint8_t addr,
                                           uint8_t ep_num, uint8_t *dst,
                                           uint16_t *rxlen, uint8_t toggle) {
    uint8_t packet[4];
    build_token(packet, USB_DISP_UDH_PID_IN, addr, ep_num);
    static uint8_t enc[USB_DISP_UDH_ENC_TX_BYTES(4)];
    uint8_t enc_len = usb_disp_udh_encode_tx(packet, 4, enc);

    usb_disp_udh_bus_prepare_receive(&h->bus);
    usb_disp_udh_bus_tx_start(&h->bus, enc, enc_len);
    usb_disp_udh_bus_tx_wait_eop_and_arm(&h->bus);

    int16_t len = usb_disp_udh_bus_receive_packet_and_ack(&h->bus);
    uint8_t pid = h->bus.rx_buf[1];
    uint8_t res;
    if (len >= 0) {
        uint8_t expect = toggle ? USB_DISP_UDH_PID_DATA1 : USB_DISP_UDH_PID_DATA0;
        if (pid == expect) {
            if (dst && len > 0) memcpy(dst, &h->bus.rx_buf[2], (size_t)len);
            if (rxlen) *rxlen = (uint16_t)len;
            res = TXN_ACK;
        } else {
            res = TXN_DUP;
        }
    } else {
        res = map_handshake(pid);
        if (res == TXN_ACK) res = TXN_ERROR;
    }
    usb_disp_udh_bus_rx_disable(&h->bus);
    return res;
}

// ---------------------------------------------------------------
// SOF / スケジューラ協調
// ---------------------------------------------------------------

// 1ホストの SOF を必要なら送る + SE0 切断チェック
static void __not_in_flash_func(host_sof_tick)(usb_disp_udh_host_t *h) {
    if (!h->bus_ok) return;
    uint32_t now = time_us_32();
    if ((int32_t)(now - h->next_sof) < 0) return;

    if (usb_disp_udh_bus_line_state(&h->bus) == USB_DISP_UDH_LINE_SE0) {
        // リセット直後はチャープ K 明け (HS 対応デバイスの FS 復帰) を待つ
        // 猶予中は切断扱いにせず SOF も送らない
        if ((int32_t)(now - h->sof_grace_until) < 0) return;
        busy_wait_us_32(2);
        if (usb_disp_udh_bus_line_state(&h->bus) == USB_DISP_UDH_LINE_SE0) {
            h->dbg.sof_se0++;
            h->bus_ok = false;
            return;
        }
    }

    static uint8_t sof_raw[4] = {USB_DISP_UDH_USB_SYNC, USB_DISP_UDH_PID_SOF, 0x00, 0x10};
    uint16_t fn = (uint16_t)(h->frame & 0x7FF);
    sof_raw[2] = (uint8_t)fn;
    sof_raw[3] = (uint8_t)((usb_disp_udh_crc5(fn) << 3) | (fn >> 8));
    uint8_t enc[16];
    uint8_t n = usb_disp_udh_encode_tx(sof_raw, 4, enc);
    usb_disp_udh_bus_tx_start(&h->bus, enc, n);
    usb_disp_udh_bus_tx_wait_eop(&h->bus);
    h->frame = h->frame + 1;

    h->next_sof += USB_DISP_UDH_SOF_PERIOD_US;
    if ((int32_t)(time_us_32() - h->next_sof) > 0) {
        h->next_sof = time_us_32() + USB_DISP_UDH_SOF_PERIOD_US;
    }
}

// 全 RUN/WARMUP ポートの SOF を維持 (全ブロッキングループから呼ぶ)
static void __not_in_flash_func(tick_all_sof)(void) {
    for (uint8_t i = 0; i < s_nhosts; i++) {
        usb_disp_udh_host_t *h = &s_hosts[i];
        if (h->hs == HS_RUN || h->hs == HS_WARMUP) {
            host_sof_tick(h);
        }
    }
}

// h のトランザクション実行が可能になるまで待つ。切断で false。
static bool __not_in_flash_func(gate)(usb_disp_udh_host_t *h) {
    for (;;) {
        if (!h->bus_ok) return false;
        tick_all_sof();
        if (!h->bus_ok) return false;
        int32_t remain = (int32_t)(h->next_sof - time_us_32());
        if (remain > USB_DISP_UDH_RESERVE_US) return true;
    }
}

// ---------------------------------------------------------------
// コントロール転送 (core1 内で完結)
// ---------------------------------------------------------------

static uint8_t __not_in_flash_func(run_control)(usb_disp_udh_host_t *h) {
    usb_disp_udh_ctrl_mbox_t *c = &h->ctrl;
    bool dir_in = (c->setup[0] & 0x80) != 0;
    uint16_t wlen = (uint16_t)(c->setup[6] | (c->setup[7] << 8));
    uint32_t deadline = time_us_32() + USB_DISP_UDH_CTRL_TIMEOUT_US;
    c->actual = 0;

    uint8_t addr = c->addr;
    uint8_t mps0 = c->mps0 ? c->mps0 : 8;

    // SETUP
    uint8_t retry = 0;
    for (;;) {
        if (!gate(h)) return CTRL_DISCONNECTED;
        uint8_t r = txn_setup(h, addr, c->setup);
        if (r == TXN_ACK) break;
        if (++retry >= USB_DISP_UDH_CTRL_RETRY ||
            (int32_t)(time_us_32() - deadline) > 0)
            return CTRL_ERROR;
    }

    uint8_t toggle = 1;
    if (wlen > 0 && dir_in) {
        uint16_t got = 0;
        for (;;) {
            if (!gate(h)) return CTRL_DISCONNECTED;
            uint16_t rl = 0;
            uint8_t r = txn_in(h, addr, 0, c->buf ? c->buf + got : NULL, &rl,
                               toggle);
            if (r == TXN_ACK) {
                got += rl;
                toggle ^= 1;
                c->actual = got;
                if (rl < mps0 || got >= wlen) break;
            } else if (r == TXN_STALL) {
                return CTRL_STALL;
            } else if ((int32_t)(time_us_32() - deadline) > 0) {
                return CTRL_ERROR;
            }
        }
        // STATUS OUT (NAK は処理中なのでリトライ数に数えない)
        retry = 0;
        for (;;) {
            if (!gate(h)) return CTRL_DISCONNECTED;
            uint8_t r = txn_out(h, addr, 0, NULL, 0, 1);
            if (r == TXN_ACK) return CTRL_OK;
            if (r == TXN_STALL) return CTRL_STALL;
            if (r != TXN_NAK && ++retry >= USB_DISP_UDH_CTRL_RETRY) return CTRL_ERROR;
            if ((int32_t)(time_us_32() - deadline) > 0) return CTRL_ERROR;
        }
    } else {
        if (wlen > 0) {
            uint16_t sent = 0;
            while (sent < wlen) {
                if (!gate(h)) return CTRL_DISCONNECTED;
                uint16_t chunk = (uint16_t)(wlen - sent);
                if (chunk > mps0) chunk = mps0;
                uint8_t r = txn_out(h, addr, 0, c->buf + sent, chunk, toggle);
                if (r == TXN_ACK) {
                    sent += chunk;
                    toggle ^= 1;
                    c->actual = sent;
                } else if (r == TXN_STALL) {
                    return CTRL_STALL;
                } else if ((int32_t)(time_us_32() - deadline) > 0) {
                    return CTRL_ERROR;
                }
            }
        }
        // STATUS IN
        retry = 0;
        for (;;) {
            if (!gate(h)) return CTRL_DISCONNECTED;
            uint16_t rl = 0;
            uint8_t r = txn_in(h, addr, 0, NULL, &rl, 1);
            if (r == TXN_ACK || r == TXN_DUP) return CTRL_OK;
            if (r == TXN_STALL) return CTRL_STALL;
            if (r != TXN_NAK && ++retry >= USB_DISP_UDH_CTRL_RETRY) return CTRL_ERROR;
            if ((int32_t)(time_us_32() - deadline) > 0) return CTRL_ERROR;
        }
    }
}

// ---------------------------------------------------------------
// バルク OUT パンプ
// ---------------------------------------------------------------

// バルク OUT パケットを事前符号化する
// OUT トークンも同じバッファへ連結しておき、
// 送信時は 1回の DMA で「トークン + ギャップ + データ」を流す
// (トークン符号化もクリティカルパスから外れる)
static void __not_in_flash_func(prep_pkt)(usb_disp_udh_host_t *h, usb_disp_udh_pkt_t *p,
                                          uint32_t off, uint16_t chunk,
                                          uint8_t toggle) {
    uint8_t tok[4];
    uint8_t raw[USB_DISP_UDH_BULK_RAW_MAX];
    build_token(tok, USB_DISP_UDH_PID_OUT, h->addr, h->bulk_ep);
    raw[0] = USB_DISP_UDH_USB_SYNC;
    raw[1] = toggle ? USB_DISP_UDH_PID_DATA1 : USB_DISP_UDH_PID_DATA0;
    ring_peek(h, off, raw + 2, chunk);
    uint16_t crc = usb_disp_udh_crc16(raw + 2, chunk);
    raw[2 + chunk] = (uint8_t)crc;
    raw[3 + chunk] = (uint8_t)(crc >> 8);
    p->enc_len = usb_disp_udh_encode_txn(tok, 4, raw, (uint8_t)(chunk + 4),
                                         p->enc);
    p->raw_len = chunk;
    p->valid = true;
}

static inline uint16_t pick_chunk(usb_disp_udh_host_t *h, uint32_t off) {
    uint32_t avail = ring_count(h);
    if (avail <= off) return 0;
    avail -= off;
    if (avail >= h->bulk_mps) return h->bulk_mps;
    return h->flush_req ? (uint16_t)avail : 0;
}

// h のフレーム予算いっぱいまでバルクを送る (1 フレーム分)
static void __not_in_flash_func(pump_bulk)(usb_disp_udh_host_t *h) {
    if (h->bulk_ep == 0) return;

    if (h->reset_toggle_req) {
        h->reset_toggle_req = false;
        h->bulk_toggle = 0;
        h->pkt[0].valid = false;
        h->pkt[1].valid = false;
    }

    for (;;) {
        uint8_t cur = h->pipeline_cur;
        if (!h->pkt[cur].valid) {
            uint16_t chunk = pick_chunk(h, 0);
            if (chunk == 0) {
                if (h->flush_req && ring_count(h) == 0) h->flush_req = false;
                return;
            }
            prep_pkt(h, &h->pkt[cur], 0, chunk, h->bulk_toggle);
        }

        if (!gate(h)) return;

        uint8_t r = txn_bulk(h, &h->pkt[cur]);
        if (r == TXN_ACK) {
            uint32_t sent = h->pkt[cur].raw_len;
            h->ring_tail += sent;
            h->stat_bytes += sent;
            h->bulk_toggle ^= 1;
            h->pkt[cur].valid = false;
            h->pipeline_cur = (uint8_t)(1 - cur);
            h->err_streak = 0;
        } else if (r == TXN_NAK) {
            h->stat_naks = h->stat_naks + 1;
            h->err_streak = 0;
        } else if (r == TXN_STALL) {
            h->stat_errors = h->stat_errors + 1;
            h->err_streak = 0;
            h->ring_tail = h->ring_head;
            h->pkt[0].valid = false;
            h->pkt[1].valid = false;
            h->flush_req = false;
            return;
        } else {
            h->stat_errors = h->stat_errors + 1;
            if (++h->err_streak >= USB_DISP_UDH_DEAD_STREAK) {
                h->err_streak = 0;
                h->stat_dead_resets = h->stat_dead_resets + 1;
                h->bus_ok = false;
                return;
            }
        }
    }
}

// ---------------------------------------------------------------
// ホストステートマシン
// ---------------------------------------------------------------

static void clear_transfers(usb_disp_udh_host_t *h, uint8_t ctrl_result) {
    if (h->ctrl.req) {
        h->ctrl.result = ctrl_result;
        __sev();
        h->ctrl.req = 0;
    }
    h->ring_tail = h->ring_head;
    h->flush_req = false;
    h->pkt[0].valid = false;
    h->pkt[1].valid = false;
    h->bulk_toggle = 0;
    h->pipeline_cur = 0;
    h->err_streak = 0;
}

static void enter_detect(usb_disp_udh_host_t *h) {
    h->state = USB_DISP_UDH_DISCONNECTED;
    h->hs = HS_DETECT;
    h->addr = 0;
    h->mps0 = 8;
    h->bulk_ep = 0;
    h->bus_ok = false;
    clear_transfers(h, CTRL_DISCONNECTED);
    usb_disp_udh_bus_reconfigure(&h->bus);
    h->dp_cnt = h->ls_cnt = h->sample_total = 0;
    h->last_sample_us = time_us_32();
}

// 非ブロッキング。1回呼ぶごとに最大1サンプル進める。接続確定で HS_RESET へ。
static void __not_in_flash_func(advance_detect)(usb_disp_udh_host_t *h) {
    uint32_t now = time_us_32();
    if ((int32_t)(now - h->last_sample_us) < 1000) return;  // -1ms 間隔
    h->last_sample_us = now;

    if (h->diag_hold) return;  // ライン診断中はバスに触らない

    // 放電付き読み。E9 ラッチ対策で D+ High の割合だけで判定
    usb_disp_udh_line_t line = usb_disp_udh_bus_line_state_forced(&h->bus);
    h->sample_total++;
    if ((uint32_t)line & 1u) {
        h->dp_cnt++;
    } else if (line == USB_DISP_UDH_LINE_LS_IDLE) {
        h->ls_cnt++;
    }

    if (h->sample_total >= USB_DISP_UDH_DEBOUNCE_MS) {
        if (h->dp_cnt * 10 >= h->sample_total * 6) {
            // FS デバイス確定 → バスリセット開始
            h->dbg.detect_ok++;
            h->ls_detected = false;
            gpio_set_outover(h->bus.pin_dp, GPIO_OVERRIDE_LOW);
            gpio_set_outover(h->bus.pin_dm, GPIO_OVERRIDE_LOW);
            gpio_set_oeover(h->bus.pin_dp, GPIO_OVERRIDE_HIGH);
            gpio_set_oeover(h->bus.pin_dm, GPIO_OVERRIDE_HIGH);
            h->hs = HS_RESET;
            h->state = USB_DISP_UDH_CONNECTING;
            h->deadline = now + USB_DISP_UDH_RESET_MS * 1000;
            return;
        }
        h->ls_detected = (h->ls_cnt * 10 >= h->sample_total * 6);
        h->dp_cnt = h->ls_cnt = h->sample_total = 0;
    }
}

static void advance_reset(usb_disp_udh_host_t *h) {
    if ((int32_t)(time_us_32() - h->deadline) < 0) return;  // SE0 継続中
    // リセット終了 → ライン解放
    gpio_set_oeover(h->bus.pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(h->bus.pin_dm, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(h->bus.pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(h->bus.pin_dm, GPIO_OVERRIDE_NORMAL);
    busy_wait_us(100);
    usb_disp_udh_bus_reconfigure(&h->bus);

    h->bus_ok = true;
    h->next_sof = time_us_32() + 100;
    h->sof_grace_until = time_us_32() + 10000;  // SE0 切断判定の猶予 10ms
    h->hs = HS_WARMUP;
    h->deadline = time_us_32() + USB_DISP_UDH_WARMUP_US;
}

static void advance_warmup(usb_disp_udh_host_t *h) {
    if (!h->bus_ok) { h->dbg.warmup_fail++; enter_detect(h); return; }
    // SOF は tick_all_sof が送る。ウォームアップ完了で RUN へ
    if ((int32_t)(time_us_32() - h->deadline) >= 0) {
        h->dbg.run_enter++;
        h->hs = HS_RUN;
        h->state = USB_DISP_UDH_RUNNING;
        h->probe_at_us = time_us_32() + USB_DISP_UDH_PROBE_US;
        h->probe_fails = 0;
    }
}

static void advance_run(usb_disp_udh_host_t *h) {
    if (!h->bus_ok || h->reenum_req) {
        h->reenum_req = false;
        enter_detect(h);
        return;
    }

    if (h->ctrl.req == 1) {
        uint8_t result = run_control(h);
        h->ctrl.result = result;
        __sev();
        h->ctrl.req = 0;
        return;
    }

    pump_bulk(h);

    // 無通信時の死活プローブ (EP0 IN)。生きたデバイスは必ず応答する。
    uint32_t now = time_us_32();
    if (h->addr != 0 && (int32_t)(now - h->probe_at_us) > 0) {
        h->probe_at_us = now + USB_DISP_UDH_PROBE_US;
        if (gate(h)) {
            uint16_t rl = 0;
            uint8_t r = txn_in(h, h->addr, 0, NULL, &rl, 1);
            if (r == TXN_ERROR) {
                if (++h->probe_fails >= 3) {
                    h->probe_fails = 0;
                    h->stat_dead_resets = h->stat_dead_resets + 1;
                    h->bus_ok = false;
                }
            } else {
                h->probe_fails = 0;
            }
        }
    }
}

// ---------------------------------------------------------------
// core1 スケジューラ
// ---------------------------------------------------------------

// サービス方式: 
// core1 常駐 (従来) or 手動 task 呼び出し
// (arduino-pico の setup1/loop1 併用時など、core1 をライブラリが専有できない場合)
enum { SVC_NONE = 0, SVC_CORE1, SVC_MANUAL };
static volatile uint8_t s_svc_mode = SVC_NONE;
static bool s_task_inited = false;

static void host_task_init(void) {
    usb_disp_udh_bus_global_init();
    for (uint8_t i = 0; i < s_nhosts; i++) {
        enter_detect(&s_hosts[i]);
    }
    s_task_inited = true;
}

// スケジューラ1周回: 全ポートの SOF 維持 + ステートを1ステップ
static void host_task_once(void) {
    tick_all_sof();  // 全 RUN/WARMUP ポートの SOF を維持
    for (uint8_t i = 0; i < s_nhosts; i++) {
        usb_disp_udh_host_t *h = &s_hosts[i];
        switch (h->hs) {
        case HS_DETECT: advance_detect(h); break;
        case HS_RESET:  advance_reset(h);  break;
        case HS_WARMUP: advance_warmup(h); break;
        case HS_RUN:    advance_run(h);    break;
        }
    }
}

static void core1_main(void) {
    host_task_init();
    for (;;) host_task_once();
}

// ---------------------------------------------------------------
// core0 側 API
// ---------------------------------------------------------------

static void idle_pump(void) {
    if (s_idle_cb) s_idle_cb();
}

usb_disp_udh_host_t *usb_disp_udh_host_add(PIO pio, uint8_t pin_dp, uint8_t pin_dm) {
    if (s_nhosts >= USB_DISP_UDH_MAX_PORTS) return NULL;
    if (pin_dm != pin_dp + 1 && pin_dp != pin_dm + 1) return NULL;  // 要隣接
    usb_disp_udh_host_t *h = &s_hosts[s_nhosts];
    memset(h, 0, sizeof(*h));
    h->in_use = true;
    h->mps0 = 8;
    h->bulk_mps = 64;
    usb_disp_udh_bus_global_init();
    if (!usb_disp_udh_bus_init(&h->bus, pio, pin_dp, pin_dm)) {
        h->in_use = false;   // PIO プログラム配置失敗 (bus 側でログ済み)
        return NULL;
    }
    s_nhosts++;
    return h;
}

void usb_disp_udh_host_start(void) {
    if (s_svc_mode != SVC_NONE) return;
    s_svc_mode = SVC_CORE1;
    multicore_launch_core1(core1_main);
}

void usb_disp_udh_host_start_manual(void) {
    if (s_svc_mode != SVC_NONE) return;
    s_svc_mode = SVC_MANUAL;
}

void usb_disp_udh_host_task(void) {
    if (s_svc_mode != SVC_MANUAL) return;
    if (!s_task_inited) host_task_init();  // 初回: 呼んだコアで初期化
    host_task_once();
}

void usb_disp_udh_host_set_idle_cb(void (*cb)(void)) { s_idle_cb = cb; }

usb_disp_udh_state_t usb_disp_udh_host_state(usb_disp_udh_host_t *h) { return (usb_disp_udh_state_t)h->state; }
bool usb_disp_udh_host_low_speed(usb_disp_udh_host_t *h) { return h->ls_detected; }
void usb_disp_udh_host_request_reenum(usb_disp_udh_host_t *h) { h->reenum_req = true; }

void usb_disp_udh_host_line_diag(usb_disp_udh_host_t *h, usb_disp_udh_diag_t *out) {
    memset(out, 0, sizeof(*out));

    // 動作中でも安全に測れるよう、まず切断状態へ落として検出を保留する
    h->diag_hold = true;
    h->reenum_req = true;
    h->bus_ok = false;      // RUN 中なら enter_detect へ抜けさせる
    busy_wait_us(5000);     // core1 が detect ループへ入るのを待つ

    // 生ラインステート統計 (放電なし、E9 ドレイン読みのみ)。100µs 毎に 1000 回
    for (uint16_t i = 0; i < 1000; i++) {
        usb_disp_udh_line_t line = usb_disp_udh_bus_line_state(&h->bus);
        out->total++;
        if (line == USB_DISP_UDH_LINE_FS_IDLE) out->dp_hi++;
        else if (line == USB_DISP_UDH_LINE_LS_IDLE) out->dm_hi++;
        else if (line == USB_DISP_UDH_LINE_SE1) out->se1++;
        busy_wait_us(100);
    }

    // 放電付き立ち上がりプロファイル (-250ns/step で最大64サンプル=16µs 窓)
    for (uint8_t i = 0; i < USB_DISP_UDH_DIAG_RISE_RUNS; i++) {
        usb_disp_udh_line_t line;
        out->rise_idx[i] = usb_disp_udh_bus_line_rise_profile(&h->bus, &line,
                                                              64);
        out->rise_line[i] = (uint8_t)line;
        busy_wait_us(2000);
    }

    h->diag_hold = false;  // 通常の接続検出へ復帰
}

void usb_disp_udh_host_dbg_counters(usb_disp_udh_host_t *h, usb_disp_udh_dbg_counters_t *out) {
    *out = h->dbg;
}

void usb_disp_udh_host_set_device(usb_disp_udh_host_t *h, uint8_t addr, uint8_t ep0_mps) {
    h->addr = addr;
    h->mps0 = ep0_mps;
}

void usb_disp_udh_host_set_bulk_out(usb_disp_udh_host_t *h, uint8_t ep_addr, uint16_t mps) {
    // Full-Speed バルクの最大パケット長は 64
    // ディスクリプタが大きい値を申告してもパケット組み立てバッファ (raw[68]) を超えさせない
    if (mps == 0 || mps > USB_DISP_UDH_BULK_RAW_MAX - 4) {
        mps = USB_DISP_UDH_BULK_RAW_MAX - 4;
    }
    h->bulk_ep = (uint8_t)(ep_addr & 0x0F);
    h->bulk_mps = mps;
    h->reset_toggle_req = true;
}

bool usb_disp_udh_host_control_addr(usb_disp_udh_host_t *h, uint8_t addr, uint8_t ep0_mps,
                           const uint8_t setup[8], void *data,
                           uint16_t *actual_len) {
    if (h->state != USB_DISP_UDH_RUNNING) return false;
    while (h->ctrl.req != 0) idle_pump();

    h->ctrl.addr = addr;
    h->ctrl.mps0 = ep0_mps;
    memcpy(h->ctrl.setup, setup, 8);
    h->ctrl.buf = (uint8_t *)data;
    h->ctrl.actual = 0;
    h->ctrl.result = CTRL_ERROR;
    __dmb();
    h->ctrl.req = 1;

    uint32_t deadline = time_us_32() + 2 * USB_DISP_UDH_CTRL_TIMEOUT_US;
    while (h->ctrl.req != 0) {
        if ((int32_t)(time_us_32() - deadline) > 0) return false;
        idle_pump();
    }
    __dmb();
    if (actual_len) *actual_len = h->ctrl.actual;
    return h->ctrl.result == CTRL_OK;
}

bool usb_disp_udh_host_control(usb_disp_udh_host_t *h, const uint8_t setup[8], void *data,
                      uint16_t *actual_len) {
    return usb_disp_udh_host_control_addr(h, h->addr, h->mps0, setup, data,
                                 actual_len);
}

uint32_t usb_disp_udh_host_bulk_write(usb_disp_udh_host_t *h, const void *data, uint32_t len) {
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    uint32_t last_progress = time_us_32();

    while (written < len) {
        if (h->state != USB_DISP_UDH_RUNNING) return written;
        uint32_t space = ring_space(h);
        if (space == 0) {
            if ((int32_t)(time_us_32() - last_progress) >
                (int32_t)USB_DISP_UDH_WRITE_STALL_US)
                return written;
            idle_pump();
            continue;
        }
        last_progress = time_us_32();

        uint32_t chunk = len - written;
        if (chunk > space) chunk = space;
        uint32_t pos = h->ring_head & (USB_DISP_UDH_RING_SIZE - 1);
        uint32_t first = USB_DISP_UDH_RING_SIZE - pos;
        if (first > chunk) first = chunk;
        memcpy(&h->ring[pos], src + written, first);
        if (chunk > first) memcpy(&h->ring[0], src + written + first, chunk - first);
        __dmb();
        h->ring_head += chunk;
        written += chunk;
    }
    return written;
}

bool usb_disp_udh_host_bulk_flush(usb_disp_udh_host_t *h, uint32_t timeout_ms) {
    if (h->state != USB_DISP_UDH_RUNNING) return false;
    h->flush_req = true;
    uint32_t deadline = time_us_32() + timeout_ms * 1000;
    while (h->flush_req || ring_count(h) > 0) {
        if (h->state != USB_DISP_UDH_RUNNING) return false;
        if ((int32_t)(time_us_32() - deadline) > 0) return false;
        idle_pump();
    }
    return true;
}

uint32_t usb_disp_udh_host_bulk_pending(usb_disp_udh_host_t *h) { return ring_count(h); }
uint64_t usb_disp_udh_host_stat_bytes(usb_disp_udh_host_t *h) { return h->stat_bytes; }
uint32_t usb_disp_udh_host_stat_naks(usb_disp_udh_host_t *h) { return h->stat_naks; }
uint32_t usb_disp_udh_host_stat_errors(usb_disp_udh_host_t *h) { return h->stat_errors; }
uint32_t usb_disp_udh_host_stat_dead_resets(usb_disp_udh_host_t *h) { return h->stat_dead_resets; }
uint32_t usb_disp_udh_host_frame(usb_disp_udh_host_t *h) { return h->frame; }

#endif

