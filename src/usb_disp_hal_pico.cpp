//
// ######################################################################
//
//    usb_disp_hal_pico - RP2040/RP2350 バックエンド
//
//    usb_disp_udh_host (PIO USB Full-Speed ホスト) を usb_disp_hal
//    インターフェースで包み、USB エニュメレーション (アドレス設定・
//    ディスクリプタ取得・バルク OUT EP 探索・コンフィグ設定) を行う。
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include "usb_disp_hal.h"

#if USB_DISP_PORT_PICO

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"   // clock_get_hz / set_sys_clock_khz

#include "usb_disp_udh_host.h"

// エニュメレーション再試行
#define USB_DISP_ENUM_MAX_ATTEMPTS 5
#define USB_DISP_ENUM_RETRY_DELAY_MS 500
// バルク OUT が無いデバイスの周期再試行
#define USB_DISP_ENUM_NO_EP_RETRY_MS 30000

//  ハブ対応 (多段可):
//    ルート直結デバイス = アドレス 1、
//    下流デバイス (ハブ含む) はアドレス 2 から順に割り当て。
//    ポート状態はハブの割り込み EP を使わず GET_STATUS ポーリングで見る。
//    多段はハブテーブルで管理する
//    (アクティブ USB ケーブル = 内蔵1ポートハブ + 外付けハブ、のような構成に対応)

#define USB_DISP_ROOT_ADDR           1   // ルート直結デバイス (ハブ含む)
#define USB_DISP_HUB_DEV_ADDR_BASE   2
#define USB_DISP_HUB_POLL_MS         500
#define USB_DISP_HUB_MAX_PORTS       7   // 追跡する下流ポート数の上限 / ハブ
#define USB_DISP_HUB_MAX_HUBS        4   // 同時に扱うハブ総数
#define USB_DISP_HUB_MAX_DEPTH       4   // ハブの段数上限 (USB 仕様は5段)
// ポートフィーチャ (USB 2.0 spec 11.24.2)
#define USB_DISP_HUB_FEAT_PORT_RESET        4
#define USB_DISP_HUB_FEAT_PORT_POWER        8
#define USB_DISP_HUB_FEAT_C_PORT_CONNECTION 16
#define USB_DISP_HUB_FEAT_C_PORT_RESET      20
// wPortStatus / wPortChange ビット
#define USB_DISP_HUB_PS_CONNECTION   0x0001
#define USB_DISP_HUB_PS_ENABLE       0x0002
#define USB_DISP_HUB_PS_LOW_SPEED    0x0200
#define USB_DISP_HUB_PC_CONNECTION   0x0001
#define USB_DISP_HUB_PC_RESET        0x0010

typedef enum {
    HW_WAIT_DEVICE = 0,  // バス接続待ち
    HW_RETRY_WAIT,       // エニュメ失敗、待って再試行
    HW_ATTACHED,         // エニュメ完了 (ctrl/bulk 使用可)
    HW_NO_EP,            // デバイスはいるがバルク OUT が無い (周期再試行)
    HW_HUB_WAIT,         // ハブ構成済み、下流の DL 接続待ち (ポーリング)
} hal_state_t;

// ハブテーブルエントリ
// (C++ ではネスト定義だとスコープが変わるためファイルスコープで定義する)
struct hub_ent {
    uint8_t addr;      // 0 = 未使用
    uint8_t mps0;      // ハブの EP0 MPS
    uint8_t nports;
    uint8_t depth;     // 1 = ルート直下
    uint8_t port_done; // bitN=1: ポートNの接続済みデバイスは処理済み
    uint8_t fail;      // GET_STATUS 連続失敗 (ハブ消失判定)
};

struct usb_disp_hal {
    bool in_use;
    usb_disp_udh_host_t *host;
    hal_state_t state;
    uint16_t vid, pid;
    uint8_t bulk_ep;
    uint16_t bulk_mps;
    uint8_t attempts;    // エニュメ再試行回数 (0..USB_DISP_ENUM_MAX_ATTEMPTS)
    uint32_t retry_at_ms;
    bool retry_pending;
    uint32_t no_ep_since_ms;
    uint8_t vdesc[64];   // config 内 0x5F ベンダーディスクリプタ
    uint8_t vdesc_len;

