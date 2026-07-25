//
// ######################################################################
//
//    usb_disp_hal_teensy - Teensy 4.x (i.MX RT1062) バックエンド
//
//    Teensyduino 同梱の USBHost_t36 (EHCI High-Speed ホスト) を
//    usb_disp_hal インターフェースで包む。
//    エニュメレーションは USBHost_t36 スタックが行う。
//      - USBDriver として対応 VID/PID のデバイスを claim
//      - コンフィグディスクリプタからバルク OUT EP を探しパイプを構成
//      - コントロール転送 (同期化) とバルク OUT リング (4 x 8KB) の提供
//    DL プロトコルはコア (usb_disp.cpp) がそのまま動く。
//
//    重要 (DL アダプタのエニュメレーション対策):
//      USBHost_t36 は接続の 100ms 後にリセット→GET_DESCRIPTOR を発行する
//      が、DL-1x5 は VBUS 投入後の内部起動に 1-2 秒かかり、その最中に
//      突かれると EP0 が STALL/無応答のまま固まり列挙不能になる
//      本バックエンドは poll からデバウンスタイマーを長い値
//      (USB_DISP_TEENSY_SETTLE_MS) で再スタートして回避する。
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include "usb_disp_hal.h"

#if USB_DISP_PORT_TEENSY

#include <string.h>

#include <Arduino.h>
#include <USBHost_t36.h>   // IRQ_USBHS もここから (utility/imxrt_usbhs.h)

#define USB_DISP_BULK_XFER_COUNT 4
#define USB_DISP_BULK_XFER_SIZE 8192
#define USB_DISP_CTRL_MAX_DATA 512
#define USB_DISP_CTRL_TIMEOUT_MS 1000

// ハブ経由のアダプタ対応 (内蔵ハブ機/ドック用)。0..2。
// 不要なら -DUSB_DISP_TEENSY_HUBS=0 で RAM を節約できる
#ifndef USB_DISP_TEENSY_HUBS
#define USB_DISP_TEENSY_HUBS 2
#endif

// 接続検出からリセット開始までの猶予 [ms] (ファイル冒頭の「重要」参照)。
// DL-165 実機で 100ms (USBHost_t36 既定) は列挙不能、1500ms で成功を確認
#ifndef USB_DISP_TEENSY_SETTLE_MS
#define USB_DISP_TEENSY_SETTLE_MS 1500
#endif

struct usb_disp_hal {
    bool in_use;

    volatile bool attached;
    volatile bool pending_probe;   // 他コンフィグ探索をメインコンテキストで
    volatile bool announce;        // attach ログをメインコンテキストで出す
    uint16_t vid, pid;
    uint8_t bulk_ep;
    uint16_t bulk_mps;
    uint8_t speed;                 // Device_t::speed (0=FS, 1=LS, 2=HS)
    uint8_t vdesc[64];             // config 内 0x5F ベンダーディスクリプタ
    uint8_t vdesc_len;

    // バルク OUT リング。単一エンドポイントのバルク転送は投入順に
    // 完了するので、空きスロットはラウンドロビンで一意に決まる
    volatile uint8_t free_slots;   // ISR (完了) が増やし、メインが減らす
    uint8_t next_slot;
    int8_t cur;                    // 書き込み中スロット (-1 = なし)
    uint32_t cur_fill;

    volatile uint32_t bulk_err;    // エラー完了 (followup_Error 経由)
    volatile uint32_t bulk_short;  // 送り残しのある完了
    uint64_t stat_bytes;           // 累積バルク送信バイト数
};

static struct usb_disp_hal s_hal[USB_DISP_MAX];
static uint8_t s_nhal = 0;
static bool s_started = false;

// バルク送信バッファ (static = DTCM。冒頭のメモリ注意を参照)
static uint8_t s_bulk_buf[USB_DISP_BULK_XFER_COUNT][USB_DISP_BULK_XFER_SIZE];

