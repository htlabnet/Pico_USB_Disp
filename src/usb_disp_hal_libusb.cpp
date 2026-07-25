//
// ######################################################################
//
//    usb_disp_hal_libusb - PC (Windows/macOS/Linux/BSD) バックエンド
//
//    libusb-1.0 の同期 API で usb_disp_hal を実装する
//
//  ドライバ準備
//    - Windows: Zadig で対象デバイスに WinUSB をバインドする
//      (メーカー公式ドライバが当たっている場合は置換)
//    - Linux: udl/udlfb カーネルドライバは auto_detach で自動的に
//      剥がす。一般ユーザー権限には udev ルールが必要
//      (VID ごとに1行。DisplayLink の例):
//      SUBSYSTEM=="usb", ATTR{idVendor}=="17e9", MODE="0666"
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include "usb_disp_hal.h"

#if USB_DISP_PORT_LIBUSB

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// Linux のシステムインストールは <libusb-1.0/libusb.h>、
// Windows のプリビルトパッケージは <libusb.h> 直置き
#if defined(__has_include)
#  if __has_include(<libusb-1.0/libusb.h>)
#    include <libusb-1.0/libusb.h>
#  else
#    include <libusb.h>
#  endif
#else
#  include <libusb-1.0/libusb.h>
#endif

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

// バッファ (HS なので大きめ)
#define USB_DISP_LIBUSB_BULK_BUF_SIZE (256 * 1024)
#define USB_DISP_LIBUSB_CTRL_TIMEOUT_MS 1000
#define USB_DISP_LIBUSB_BULK_TIMEOUT_MS 5000
#define USB_DISP_LIBUSB_SCAN_INTERVAL_MS 500

struct usb_disp_hal {
    bool in_use;
    libusb_device_handle *dev;
    bool attached;
    uint16_t vid, pid;
    uint8_t bulk_ep;
    int16_t iface;      // クレーム中のIF番号 (-1 = 未クレーム)
    uint8_t port;       // 担当する「対応デバイス発見順リスト」の番号
    uint8_t bus, addr;  // 掴んでいる個体の識別 (多重割り当て防止)
    uint32_t last_scan_ms;

    uint8_t bulk_buf[USB_DISP_LIBUSB_BULK_BUF_SIZE];
    uint32_t bulk_fill;
    uint64_t stat_bytes;  // 累積バルク送信バイト数

    uint8_t vdesc[64];    // config 内 0x5F ベンダーディスクリプタ
    uint8_t vdesc_len;
};

static struct usb_disp_hal s_hal[USB_DISP_MAX];
static uint8_t s_nhal = 0;
static libusb_context *s_ctx = NULL;

// コンフィグに vendor class (0xFF) のインターフェースがあるか
static bool cfg_has_vendor_if(const struct libusb_config_descriptor *c) {
    for (uint8_t fi = 0; fi < c->bNumInterfaces; fi++) {
        if (c->interface[fi].num_altsetting >= 1 &&
            c->interface[fi].altsetting[0].bInterfaceClass == 0xFF)
            return true;
    }
    return false;
}

// この個体 (bus/addr) を既に他のインスタンスが掴んでいるか
static bool device_in_use(uint8_t bus, uint8_t addr) {
    for (uint8_t i = 0; i < s_nhal; i++) {
        if (s_hal[i].in_use && s_hal[i].attached && s_hal[i].bus == bus &&
            s_hal[i].addr == addr) {
            return true;
        }
    }
    return false;
}

uint32_t usb_disp_hal_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount64();
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
#endif
}

// ---------------------------------------------------------------
// デバイス接続/切断
// ---------------------------------------------------------------

static void device_close(struct usb_disp_hal *h) {
    h->attached = false;
    if (h->dev) {
        if (h->iface >= 0) libusb_release_interface(h->dev, h->iface);
        libusb_close(h->dev);
        h->dev = NULL;
    }
    h->iface = -1;
    h->vid = h->pid = 0;
    h->bulk_ep = 0;
    h->bulk_fill = 0;
    h->bus = h->addr = 0;
    h->vdesc_len = 0;
}