    // ハブ対応 (構成済みハブのテーブル。多段はここに積まれる)
    struct hub_ent hubs[USB_DISP_HUB_MAX_HUBS];
    uint8_t nhubs;
    uint8_t next_addr;     // 下流デバイスに次に割り当てるアドレス
    uint32_t hub_poll_at_ms;
};

static struct usb_disp_hal s_hal[USB_DISP_MAX];
static uint8_t s_nhal = 0;

// エニュメレーション用の共有スクラッチ (core0 で逐次実行のため共有可)
static uint8_t s_desc_buf[512];

uint32_t usb_disp_hal_ms(void) {
    return to_ms_since_boot(get_absolute_time());
}

// 任意アドレスへのコントロール転送 (ハブと下流デバイスの使い分け用)
static bool ctrl_to(struct usb_disp_hal *h, uint8_t addr, uint8_t mps0,
                    uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue,
                    uint16_t wIndex, void *data, uint16_t wLength,
                    uint16_t *actual) {
    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)wValue;
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)wIndex;
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)wLength;
    setup[7] = (uint8_t)(wLength >> 8);
    return usb_disp_udh_host_control_addr(h->host, addr, mps0, setup, data, actual);
}

// アドレス設定済みデバイスの本体列挙: ディスクリプタ読み → バルク OUT EP
// 探索 → SET_CONFIGURATION → udh へ登録。
// 戻り値: 0=成功, -1=失敗(再試行対象), -2=バルクOUT無し
static int8_t enum_addressed(struct usb_disp_hal *h, uint8_t addr,
                             uint8_t mps0) {
    uint16_t actual = 0;

    if (!ctrl_to(h, addr, mps0, 0x80, 0x06, 0x0100, 0, s_desc_buf, 18,
                 &actual) || actual < 18) {
        usb_disp_log("[ENUM] device descriptor (18) failed");
        return -1;
    }
    h->vid = (uint16_t)(s_desc_buf[8] | (s_desc_buf[9] << 8));
    h->pid = (uint16_t)(s_desc_buf[10] | (s_desc_buf[11] << 8));
    uint8_t num_cfgs = s_desc_buf[17];
    usb_disp_log("[ENUM] addr=%u VID=%04X PID=%04X mps0=%u", addr, h->vid,
                 h->pid, mps0);

    h->bulk_ep = 0;
    uint8_t cfg_value = 0;
    bool got_vendor_ep = false;  // 採用済み EP が vendor class IF のものか
    for (uint8_t ci = 0; ci < num_cfgs && ci < 4; ci++) {
        if (!ctrl_to(h, addr, mps0, 0x80, 0x06, (uint16_t)(0x0200 + ci), 0,
                     s_desc_buf, 9, &actual) ||
            actual < 9) {
            usb_disp_log("[ENUM] config %u (9) failed", ci);
            return -1;
        }
        uint16_t total = (uint16_t)(s_desc_buf[2] | (s_desc_buf[3] << 8));
        if (total > sizeof(s_desc_buf)) total = sizeof(s_desc_buf);
        if (!ctrl_to(h, addr, mps0, 0x80, 0x06, (uint16_t)(0x0200 + ci), 0,
                     s_desc_buf, total, &actual) ||
            actual < total) {
            usb_disp_log("[ENUM] config %u (%u) failed", ci, total);
            return -1;
        }

        // バルク OUT EP を探す。
        // alt setting 0 のみ対象、vendor class (0xFF) インターフェース優先。
        // 優先はコンフィグ横断で効かせる:
        // オートインストール機 (GX-DVI/U2AI 実測) は
        // cfg[0]=Mass Storage のみ / cfg[1]=表示 (vendor IF) の複数コンフィグ構成で、
        // cfg[0] の MSC バルク OUT を掴むと映像が全く出ない。
        // vendor IF を持つコンフィグが見つかるまで全コンフィグを走査する。
        // 0x5F ベンダーディスクリプタも捕捉する
        // (チップ判別フォールバック用、usb_disp_hal_vendor_desc。表示コンフィグ内に埋め込む個体あり)
        const uint8_t *p = s_desc_buf;
        const uint8_t *end = s_desc_buf + total;
        uint8_t cur_alt = 0;
        bool cur_vendor_if = false;
        while (p + 1 < end && p[0] >= 2 && p + p[0] <= end) {
            uint8_t len = p[0], type = p[1];
            if (type == 0x04 && len >= 9) {  // INTERFACE
                cur_alt = p[3];
                cur_vendor_if = (p[5] == 0xFF);
            } else if (type == 0x05 && len >= 7) {  // ENDPOINT
                uint8_t ep_addr = p[2];
                uint8_t attr = p[3] & 0x03;
                uint16_t mps = (uint16_t)(p[4] | (p[5] << 8));
                if (attr == 0x02 && (ep_addr & 0x80) == 0 && cur_alt == 0 &&
                    (h->bulk_ep == 0 || (cur_vendor_if && !got_vendor_ep))) {
                    h->bulk_ep = ep_addr;
                    h->bulk_mps = mps;
                    got_vendor_ep = cur_vendor_if;
                    cfg_value = s_desc_buf[5];
                }
            } else if (type == 0x5F && h->vdesc_len == 0) {  // DL vendor desc
                uint8_t n = (len <= sizeof(h->vdesc)) ? len : sizeof(h->vdesc);
                memcpy(h->vdesc, p, n);
                h->vdesc_len = n;
            }
            p += len;
        }
        if (got_vendor_ep) break;  // vendor IF のバルク OUT で確定
        if (ci == 0 && cfg_value == 0) cfg_value = s_desc_buf[5];
    }

    if (h->bulk_ep == 0) {
        usb_disp_log("[ENUM] no bulk OUT endpoint");
        if (cfg_value != 0)
            ctrl_to(h, addr, mps0, 0x00, 0x09, cfg_value, 0, NULL, 0, NULL);
        return -2;
    }

    if (!ctrl_to(h, addr, mps0, 0x00, 0x09, cfg_value, 0, NULL, 0, NULL)) {
        usb_disp_log("[ENUM] SET_CONFIGURATION failed");
        return -1;
    }
    // バルク / 死活プローブの対象をこのデバイスにする
    usb_disp_udh_host_set_device(h->host, addr, mps0);
    usb_disp_udh_host_set_bulk_out(h->host, h->bulk_ep, h->bulk_mps);
    usb_disp_log("[ENUM] configured. addr=%u cfg=%u%s bulk OUT=%02X mps=%u",
                 addr, cfg_value, got_vendor_ep ? " (vendor IF)" : "",
                 h->bulk_ep, h->bulk_mps);
    return 0;
}