// 同期コントロール転送の状態。setup はセットアップステージの DMA
// バッファそのものとして使われるため、完了まで保持が必要 (= static)
static struct {
    volatile bool busy;        // 発行済み・未消費
    volatile bool done;        // 完了 (成功/失敗を問わず)
    volatile bool failed;      // エラー完了 (STALL 等)
    volatile bool abandoned;   // タイムアウトで待ち手が去った → ISR 側で破棄
    setup_t setup;
    uint8_t data[USB_DISP_CTRL_MAX_DATA];
} s_ctrl;

uint32_t usb_disp_hal_ms(void) { return millis(); }

// ---------------------------------------------------------------
// USBHost_t36 ドライバ
// ---------------------------------------------------------------

static void bulk_cb(const Transfer_t *transfer);
static void bulk_err_cb(const Transfer_t *transfer);
static void ctrl_err_cb(const Transfer_t *transfer);

// コントロールパイプの元エラーコールバック (enumeration_error)。
// 自分の転送のエラーだけ横取りし、それ以外 (エニュメ中のエラー) は戻す
static void (*s_enum_error_cb)(const Transfer_t *) = NULL;

class USBDispDriver : public USBDriver {
public:
    USBDispDriver(USBHost &host) {
        (void)host;               // 全インスタンス共有 (静的) だが作法として受ける
        driver_ready_for_device(this);
    }
    Device_t *dev() { return *(Device_t * volatile *)&device; }

    // protected な USBHost 静的メンバへのファイル内アクセス用
    bool ctrl_submit(setup_t *s, void *buf) {
        Device_t *d = dev();
        if (!d) return false;
        return queue_Control_Transfer(d, s, buf, this);
    }
    bool bulk_submit(void *buf, uint32_t len) {
        if (!bulk_pipe) return false;
        return queue_Data_Transfer(bulk_pipe, buf, len, this);
    }
    // メインコンテキストからのパイプ作成 (他コンフィグ探索用)。
    // USBHost_t36 のプール操作は ISR と排他されないため IRQ を止めて行う
    bool make_bulk_pipe_irqsafe(uint8_t ep, uint16_t mps) {
        Device_t *d = dev();
        if (!d) return false;
        NVIC_DISABLE_IRQ(IRQ_USBHS);
        Pipe_t *p = new_Pipe(d, 2 /*bulk*/, ep & 0x0F, 0 /*OUT*/, mps);
        if (p) {
            p->callback_function = bulk_cb;
            p->error_callback_function = bulk_err_cb;
            bulk_pipe = p;
        }
        NVIC_ENABLE_IRQ(IRQ_USBHS);
        return p != NULL;
    }

    Pipe_t *bulk_pipe = NULL;

protected:
    virtual bool claim(Device_t *device, int type, const uint8_t *descriptors,
                       uint32_t len);
    virtual void control(const Transfer_t *transfer);
    virtual void disconnect();
};

static USBHost s_usbhost;
#if USB_DISP_TEENSY_HUBS >= 1
static USBHub s_hub1(s_usbhost);
#endif
#if USB_DISP_TEENSY_HUBS >= 2
static USBHub s_hub2(s_usbhost);
#endif
static USBDispDriver s_drv(s_usbhost);

// ---------------------------------------------------------------
// ディスクリプタ列の走査 (コンフィグヘッダ有無どちらの列でも可)
//   - alt setting 0 のみ対象
//   - vendor class (0xFF) のインターフェースを優先 (オートインストール用
//     Mass Storage のバルク OUT を誤って掴まないように)
//   - 0x5F ベンダーディスクリプタも捕捉 (チップ判別フォールバック)
// 戻り値: バルク OUT EP が見つかったか。*vendor_if は採用 EP が
// vendor class IF のものだったか
// ---------------------------------------------------------------