// 対応デバイス (usb_disp_supported_device = 全プロトコルの match) を列挙し、
// 発見順リストの h->port 番目の個体を開いてバルク OUT EP を構成する
// (port=0 が列挙順の1台目。チップ種別は問わず同じリストに並ぶ)。
// 対象が未接続 / 他インスタンス使用中 / open 失敗なら今回は諦めて
// 次回スキャンに任せる。
static bool device_scan(struct usb_disp_hal *h) {
    libusb_device **list = NULL;
    ssize_t n = libusb_get_device_list(s_ctx, &list);
    if (n < 0) return false;

    bool ok = false;
    int16_t match_idx = -1;   // 対応デバイスに出会うたびに進める発見順番号
    for (ssize_t i = 0; i < n && !ok; i++) {
        struct libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;
        if (!usb_disp_supported_device(dd.idVendor, dd.idProduct)) continue;
        if (++match_idx != h->port) continue;  // 担当リスト番号の個体のみ

        uint8_t bus = libusb_get_bus_number(list[i]);
        uint8_t addr = libusb_get_device_address(list[i]);
        if (device_in_use(bus, addr)) break;  // 抜き差しで番号が詰まった直後

        libusb_device_handle *dev = NULL;
        if (libusb_open(list[i], &dev) != 0) {
            usb_disp_log("[HAL] found %04X:%04X but open failed "
                         "(driver? permission?)",
                         dd.idVendor, dd.idProduct);
            continue;
        }
        libusb_set_auto_detach_kernel_driver(dev, 1);  // Linux: udl を剥がす

        // アクティブコンフィグからバルク OUT EP を探す
        struct libusb_config_descriptor *cfg = NULL;
        if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) {
            // 未コンフィグの可能性 → config 1 を設定して再取得
            libusb_set_configuration(dev, 1);
            if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) {
                libusb_close(dev);
                continue;
            }
        }

        // オートインストール機対応 (GX-DVI/U2AI): 
        // アクティブコンフィグに vendor class IF が無く複数コンフィグを持つ場合、
        // 表示 IF を持つコンフィグへ SET_CONFIGURATION で切り替える。
        // 注意: Windows の WinUSB.sys はコンフィグ変更をサポートしない →
        // Zadig で libusbK または libusb0 (libusb-win32) をバインドすること。
        // Linux はカーネルドライバ (usb-storage) が掴んでいると
        // BUSY になるので先に IF0 をデタッチする
        if (!cfg_has_vendor_if(cfg) && dd.bNumConfigurations > 1) {
            for (uint8_t ci = 0; ci < dd.bNumConfigurations; ci++) {
                struct libusb_config_descriptor *c2 = NULL;
                if (libusb_get_config_descriptor(list[i], ci, &c2) != 0)
                    continue;
                bool vendor = cfg_has_vendor_if(c2);
                uint8_t value = c2->bConfigurationValue;
                libusb_free_config_descriptor(c2);
                if (!vendor) continue;
                libusb_detach_kernel_driver(dev, 0);  // Linux: usb-storage 剥がし
                int sr = libusb_set_configuration(dev, value);
                if (sr == 0) {
                    usb_disp_log("[ENUM] switched to config %u (vendor IF)",
                                 value);
                    libusb_free_config_descriptor(cfg);
                    cfg = NULL;
                    if (libusb_get_active_config_descriptor(list[i], &cfg) !=
                        0) {
                        libusb_close(dev);
                        break;
                    }
                } else {
                    usb_disp_log("[HAL] set_configuration(%u) failed (%d)."
                                 " WinUSB cannot change configuration."
                                 " Use Zadig to bind libusbK or libusb0",
                                 value, sr);
                }
                break;
            }
            if (cfg == NULL) continue;  // 再取得失敗 (上で close 済み)
        }
        // バルク OUT EP を探す (alt 0 のみ、vendor class 0xFF の IF を優先。
        // オートインストール用 Mass Storage 等を誤って掴まないように)。
        // ついでに IF extras の 0x5F ベンダーディスクリプタも捕捉
        // (GET_DESCRIPTOR 0x5F に応答しない個体のチップ判別フォールバック)
        uint8_t ep = 0;
        int16_t iface = -1;
        bool got_vendor_ep = false;
        h->vdesc_len = 0;
        for (uint8_t fi = 0; fi < cfg->bNumInterfaces; fi++) {
            if (cfg->interface[fi].num_altsetting < 1) continue;
            const struct libusb_interface_descriptor *id =
                &cfg->interface[fi].altsetting[0];
            bool vendor_if = (id->bInterfaceClass == 0xFF);
            if (h->vdesc_len == 0 && id->extra && id->extra_length >= 2 &&
                id->extra[1] == 0x5F) {
                int n = id->extra[0];   // extra_length (int) と比較するため int
                if (n > id->extra_length) n = id->extra_length;
                if (n > (int)sizeof(h->vdesc)) n = (int)sizeof(h->vdesc);
                memcpy(h->vdesc, id->extra, (size_t)n);
                h->vdesc_len = (uint8_t)n;
            }
            for (uint8_t ei = 0; ei < id->bNumEndpoints; ei++) {
                const struct libusb_endpoint_descriptor *ed = &id->endpoint[ei];
                if ((ed->bmAttributes & 0x03) == LIBUSB_TRANSFER_TYPE_BULK &&
                    (ed->bEndpointAddress & 0x80) == 0 &&
                    (ep == 0 || (vendor_if && !got_vendor_ep))) {
                    ep = ed->bEndpointAddress;
                    iface = id->bInterfaceNumber;
                    got_vendor_ep = vendor_if;
                    break;
                }
            }
        }
        libusb_free_config_descriptor(cfg);

        if (ep == 0) {
            usb_disp_log("[ENUM] %04X:%04X no bulk OUT endpoint", dd.idVendor,
                         dd.idProduct);
            libusb_close(dev);
            continue;
        }
        if (libusb_claim_interface(dev, iface) != 0) {
            usb_disp_log("[HAL] claim_interface failed (check that WinUSB is"
                         " bound with Zadig)");
            libusb_close(dev);
            continue;
        }

        int speed = libusb_get_device_speed(list[i]);
        static const char *spd[] = {"?", "LOW", "FULL", "HIGH", "SUPER",
                                    "SUPER+"};
        h->dev = dev;
        h->vid = dd.idVendor;
        h->pid = dd.idProduct;
        h->bulk_ep = ep;
        h->iface = iface;
        h->bus = bus;
        h->addr = addr;
        h->bulk_fill = 0;
        h->attached = true;
        usb_disp_log("[ENUM] port%u VID=%04X PID=%04X bus=%u addr=%u "
                     "bulk OUT=%02X if=%d speed=%s",
                     h->port, h->vid, h->pid, bus, addr, ep, iface,
                     (speed >= 0 && speed <= 5) ? spd[speed] : "?");
        ok = true;
    }
    libusb_free_device_list(list, 1);
    return ok;
}