// ---------------------------------------------------------------
//  ハブ対応 (多段可、FS デバイスのみ)
//    ハブ検出 → SET_CONFIGURATION → 全ポート給電 → GET_STATUS ポーリング。
//    接続を見つけたらポートリセット → 下流をアドレス 2+ で列挙し、
//    DisplayLink (VID 0x17E9) ならバルク対象として構成する。
//    下流がさらにハブならテーブルに追加して同様に走査する
//    (多段,アクティブ USB ケーブル = 内蔵1ポートハブにも対応)。
//    非 DL はアドレスだけ振って放置、Low-Speed はスキップ。
//    DL の切断は udh の死活プローブ/バルク dead 検出が拾い、バス全体を再列挙する
//    (ハブは接続されたままなので数百 ms で HW_HUB_WAIT に戻る)
// ---------------------------------------------------------------

static bool hub_port_status(struct usb_disp_hal *h, struct hub_ent *hub,
                            uint8_t port, uint16_t *status,
                            uint16_t *change) {
    uint8_t st[4];
    uint16_t actual = 0;
    if (!ctrl_to(h, hub->addr, hub->mps0, 0xA3, 0x00, 0, port, st, 4,
                 &actual) || actual < 4)
        return false;
    *status = (uint16_t)(st[0] | (st[1] << 8));
    *change = (uint16_t)(st[2] | (st[3] << 8));
    return true;
}

static bool hub_port_feature(struct usb_disp_hal *h, struct hub_ent *hub,
                             bool set, uint8_t feat, uint8_t port) {
    return ctrl_to(h, hub->addr, hub->mps0, 0x23, set ? 0x03 : 0x01, feat,
                   port, NULL, 0, NULL);
}