static bool scan_descriptors(struct usb_disp_hal *h, const uint8_t *p,
                             uint32_t len, uint8_t *ep_out, uint16_t *mps_out,
                             bool *vendor_if) {
    const uint8_t *end = p + len;
    uint8_t ep = 0;
    uint16_t mps = 0;
    uint8_t cur_alt = 0;
    bool cur_vendor = false, got_vendor = false;
    while (p + 1 < end && p[0] >= 2 && p + p[0] <= end) {
        uint8_t dlen = p[0], dtype = p[1];
        if (dtype == 0x04 && dlen >= 9) {  // INTERFACE
            cur_alt = p[3];
            cur_vendor = (p[5] == 0xFF);
        } else if (dtype == 0x05 && dlen >= 7) {  // ENDPOINT
            if ((p[3] & 0x03) == 0x02 && (p[2] & 0x80) == 0 && cur_alt == 0 &&
                (ep == 0 || (cur_vendor && !got_vendor))) {
                ep = p[2];
                mps = (uint16_t)(p[4] | (p[5] << 8));
                got_vendor = cur_vendor;
            }
        } else if (dtype == 0x5F && h->vdesc_len == 0) {  // DL vendor desc
            uint8_t n = (dlen <= sizeof(h->vdesc)) ? dlen : sizeof(h->vdesc);
            memcpy(h->vdesc, p, n);
            h->vdesc_len = n;
        }
        p += dlen;
    }
    if (ep == 0) return false;
    *ep_out = ep;
    *mps_out = mps;
    if (vendor_if) *vendor_if = got_vendor;
    return true;
}

// ---------------------------------------------------------------
// USBDriver コールバック (すべて USB ISR コンテキスト)
// ---------------------------------------------------------------

// デバイス単位 (type 0) で claim する。descriptors はコンフィグ
// ディスクリプタからヘッダ 9 バイトを除いた列 (enumeration.cpp 参照)。
// ISR コンテキストなのでログは出さず、メインコンテキスト (poll) に回す
bool USBDispDriver::claim(Device_t *device, int type,
                          const uint8_t *descriptors, uint32_t len) {
    if (type != 0) return false;
    if (s_nhal == 0) return false;   // ポート未登録
    struct usb_disp_hal *h = &s_hal[0];
    if (h->attached || h->pending_probe) return false;  // 1台のみ
    if (!usb_disp_supported_device(device->idVendor, device->idProduct))
        return false;

    h->vid = device->idVendor;
    h->pid = device->idProduct;
    h->speed = device->speed;
    h->vdesc_len = 0;
    h->bulk_err = 0;
    h->bulk_short = 0;

    // 自分のコントロール転送のエラー (STALL 等) を即座に受け取れるよう、
    // コントロールパイプのエラーコールバックを横取りする
    // (エニュメ分は元の enumeration_error へ転送)
    // パイプは切断時にスタックが破棄する
    if (device->control_pipe &&
        device->control_pipe->error_callback_function != ctrl_err_cb) {
        s_enum_error_cb = device->control_pipe->error_callback_function;
        device->control_pipe->error_callback_function = ctrl_err_cb;
    }

    uint8_t ep = 0;
    uint16_t mps = 0;
    bool vendor_if = false;
    if (scan_descriptors(h, descriptors, len, &ep, &mps, &vendor_if) &&
        vendor_if) {
        Pipe_t *p = new_Pipe(device, 2 /*bulk*/, ep & 0x0F, 0 /*OUT*/, mps);
        if (p) {
            p->callback_function = bulk_cb;
            p->error_callback_function = bulk_err_cb;
            bulk_pipe = p;
            h->bulk_ep = ep;
            h->bulk_mps = mps;
            h->free_slots = USB_DISP_BULK_XFER_COUNT;
            h->next_slot = 0;
            h->cur = -1;
            h->cur_fill = 0;
            h->announce = true;
            h->attached = true;
        }
        // パイプが確保できない場合も claim は成立させる
        // (掴んだまま放置。コアが VID を見て FAILED 判定できるように attached にはしない)
    } else {
        // アクティブコンフィグに vendor class IF が無い
        // (オートインストール機は Mass Storage だけのコンフィグで列挙される)
        // 他コンフィグの生読みには同期コントロール転送が必要 → メインコンテキストへ
        h->pending_probe = true;
    }
    return true;
}