// ---------------------------------------------------------------
// HAL インターフェース実装
// ---------------------------------------------------------------

usb_disp_hal_t *usb_disp_hal_add(const usb_disp_config_t *cfg) {
    // PC: cfg->port = 対応デバイス発見順リストの番号 (0 = 列挙順の1台目)。
    // pin_dp/pin_dm は読み飛ばす
    if (!cfg || s_nhal >= USB_DISP_MAX) return NULL;
    struct usb_disp_hal *h = &s_hal[s_nhal++];
    memset(h, 0, sizeof(*h));
    h->in_use = true;
    h->iface = -1;
    h->port = cfg->port;
    return h;
}

void usb_disp_hal_start(void) {
    if (s_ctx == NULL) libusb_init(&s_ctx);
}

// 手動サービスモードは Pico 専用の概念 (PC は start と同義 / task は no-op)
void usb_disp_hal_start_manual(void) { usb_disp_hal_start(); }
void usb_disp_hal_task(void) {}

void usb_disp_hal_poll(usb_disp_hal_t *h) {
    if (h->attached || s_ctx == NULL) return;
    uint32_t now = usb_disp_hal_ms();
    if (now - h->last_scan_ms < USB_DISP_LIBUSB_SCAN_INTERVAL_MS) return;
    h->last_scan_ms = now;
    device_scan(h);
}

bool usb_disp_hal_attached(usb_disp_hal_t *h) { return h->attached; }
uint16_t usb_disp_hal_vid(usb_disp_hal_t *h) { return h->vid; }
uint16_t usb_disp_hal_pid(usb_disp_hal_t *h) { return h->pid; }