// アドレス設定済みのデバイスをハブとして構成し、テーブルへ追加する。
// depth = 1 (ルート直下) から数える段数
static bool hub_setup(struct usb_disp_hal *h, uint8_t addr, uint8_t mps0,
                      uint8_t depth) {
    uint16_t actual = 0;

    if (h->nhubs >= USB_DISP_HUB_MAX_HUBS) {
        usb_disp_log("[ENUM] too many hubs (max %u)", USB_DISP_HUB_MAX_HUBS);
        return false;
    }
    if (!ctrl_to(h, addr, mps0, 0x80, 0x06, 0x0100, 0, s_desc_buf, 18,
                 &actual) || actual < 18) {
        usb_disp_log("[ENUM] hub device descriptor failed");
        return false;
    }
    usb_disp_log("[ENUM] hub addr=%u depth=%u VID=%04X PID=%04X", addr,
                 depth, s_desc_buf[8] | (s_desc_buf[9] << 8),
                 s_desc_buf[10] | (s_desc_buf[11] << 8));

    if (!ctrl_to(h, addr, mps0, 0x80, 0x06, 0x0200, 0, s_desc_buf, 9,
                 &actual) || actual < 9) {
        usb_disp_log("[ENUM] hub config descriptor failed");
        return false;
    }
    uint8_t cfg_value = s_desc_buf[5];
    if (!ctrl_to(h, addr, mps0, 0x00, 0x09, cfg_value, 0, NULL, 0, NULL)) {
        usb_disp_log("[ENUM] hub SET_CONFIGURATION failed");
        return false;
    }

    // ハブクラスディスクリプタ (type 0x29): bNbrPorts / bPwrOn2PwrGood
    if (!ctrl_to(h, addr, mps0, 0xA0, 0x06, 0x2900, 0, s_desc_buf, 9,
                 &actual) || actual < 6) {
        usb_disp_log("[ENUM] hub class descriptor failed");
        return false;
    }
    uint8_t nports = s_desc_buf[2];
    uint8_t pwr2good_2ms = s_desc_buf[5];
    if (nports > USB_DISP_HUB_MAX_PORTS) nports = USB_DISP_HUB_MAX_PORTS;

    struct hub_ent *hub = &h->hubs[h->nhubs];
    hub->addr = addr;
    hub->mps0 = mps0;
    hub->nports = nports;
    hub->depth = depth;
    hub->port_done = 0;
    hub->fail = 0;

    for (uint8_t p = 1; p <= nports; p++) {
        hub_port_feature(h, hub, true, USB_DISP_HUB_FEAT_PORT_POWER, p);
    }
    sleep_ms((uint32_t)pwr2good_2ms * 2 + 50);

    h->nhubs++;
    usb_disp_log("[ENUM] hub addr=%u: %u port(s) powered", addr, nports);
    return true;
}