// 自分が発行したコントロール転送の正常完了 (ISR)
// エラー時は呼ばれず ctrl_err_cb に来る (followup_Transfer / followup_Error 参照)
void USBDispDriver::control(const Transfer_t *transfer) {
    (void)transfer;
    if (!s_ctrl.busy) return;
    if (s_ctrl.abandoned) {   // 待ち手はタイムアウト済み → 静かに破棄
        s_ctrl.abandoned = false;
        s_ctrl.busy = false;
        return;
    }
    s_ctrl.done = true;
}

static void ctrl_err_cb(const Transfer_t *transfer) {
    if (transfer->driver == &s_drv) {
        if (s_ctrl.busy) {
            if (s_ctrl.abandoned) {
                s_ctrl.abandoned = false;
                s_ctrl.busy = false;
            } else {
                s_ctrl.failed = true;
                s_ctrl.done = true;
            }
        }
        return;
    }
    if (s_enum_error_cb) s_enum_error_cb(transfer);   // エニュメ中のエラー
}

void USBDispDriver::disconnect() {
    struct usb_disp_hal *h = &s_hal[0];
    h->attached = false;
    h->pending_probe = false;
    h->announce = false;
    bulk_pipe = NULL;   // パイプ/転送はスタックが破棄する
    s_enum_error_cb = NULL;
    if (s_ctrl.busy) {  // 途中のコントロール転送はコールバック無しで消える
        s_ctrl.failed = true;
        s_ctrl.done = true;
    }
    h->vid = h->pid = 0;
    h->bulk_ep = 0;
    h->bulk_mps = 0;
    h->vdesc_len = 0;
    // cur / cur_fill はメインコンテキストの所有
    // (ここで触ると bulk_write 中の添字と競合する)
    // attached=false を見てメイン側が自分で片づけ、次の claim で全リセットされる
    h->free_slots = USB_DISP_BULK_XFER_COUNT;
}

// バルク転送の正常完了 (ISR): スロットを空きに戻す
static void bulk_cb(const Transfer_t *transfer) {
    struct usb_disp_hal *h = &s_hal[0];
    uint32_t remaining = (transfer->qtd.token >> 16) & 0x7FFF;
    if (remaining) h->bulk_short = h->bulk_short + 1;
    if (h->free_slots < USB_DISP_BULK_XFER_COUNT)
        h->free_slots = h->free_slots + 1;
}

// バルクパイプのエラー (ISR): followup_Error がこのパイプの未完了転送を
// すべて破棄しパイプを再開済み → 書き込み中 (cur) 以外を全部空きに戻す
static void bulk_err_cb(const Transfer_t *transfer) {
    (void)transfer;
    struct usb_disp_hal *h = &s_hal[0];
    h->bulk_err = h->bulk_err + 1;
    h->free_slots =
        (uint8_t)(USB_DISP_BULK_XFER_COUNT - (h->cur >= 0 ? 1 : 0));
}

// ---------------------------------------------------------------
// コントロール転送 (同期化。attached 前の他コンフィグ探索でも使う)
// ---------------------------------------------------------------