bool usb_disp_hal_ctrl(usb_disp_hal_t *h, const uint8_t setup[8], void *data,
                       uint16_t *actual) {
    if (!h->attached) return false;
    uint8_t bmRequestType = setup[0];
    uint8_t bRequest = setup[1];
    uint16_t wValue = (uint16_t)(setup[2] | (setup[3] << 8));
    uint16_t wIndex = (uint16_t)(setup[4] | (setup[5] << 8));
    uint16_t wLength = (uint16_t)(setup[6] | (setup[7] << 8));

    int r = libusb_control_transfer(h->dev, bmRequestType, bRequest, wValue,
                                    wIndex, (unsigned char *)data, wLength,
                                    USB_DISP_LIBUSB_CTRL_TIMEOUT_MS);
    if (r < 0) {
        if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO) {
            usb_disp_log("[HAL] device lost (ctrl)");
            device_close(h);
        }
        return false;
    }
    if (actual) *actual = (uint16_t)r;
    return true;
}

// バッファに溜まった分を送信する
static bool bulk_drain(struct usb_disp_hal *h) {
    uint32_t off = 0;
    while (off < h->bulk_fill) {
        int sent = 0;
        int r = libusb_bulk_transfer(h->dev, h->bulk_ep, h->bulk_buf + off,
                                     (int)(h->bulk_fill - off), &sent,
                                     USB_DISP_LIBUSB_BULK_TIMEOUT_MS);
        if (r != 0) {
            usb_disp_log("[HAL] bulk error %d", r);
            if (r == LIBUSB_ERROR_NO_DEVICE || r == LIBUSB_ERROR_IO ||
                r == LIBUSB_ERROR_PIPE) {
                device_close(h);
            }
            h->bulk_fill = 0;
            return false;
        }
        off += (uint32_t)sent;
        h->stat_bytes += (uint32_t)sent;
    }
    h->bulk_fill = 0;
    return true;
}

uint32_t usb_disp_hal_bulk_write(usb_disp_hal_t *h, const void *data,
                                 uint32_t len) {
    if (!h->attached) return 0;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    while (written < len) {
        uint32_t n = USB_DISP_LIBUSB_BULK_BUF_SIZE - h->bulk_fill;
        if (n > len - written) n = len - written;
        memcpy(h->bulk_buf + h->bulk_fill, src + written, n);
        h->bulk_fill += n;
        written += n;
        if (h->bulk_fill == USB_DISP_LIBUSB_BULK_BUF_SIZE) {
            if (!bulk_drain(h)) return written;
        }
    }
    return written;
}

bool usb_disp_hal_bulk_flush(usb_disp_hal_t *h, uint32_t timeout_ms) {
    (void)timeout_ms;  // 同期送信なので drain 完了 = 送信完了
    if (!h->attached) return false;
    return bulk_drain(h);
}

bool usb_disp_hal_bulk_split(usb_disp_hal_t *h) {
    if (!h->attached) return false;
    return h->bulk_fill ? bulk_drain(h) : true;
}

bool usb_disp_hal_bulk_zlp(usb_disp_hal_t *h) {
    if (!h->attached) return false;
    if (h->bulk_fill && !bulk_drain(h)) return false;
    int sent = 0;
    uint8_t dummy = 0;
    return libusb_bulk_transfer(h->dev, h->bulk_ep, &dummy, 0, &sent,
                                USB_DISP_LIBUSB_BULK_TIMEOUT_MS) == 0;
}

void usb_disp_hal_request_reenum(usb_disp_hal_t *h) {
    // 掴んでいるデバイスを手放して次の poll で再スキャン
    device_close(h);
    h->last_scan_ms = 0;
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

// ---- 大容量FBメモリ: PC は普通のヒープ ----
static uint32_t s_fb_used = 0;

void *usb_disp_hal_fb_alloc(uint32_t size) {
    void *p = malloc(size);
    if (p) s_fb_used += size;
    return p;
}
void usb_disp_hal_fb_free(void *p, uint32_t size) {
    if (!p) return;
    free(p);
    s_fb_used = (s_fb_used >= size) ? s_fb_used - size : 0;
}
uint32_t usb_disp_hal_fb_used(void) { return s_fb_used; }

#endif  // USB_DISP_PORT_LIBUSB