// 接続を検出したポートをリセットして下流デバイスを列挙する。
// 戻り値: true = DisplayLink を構成できた (HW_ATTACHED へ)
static bool hub_port_attach(struct usb_disp_hal *h, struct hub_ent *hub,
                            uint8_t port) {
    uint16_t status, change;

    // デバウンス (TATTDB 相当): 100ms 後も接続が安定しているか
    sleep_ms(100);
    if (!hub_port_status(h, hub, port, &status, &change) ||
        !(status & USB_DISP_HUB_PS_CONNECTION))
        return false;

    // ポートリセット → C_PORT_RESET を待つ
    if (!hub_port_feature(h, hub, true, USB_DISP_HUB_FEAT_PORT_RESET, port))
        return false;
    bool reset_done = false;
    for (uint8_t i = 0; i < 50; i++) {
        sleep_ms(2);
        if (!hub_port_status(h, hub, port, &status, &change)) return false;
        if (change & USB_DISP_HUB_PC_RESET) {
            reset_done = true;
            break;
        }
    }
    hub_port_feature(h, hub, false, USB_DISP_HUB_FEAT_C_PORT_RESET, port);
    if (!reset_done || !(status & USB_DISP_HUB_PS_ENABLE)) {
        usb_disp_log("[ENUM] hub %u port %u reset failed", hub->addr, port);
        return false;
    }
    if (status & USB_DISP_HUB_PS_LOW_SPEED) {
        usb_disp_log("[ENUM] hub %u port %u: Low-Speed device (unsupported)",
                     hub->addr, port);
        return false;
    }
    sleep_ms(10);  // リセットリカバリ (TRSTRCY)

    // 下流デバイスはアドレス 0 で応答する。8B 読み → アドレス割り当て
    uint16_t actual = 0;
    if (!ctrl_to(h, 0, 8, 0x80, 0x06, 0x0100, 0, s_desc_buf, 8, &actual) ||
        actual < 8) {
        usb_disp_log("[ENUM] hub %u port %u: device descriptor (8) failed",
                     hub->addr, port);
        return false;
    }
    uint8_t mps0 = s_desc_buf[7];
    if (mps0 < 8) mps0 = 8;
    uint8_t dev_class = s_desc_buf[4];

    uint8_t addr = h->next_addr;
    if (addr >= 0x7F) return false;  // アドレス枯渇
    if (!ctrl_to(h, 0, 8, 0x00, 0x05, addr, 0, NULL, 0, NULL)) {
        usb_disp_log("[ENUM] hub %u port %u: SET_ADDRESS failed", hub->addr,
                     port);
        return false;
    }
    h->next_addr++;
    sleep_ms(5);

    if (dev_class == 0x09) {
        // 多段ハブ: テーブルへ追加して以後のスキャン対象にする
        if (hub->depth >= USB_DISP_HUB_MAX_DEPTH) {
            usb_disp_log("[ENUM] hub %u port %u: hub too deep (max %u)",
                         hub->addr, port, USB_DISP_HUB_MAX_DEPTH);
            return false;
        }
        hub_setup(h, addr, mps0, (uint8_t)(hub->depth + 1));
        return false;  // DL ではないので attach ではない (次のスキャンで下流を見る)
    }

    // VID/PID を見て対応チップ以外はアドレスだけ振って放置
    // (Pico=FS は DL-1xx のみ対応。テーブル側で自動的にそうなる)
    uint16_t vid = 0, pid = 0;
    if (ctrl_to(h, addr, mps0, 0x80, 0x06, 0x0100, 0, s_desc_buf, 18,
                &actual) && actual >= 18) {
        vid = (uint16_t)(s_desc_buf[8] | (s_desc_buf[9] << 8));
        pid = (uint16_t)(s_desc_buf[10] | (s_desc_buf[11] << 8));
    }
    if (!usb_disp_supported_device(vid, pid)) {
        usb_disp_log("[ENUM] hub %u port %u: ignoring unsupported "
                     "%04X:%04X", hub->addr, port, vid, pid);
        return false;
    }

    int8_t r = enum_addressed(h, addr, mps0);
    if (r != 0) {
        usb_disp_log("[ENUM] hub %u port %u: DL enum failed (%d)", hub->addr,
                     port, r);
        return false;
    }
    usb_disp_log("[ENUM] hub %u port %u: DisplayLink attached", hub->addr,
                 port);
    return true;
}

// HW_HUB_WAIT: 全ハブの全ポートを見て、新規接続があれば列挙を試みる。
// hub_port_attach が多段ハブを hubs[] に追加すると、
// この関数の同一パス内でそのまま走査対象に含まれる
// (hubs[] は追加専用のため添字で回せる)
static void hub_scan(struct usb_disp_hal *h) {
    for (uint8_t i = 0; i < h->nhubs; i++) {
        struct hub_ent *hub = &h->hubs[i];
        bool hub_ok = true;
        for (uint8_t p = 1; p <= hub->nports; p++) {
            uint16_t status, change;
            if (!hub_port_status(h, hub, p, &status, &change)) {
                hub_ok = false;
                break;
            }
            hub->fail = 0;
            if (change & USB_DISP_HUB_PC_CONNECTION)
                hub_port_feature(h, hub, false,
                                 USB_DISP_HUB_FEAT_C_PORT_CONNECTION, p);

            bool conn = (status & USB_DISP_HUB_PS_CONNECTION) != 0;
            bool done = (hub->port_done & (1u << p)) != 0;
            if (!conn) {
                hub->port_done &= (uint8_t)~(1u << p);
                continue;
            }
            if (done) continue;
            // 新規接続。成否に関わらず一度だけ処理する
            // (非 DL の再試行はしない。失敗した DL は抜き差しか CLI 'x' で再列挙)
            hub->port_done |= (uint8_t)(1u << p);
            if (hub_port_attach(h, hub, p)) {
                h->state = HW_ATTACHED;
                return;
            }
        }
        if (!hub_ok) {
            // このハブが応答しない (上流ごと抜かれた等)。
            // 数回連続で失敗したらトポロジが壊れているのでバス全体を再列挙する
            if (++hub->fail >= 3) {
                usb_disp_log("[ENUM] hub addr=%u lost, re-enumerating",
                             hub->addr);
                usb_disp_hal_request_reenum(h);
                return;
            }
        }
    }
}