static bool ctrl_common(struct usb_disp_hal *h, const uint8_t setup[8],
                        void *data, uint16_t *actual) {
    (void)h;
    if (s_drv.dev() == NULL) return false;
    uint16_t wLength = (uint16_t)(setup[6] | (setup[7] << 8));
    if (wLength > USB_DISP_CTRL_MAX_DATA) return false;
    if (s_ctrl.busy) return false;   // 前回分が未完 (NAK し続ける個体等)
    bool dir_in = (setup[0] & 0x80) != 0;

    memcpy(&s_ctrl.setup, setup, 8);   // setup_t はワイヤ形式と同レイアウト
    if (!dir_in && wLength && data) memcpy(s_ctrl.data, data, wLength);
    s_ctrl.done = false;
    s_ctrl.failed = false;
    s_ctrl.abandoned = false;
    s_ctrl.busy = true;
    if (!s_drv.ctrl_submit(&s_ctrl.setup, wLength ? s_ctrl.data : NULL)) {
        s_ctrl.busy = false;
        return false;
    }

    // 完了は ISR 任せ (スピン待ち)。タイムアウト時は busy を保持したまま
    // abandoned を立て、遅れて来た完了/エラーで ISR 側に解放させる
    uint32_t t0 = millis();
    while (!s_ctrl.done) {
        if (millis() - t0 >= USB_DISP_CTRL_TIMEOUT_MS) {
            NVIC_DISABLE_IRQ(IRQ_USBHS);
            bool done = s_ctrl.done;   // 滑り込み完了の確認
            if (!done) s_ctrl.abandoned = true;
            NVIC_ENABLE_IRQ(IRQ_USBHS);
            if (!done) {
                usb_disp_log("[HAL] ctrl timeout (bReq=%02X)", setup[1]);
                return false;
            }
            break;
        }
    }
    bool ok = !s_ctrl.failed;
    if (ok) {
        if (dir_in && data && wLength) memcpy(data, s_ctrl.data, wLength);
        // 実受信長は取れない (ファイル冒頭の制約参照) → 要求長を返す
        if (actual) *actual = wLength;
    }
    s_ctrl.busy = false;
    return ok;
}

static bool raw_ctrl(struct usb_disp_hal *h, uint8_t bmRequestType,
                     uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                     void *data, uint16_t wLength) {
    uint8_t setup[8];
    setup[0] = bmRequestType;
    setup[1] = bRequest;
    setup[2] = (uint8_t)wValue;
    setup[3] = (uint8_t)(wValue >> 8);
    setup[4] = (uint8_t)wIndex;
    setup[5] = (uint8_t)(wIndex >> 8);
    setup[6] = (uint8_t)wLength;
    setup[7] = (uint8_t)(wLength >> 8);
    return ctrl_common(h, setup, data, NULL);
}

// ---------------------------------------------------------------
//  他コンフィグ探索 (メインコンテキスト。オートインストール機対応)
//    アクティブコンフィグに vendor class IF が無い場合、
//    全コンフィグを生読みして表示 IF を持つものへ
//    SET_CONFIGURATION で切り替え、
//    そのコンフィグのバルク OUT EP へ直接パイプを作る
// ---------------------------------------------------------------

// バルクパイプを作って attached にする (probe 経路の仕上げ)
static void finish_pipe(struct usb_disp_hal *h, uint8_t ep, uint16_t mps) {
    if (!s_drv.make_bulk_pipe_irqsafe(ep, mps)) {
        usb_disp_log("[HAL] pipe alloc failed");
        return;
    }
    h->bulk_ep = ep;
    h->bulk_mps = mps;
    NVIC_DISABLE_IRQ(IRQ_USBHS);
    h->free_slots = USB_DISP_BULK_XFER_COUNT;
    h->next_slot = 0;
    h->cur = -1;
    h->cur_fill = 0;
    h->announce = true;
    h->attached = true;
    NVIC_ENABLE_IRQ(IRQ_USBHS);
}

static void probe_and_finish(struct usb_disp_hal *h) {
    uint8_t ddesc[18];
    if (!raw_ctrl(h, 0x80, 0x06, 0x0100, 0, ddesc, 18)) {
        usb_disp_log("[ENUM] device desc read failed");
        return;
    }
    uint8_t ncfg = ddesc[17];
    usb_disp_log("[ENUM] no vendor IF in active cfg, probing %u configs...",
                 ncfg);
    static uint8_t cfgbuf[256];
    for (uint8_t ci = 0; ci < ncfg && ci < 4; ci++) {
        uint8_t hdr[9];
        if (!raw_ctrl(h, 0x80, 0x06, (uint16_t)(0x0200 + ci), 0, hdr, 9)) {
            usb_disp_log("[ENUM] cfg[%u] read failed", ci);
            continue;
        }
        uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));
        if (total < 9) continue;
        if (total > sizeof(cfgbuf)) total = sizeof(cfgbuf);
        if (!raw_ctrl(h, 0x80, 0x06, (uint16_t)(0x0200 + ci), 0, cfgbuf,
                      total)) {
            usb_disp_log("[ENUM] cfg[%u] full read failed", ci);
            continue;
        }
        uint8_t ep = 0;
        uint16_t mps = 0;
        bool vendor_if = false;
        if (!scan_descriptors(h, cfgbuf, total, &ep, &mps, &vendor_if) ||
            !vendor_if)
            continue;
        uint8_t value = cfgbuf[5];   // bConfigurationValue
        usb_disp_log("[ENUM] vendor IF in cfg[%u] (value=%u), switching",
                     ci, value);
        if (!raw_ctrl(h, 0x00, 0x09, value, 0, NULL, 0)) {
            usb_disp_log("[ENUM] SET_CONFIGURATION(%u) failed", value);
            continue;
        }
        finish_pipe(h, ep, mps);
        return;
    }
    // vendor class IF を持つコンフィグが無い:
    // ESP32 バックエンドと同様、アクティブコンフィグ
    // (index 0 = スタックが選んだもの) の最初のバルク OUT へフォールバックする
    // (表示 IF が class 0xFF でない個体の救済。それも無ければ掴んだまま放置 = コアが FAILED 判定)
    uint8_t hdr[9];
    if (raw_ctrl(h, 0x80, 0x06, 0x0200, 0, hdr, 9)) {
        uint16_t total = (uint16_t)(hdr[2] | (hdr[3] << 8));
        if (total >= 9) {
            if (total > sizeof(cfgbuf)) total = sizeof(cfgbuf);
            uint8_t ep = 0;
            uint16_t mps = 0;
            if (raw_ctrl(h, 0x80, 0x06, 0x0200, 0, cfgbuf, total) &&
                scan_descriptors(h, cfgbuf, total, &ep, &mps, NULL)) {
                usb_disp_log("[ENUM] fallback: non-vendor bulk OUT %02X", ep);
                finish_pipe(h, ep, mps);
                return;
            }
        }
    }
    usb_disp_log("[ENUM] no display config found (%04X:%04X)", h->vid,
                 h->pid);
    // デバイスは掴んだまま attached にしない (切断まで放置)
}

// ---------------------------------------------------------------
// HAL インターフェース実装
// ---------------------------------------------------------------

usb_disp_hal_t *usb_disp_hal_add(const usb_disp_config_t *cfg) {
    // Teensy 4.x: EHCI ホスト 1 系統なので port は 0 のみ。
    // pin_dp/pin_dm は読み飛ばす
    if (!cfg || cfg->port >= 1) {
        usb_disp_log("[HAL] add failed (port %u: Teensy has only one host port)",
                     cfg ? cfg->port : 0);
        return NULL;
    }
    if (s_nhal >= USB_DISP_MAX) return NULL;
    struct usb_disp_hal *h = &s_hal[s_nhal++];
    memset(h, 0, sizeof(*h));
    h->in_use = true;
    h->free_slots = USB_DISP_BULK_XFER_COUNT;
    h->cur = -1;
    return h;
}

void usb_disp_hal_start(void) {
    if (s_started || s_nhal == 0) return;
    s_started = true;
    s_usbhost.begin();
}

// 手動サービスモードは Pico 専用の概念 (Teensy は start と同義)。
// task はスタックのハウスキーピングを回す (呼ばれなくても poll が回す)
static void stretch_debounce(void);
void usb_disp_hal_start_manual(void) { usb_disp_hal_start(); }
void usb_disp_hal_task(void) {
    if (!s_started) return;
    s_usbhost.Task();
    stretch_debounce();
}