// USB 標準エニュメレーション (バスリセット直後に呼ぶ)
// 戻り値:  0=成功,
//          -1=失敗(再試行対象),
//          -2=バルクOUT無し(長期再試行),
//          -3=ハブ構成完了 (下流の接続待ちへ)
static int8_t enum_device(struct usb_disp_hal *h) {
    uint16_t actual = 0;

    h->vdesc_len = 0;
    h->nhubs = 0;
    h->next_addr = USB_DISP_HUB_DEV_ADDR_BASE;
    usb_disp_udh_host_set_device(h->host, 0, 8);
    if (!ctrl_to(h, 0, 8, 0x80, 0x06, 0x0100, 0, s_desc_buf, 8, &actual) ||
        actual < 8) {
        usb_disp_log("[ENUM] device descriptor (8) failed");
        return -1;
    }
    uint8_t mps0 = s_desc_buf[7];
    if (mps0 < 8) mps0 = 8;
    uint8_t dev_class = s_desc_buf[4];

    if (!ctrl_to(h, 0, 8, 0x00, 0x05, USB_DISP_ROOT_ADDR, 0, NULL, 0, NULL)) {
        usb_disp_log("[ENUM] SET_ADDRESS failed");
        return -1;
    }
    sleep_ms(5);
    usb_disp_udh_host_set_device(h->host, USB_DISP_ROOT_ADDR, mps0);

    if (dev_class == 0x09) {
        // ハブ直結 → 構成して下流の接続待ちへ。
        // DL が見つかるまでは死活プローブの対象をルートハブにしておく
        // (ハブが抜かれたらバス再列挙が走る)
        if (!hub_setup(h, USB_DISP_ROOT_ADDR, mps0, 1)) return -1;
        return -3;
    }
    return enum_addressed(h, USB_DISP_ROOT_ADDR, mps0);  // 従来の直結パス
}

static void hal_reset_device_state(struct usb_disp_hal *h) {
    h->vid = h->pid = 0;
    h->bulk_ep = 0;
    h->vdesc_len = 0;
    memset(h->hubs, 0, sizeof(h->hubs));
    h->nhubs = 0;
}

// ---------------------------------------------------------------
// HAL インターフェース実装
// ---------------------------------------------------------------

// PIO USB は sys_clk が 12MHz の倍数であることが必要。倍数でなければ
// 「下の倍数」へ自動調整する (例: 既定の RP2350 150MHz → 144MHz,
// RP2040 133MHz → 132MHz)。上方向へは絶対に丸めない = クロックアップ
// しないので vreg (電圧) はそのままで安全。アプリが意図的に設定した
// クロック (240MHz 等の倍数) はそのまま通る。
static void ensure_12mhz_multiple(void) {
    uint32_t hz = clock_get_hz(clk_sys);
    if (hz % 12000000u == 0) return;
    uint32_t target = (hz / 12000000u) * 12000000u;  // 常に下へ丸める
    if (target < 48000000u) target = 48000000u;
    if (set_sys_clock_khz(target / 1000u, false)) {
        usb_disp_log("[HAL] sys_clk %lu -> %lu kHz (PIO USB needs a multiple "
                     "of 12MHz)", (unsigned long)(hz / 1000u),
                     (unsigned long)(target / 1000u));
    } else {
        usb_disp_log("[HAL] sys_clk %lu kHz: not a multiple of 12MHz and "
                     "auto-adjust failed - display will not work",
                     (unsigned long)(hz / 1000u));
    }
}

usb_disp_hal_t *usb_disp_hal_add(const usb_disp_config_t *cfg) {
    if (!cfg || s_nhal >= USB_DISP_MAX) return NULL;
    ensure_12mhz_multiple();
    // cfg->port = 0 始まりの PIO ブロック番号 (プラットフォーム共通 config)
    static PIO const k_pio[NUM_PIOS] = {
        pio0, pio1,
#if NUM_PIOS >= 3
        pio2,
#endif
    };
    if (cfg->port >= NUM_PIOS) {
        usb_disp_log("[HAL] add failed (port %u >= NUM_PIOS %u)", cfg->port,
                     (unsigned)NUM_PIOS);
        return NULL;
    }
    // pin_dp/pin_dm は両方明示必須 (隣接チェックは usb_disp_udh_host_add が行う)
    usb_disp_udh_host_t *host = usb_disp_udh_host_add(k_pio[cfg->port], cfg->pin_dp,
                                    cfg->pin_dm);
    if (!host) {
        usb_disp_log("[HAL] add failed (dp=%u dm=%u: pins not adjacent, "
                     "or PIO program load refused - see prior log)",
                     cfg->pin_dp, cfg->pin_dm);
        return NULL;
    }
    struct usb_disp_hal *h = &s_hal[s_nhal++];
    memset(h, 0, sizeof(*h));
    h->in_use = true;
    h->host = host;
    h->state = HW_WAIT_DEVICE;
    return h;
}