//  デバウンス窓の検出とタイマー延長
//    検出条件:
//      接続済み (CCS)・ポート未イネーブル (PE=0)・
//      リセット中でない (PR=0)・
//      GPTIMER0 のロード値が USBHost_t36 既定の 100ms のまま
//      = ISR が張ったデバウンス待ちの最中。
//      このとき一度だけ長い値で再スタートする
//      (再スタート後はロード値が変わるので再発火しない)
//      USBHost_t36 側の既定値が変わった場合は条件が成立せず何もしない (安全側)
static void stretch_debounce(void) {
    uint32_t portsc = USBHS_PORTSC1;
    if ((portsc & USBHS_PORTSC_CCS) && !(portsc & USBHS_PORTSC_PE) &&
        !(portsc & USBHS_PORTSC_PR) && USBHS_GPTIMER0LD == 100000) {
        USBHS_GPTIMER0LD = (uint32_t)USB_DISP_TEENSY_SETTLE_MS * 1000u;
        USBHS_GPTIMER0CTL = USBHS_GPTIMERCTL_RST | USBHS_GPTIMERCTL_RUN;
    }
}

void usb_disp_hal_poll(usb_disp_hal_t *h) {
    if (!s_started) return;
    s_usbhost.Task();   // 転送完了は ISR 駆動。Task はタイマ等の雑務のみ
    stretch_debounce();
    if (h->pending_probe && s_drv.dev() != NULL) {
        h->pending_probe = false;
        probe_and_finish(h);
    }
    if (h->announce && h->attached) {   // claim (ISR) 内で出せなかったログ
        h->announce = false;
        usb_disp_log("[ENUM] VID=%04X PID=%04X speed=%s bulk OUT=%02X mps=%u",
                     h->vid, h->pid,
                     h->speed == 2 ? "HS" : (h->speed == 1 ? "LS" : "FS"),
                     h->bulk_ep, h->bulk_mps);
    }
}

bool usb_disp_hal_attached(usb_disp_hal_t *h) { return h->attached; }
uint16_t usb_disp_hal_vid(usb_disp_hal_t *h) { return h->vid; }
uint16_t usb_disp_hal_pid(usb_disp_hal_t *h) { return h->pid; }

bool usb_disp_hal_ctrl(usb_disp_hal_t *h, const uint8_t setup[8], void *data,
                       uint16_t *actual) {
    if (!h->attached) return false;
    return ctrl_common(h, setup, data, actual);
}

// 空きスロットを1つ確保する (完了 ISR が返すのを待つ)
static bool take_slot(struct usb_disp_hal *h, uint32_t timeout_ms) {
    uint32_t t0 = millis();
    for (;;) {
        NVIC_DISABLE_IRQ(IRQ_USBHS);
        if (h->free_slots > 0) {
            h->free_slots = h->free_slots - 1;
            NVIC_ENABLE_IRQ(IRQ_USBHS);
            return true;
        }
        NVIC_ENABLE_IRQ(IRQ_USBHS);
        if (!h->attached) return false;
        if (millis() - t0 >= timeout_ms) return false;
    }
}

// 書き込み中スロットをサブミットする (呼び出し側で cur >= 0 を保証)
static bool submit_cur(struct usb_disp_hal *h) {
    bool ok = h->attached &&
              s_drv.bulk_submit(s_bulk_buf[h->cur], h->cur_fill);
    if (ok) {
        h->stat_bytes += h->cur_fill;
    } else {
        // スロットは消費されなかった: 空きカウンタとラウンドロビンを戻す
        NVIC_DISABLE_IRQ(IRQ_USBHS);
        if (h->free_slots < USB_DISP_BULK_XFER_COUNT)
        h->free_slots = h->free_slots + 1;
        h->next_slot = (uint8_t)((h->next_slot + USB_DISP_BULK_XFER_COUNT -
                                  1) % USB_DISP_BULK_XFER_COUNT);
        NVIC_ENABLE_IRQ(IRQ_USBHS);
    }
    h->cur = -1;
    h->cur_fill = 0;
    return ok;
}

uint32_t usb_disp_hal_bulk_write(usb_disp_hal_t *h, const void *data,
                                 uint32_t len) {
    if (!h->attached) return 0;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    while (written < len) {
        if (h->cur < 0) {
            if (!take_slot(h, 1000)) break;   // タイムアウト (デバイス消失等)
            if (!h->attached) break;
            // 投入順=完了順なのでラウンドロビンで空きスロットが決まる
            h->cur = (int8_t)h->next_slot;
            h->next_slot = (uint8_t)((h->next_slot + 1) %
                                     USB_DISP_BULK_XFER_COUNT);
            h->cur_fill = 0;
        }
        uint32_t n = USB_DISP_BULK_XFER_SIZE - h->cur_fill;
        if (n > len - written) n = len - written;
        memcpy(&s_bulk_buf[h->cur][h->cur_fill], src + written, n);
        h->cur_fill += n;
        written += n;
        if (h->cur_fill == USB_DISP_BULK_XFER_SIZE) {
            if (!submit_cur(h)) break;
        }
    }
    return written;
}

bool usb_disp_hal_bulk_split(usb_disp_hal_t *h) {
    if (!h->attached) return false;
    if (h->cur >= 0 && h->cur_fill) return submit_cur(h);
    return true;   // 保留なし = 既に境界
}

bool usb_disp_hal_bulk_zlp(usb_disp_hal_t *h) {
    // 非対応。必要チップ (MS913x) は Teensy に組み込まれないため、保留分の送出だけ行う
    if (!h->attached) return false;
    if (h->cur >= 0 && h->cur_fill) return submit_cur(h);
    return true;
}

bool usb_disp_hal_bulk_flush(usb_disp_hal_t *h, uint32_t timeout_ms) {
    if (!h->attached) return false;
    if (h->cur >= 0 && h->cur_fill) {
        if (!submit_cur(h)) return false;
    }
    // 全スロットが空きに戻るまで待つ (完了は ISR 駆動)
    uint32_t t0 = millis();
    while (h->free_slots < USB_DISP_BULK_XFER_COUNT) {
        if (!h->attached) return false;
        if (millis() - t0 >= timeout_ms) return false;
    }
    return true;
}

void usb_disp_hal_request_reenum(usb_disp_hal_t *h) {
    (void)h;
#if defined(ARDUINO_TEENSY41)
    // ルートポートの VBUS を落として入れ直す = 仮想的な抜き差し。
    // Teensy 4.1 のホスト 5V スイッチは GPIO8 bit26 (GPIO_EMC_40) 制御
    // (USBHost_t36 begin() と同じ制御線)
    // 切断→スタックの teardown → 再接続で新しいエニュメレーションが走る
    if (!s_started) return;
    usb_disp_log("[HAL] VBUS power cycle");
    GPIO8_DR_CLEAR = 1u << 26;
    delay(300);
    GPIO8_DR_SET = 1u << 26;
#else
    // Teensy 4.0: ホスト電源は配線依存 (基板上のスイッチが無い) → no-op
#endif
}

uint64_t usb_disp_hal_stat_bytes(usb_disp_hal_t *h) { return h->stat_bytes; }

uint16_t usb_disp_hal_vendor_desc(usb_disp_hal_t *h, void *buf,
                                  uint16_t maxlen) {
    uint16_t n = h->vdesc_len;
    if (n == 0) return 0;
    if (n > maxlen) n = maxlen;
    memcpy(buf, h->vdesc, n);
    return n;
}

// ---- 大容量FBメモリ (シャドウFB 等) ----
// Teensy 4.1 の増設 PSRAM (extmem_malloc)
// 未実装ボード・PSRAM 無しでは NULL が返る
static uint32_t s_fb_used = 0;

void *usb_disp_hal_fb_alloc(uint32_t size) {
    void *p = extmem_malloc(size);
    if (p) s_fb_used += size;
    return p;
}

void usb_disp_hal_fb_free(void *p, uint32_t size) {
    if (!p) return;
    extmem_free(p);
    s_fb_used = (s_fb_used >= size) ? s_fb_used - size : 0;
}

uint32_t usb_disp_hal_fb_used(void) { return s_fb_used; }

#endif  // USB_DISP_PORT_TEENSY