#if defined(ARDUINO)
// arduino-pico はスケッチが setup1()/loop1() を定義していると core1 で main1 を起動する。
// その状態で multicore_launch_core1 を呼ぶと SDK の起動ハンドシェイク (エコー応答待ち) が
// 完了せず core0 が永久ブロックするため、弱シンボル参照で検出して手動モードへ落とす。
// (コアの RP2040Support.h と同じ C リンケージで宣言しないと別シンボルになる)
extern "C" void setup1() __attribute__((weak));
extern "C" void loop1() __attribute__((weak));
#endif

void usb_disp_hal_start(void) {
#if defined(__FREERTOS)
    // arduino-pico の FreeRTOS モード (menu: FreeRTOS SMP) は
    // スケジューラが両コアを使うため multicore_launch_core1 は不可
    // 常に手動モードへ: loop1(){ usb_disp_task(); delay(1); }
    //  (delay 無しのタイトループは低優先度タスク=USB CDC を飢餓させる。
    //  delay(1) でも SOF は維持され表示動作を実機確認済み)。
    usb_disp_log("[HAL] FreeRTOS build: core1 launch is not possible. "
                 "Manual mode - call usb_disp_task() from loop1() "
                 "(with delay(1) to avoid starving other tasks).");
    usb_disp_udh_host_start_manual();
#elif defined(ARDUINO)
    if (setup1 || loop1) {
        usb_disp_log("[HAL] setup1/loop1 detected: core1 is owned by the "
                     "sketch. Falling back to manual mode - call "
                     "usb_disp_task() from loop1().");
        usb_disp_udh_host_start_manual();
        return;
    }
    usb_disp_udh_host_start();
#else
    usb_disp_udh_host_start();
#endif
}

void usb_disp_hal_start_manual(void) {
    usb_disp_udh_host_start_manual();
}

void usb_disp_hal_task(void) {
    usb_disp_udh_host_task();
}

void usb_disp_hal_poll(usb_disp_hal_t *h) {
    uint32_t now = usb_disp_hal_ms();
    bool running = (usb_disp_udh_host_state(h->host) == USB_DISP_UDH_RUNNING);

    switch (h->state) {
    case HW_WAIT_DEVICE:
        if (!running) {
            if (!h->retry_pending) h->attempts = 0;
            break;
        }
        h->retry_pending = false;
        {
            int8_t r = enum_device(h);
            if (r == 0) {
                h->attempts = 0;
                h->state = HW_ATTACHED;
            } else if (r == -3) {
                h->attempts = 0;
                h->hub_poll_at_ms = now - USB_DISP_HUB_POLL_MS;  // 即スキャン
                h->state = HW_HUB_WAIT;
            } else if (r == -2) {
                h->no_ep_since_ms = now;
                h->state = HW_NO_EP;
            } else if (++h->attempts < USB_DISP_ENUM_MAX_ATTEMPTS) {
                usb_disp_log("[ENUM] retry in %dms (%d/%d)",
                             USB_DISP_ENUM_RETRY_DELAY_MS, h->attempts,
                             USB_DISP_ENUM_MAX_ATTEMPTS);
                hal_reset_device_state(h);
                h->retry_at_ms = now + USB_DISP_ENUM_RETRY_DELAY_MS;
                h->state = HW_RETRY_WAIT;
            } else {
                usb_disp_log("[ENUM] giving up, periodic retry");
                hal_reset_device_state(h);
                h->no_ep_since_ms = now;
                h->state = HW_NO_EP;  // 周期再試行へ
            }
        }
        break;

    case HW_RETRY_WAIT:
        if (!running) {
            hal_reset_device_state(h);
            h->state = HW_WAIT_DEVICE;
            break;
        }
        if ((int32_t)(now - h->retry_at_ms) >= 0) {
            h->retry_pending = true;
            usb_disp_udh_host_request_reenum(h->host);
            h->state = HW_WAIT_DEVICE;
        }
        break;

    case HW_ATTACHED:
        if (!running) {
            hal_reset_device_state(h);
            h->state = HW_WAIT_DEVICE;
        }
        break;

    case HW_NO_EP:
        if (!running) {
            hal_reset_device_state(h);
            h->state = HW_WAIT_DEVICE;
            break;
        }
        if (now - h->no_ep_since_ms >= USB_DISP_ENUM_NO_EP_RETRY_MS) {
            usb_disp_log("[ENUM] periodic re-enumeration...");
            hal_reset_device_state(h);
            h->attempts = 0;
            h->retry_pending = true;
            usb_disp_udh_host_request_reenum(h->host);
            h->state = HW_WAIT_DEVICE;
        }
        break;

    case HW_HUB_WAIT:
        // ハブ構成済み、下流の DisplayLink 待ち (GET_STATUS ポーリング)。
        // ハブ自体の切断は udh の死活プローブ (対象=ハブ) と
        // hub_scan 内の GET_STATUS 連続失敗の両方で検出される
        if (!running) {
            hal_reset_device_state(h);
            h->state = HW_WAIT_DEVICE;
            break;
        }
        if (now - h->hub_poll_at_ms < USB_DISP_HUB_POLL_MS) break;
        h->hub_poll_at_ms = now;
        hub_scan(h);  // DL を構成できたら HW_ATTACHED へ遷移する
        break;
    }
}

bool usb_disp_hal_attached(usb_disp_hal_t *h) {
    return h->state == HW_ATTACHED &&
           usb_disp_udh_host_state(h->host) == USB_DISP_UDH_RUNNING;
}

uint16_t usb_disp_hal_vid(usb_disp_hal_t *h) { return h->vid; }
uint16_t usb_disp_hal_pid(usb_disp_hal_t *h) { return h->pid; }

bool usb_disp_hal_ctrl(usb_disp_hal_t *h, const uint8_t setup[8], void *data,
                       uint16_t *actual) {
    return usb_disp_udh_host_control(h->host, setup, data, actual);
}

uint32_t usb_disp_hal_bulk_write(usb_disp_hal_t *h, const void *data,
                                 uint32_t len) {
    return usb_disp_udh_host_bulk_write(h->host, data, len);
}

bool usb_disp_hal_bulk_flush(usb_disp_hal_t *h, uint32_t timeout_ms) {
    return usb_disp_udh_host_bulk_flush(h->host, timeout_ms);
}

void usb_disp_hal_request_reenum(usb_disp_hal_t *h) {
    hal_reset_device_state(h);
    h->retry_pending = true;
    h->state = HW_WAIT_DEVICE;
    usb_disp_udh_host_request_reenum(h->host);
}

// Pico 固有: 統計アクセス用に udh ハンドルを公開 (usb_disp_get_host が使う)
usb_disp_udh_host_t *usb_disp_hal_pico_host(usb_disp_hal_t *h) {
    return h->host;
}

uint64_t usb_disp_hal_stat_bytes(usb_disp_hal_t *h) {
    return usb_disp_udh_host_stat_bytes(h->host);
}

bool usb_disp_hal_bulk_zlp(usb_disp_hal_t *h) {
    // Pico (FS) に ZLP を要するプロトコル (MS913x) は来ない → no-op
    (void)h;
    return true;
}

bool usb_disp_hal_bulk_split(usb_disp_hal_t *h) {
    // Pico (FS) に転送境界を要するプロトコル (T6) は来ない → no-op
    (void)h;
    return true;
}

uint16_t usb_disp_hal_vendor_desc(usb_disp_hal_t *h, void *buf,
                                  uint16_t maxlen) {
    uint16_t n = h->vdesc_len;
    if (n == 0) return 0;
    if (n > maxlen) n = maxlen;
    memcpy(buf, h->vdesc, n);
    return n;
}

// ---- 大容量FBメモリ: Pico は非対応 ----
void *usb_disp_hal_fb_alloc(uint32_t size) {
    (void)size;
    return NULL;
}
void usb_disp_hal_fb_free(void *p, uint32_t size) { (void)p; (void)size; }
uint32_t usb_disp_hal_fb_used(void) { return 0; }

#endif  // USB_DISP_PORT_PICO

