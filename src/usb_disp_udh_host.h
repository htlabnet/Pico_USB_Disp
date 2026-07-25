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

#ifndef USB_DISP_UDH_HOST_H_
#define USB_DISP_UDH_HOST_H_

#include <stdint.h>
#include <stdbool.h>

#include "hardware/pio.h"

#ifdef __cplusplus
extern "C" {
#endif

// 同時対応ポート数の上限 (1ポート = PIO 1ブロック)
//   RP2350: PIO 3ブロック → 最大3ポート
//   RP2040: PIO 2ブロック → 最大2ポート
// ホストプールは USB_DISP_UDH_MAX_PORTS x 16KB リングを bss 予約するので、
// RAM の少ない RP2040 では小さめが望ましい
#ifndef USB_DISP_UDH_MAX_PORTS
  #if PICO_RP2040
    #define USB_DISP_UDH_MAX_PORTS 2
  #else
    #define USB_DISP_UDH_MAX_PORTS 3
  #endif
#endif

typedef enum {
    USB_DISP_UDH_DISCONNECTED = 0,   // デバイス未接続 (接続検出中)
    USB_DISP_UDH_CONNECTING,         // バスリセット/ウォームアップ中
    USB_DISP_UDH_RUNNING,            // バス動作中 (転送可能)
} usb_disp_udh_state_t;

typedef struct usb_disp_udh_host usb_disp_udh_host_t;

// ---- セットアップ (core0 から core1 起動前に) ----

// ポートを登録する。pio は専用 PIO ブロック。
// pin_dp/pin_dm は D+/D- の GPIO 番号で、隣接していれば順序は自由。
// 戻り値: ホストハンドル
// (プール枯渇・ピン非隣接・PIO プログラム配置失敗はNULL。
// 配置失敗の内訳は usb_disp_udh_bus_init のログに出る)。
usb_disp_udh_host_t *usb_disp_udh_host_add(PIO pio, uint8_t pin_dp, uint8_t pin_dm);

// 登録済みの全ポートを core1 のスケジューラで駆動開始 (一度だけ呼ぶ)。
void usb_disp_udh_host_start(void);

// 手動サービスモード:
// core1 を起動せず、start の代わりに一度呼ぶ。
// 以後 usb_disp_udh_host_task() を専有できるコンテキスト
// (arduino-pico の loop1() 等) から休みなく呼び続けること。
// SOF (1ms) 維持が必要なため、呼び出しの間に長い処理を挟むと接続中のデバイスがサスペンドする。
void usb_disp_udh_host_start_manual(void);

// 手動モードの1周回サービス。
// 初回呼び出しでスケジューラを初期化する
// (呼んだコアがサービスコアになる)
// core1 モード時・start 前は no-op。
void usb_disp_udh_host_task(void);

// ブロッキング待ち中に呼ばれるコールバック (CDC ポンプ等)
void usb_disp_udh_host_set_idle_cb(void (*cb)(void));

// ---- 状態 ----
usb_disp_udh_state_t usb_disp_udh_host_state(usb_disp_udh_host_t *h);
bool usb_disp_udh_host_low_speed(usb_disp_udh_host_t *h);
void usb_disp_udh_host_request_reenum(usb_disp_udh_host_t *h);

// ---- エニュメレーション用 ----
// set_device はバルク/死活プローブの対象デバイス (通常 = DL アダプタ、ハブ監視中はハブ) を指定する
void usb_disp_udh_host_set_device(usb_disp_udh_host_t *h, uint8_t addr, uint8_t ep0_mps);
void usb_disp_udh_host_set_bulk_out(usb_disp_udh_host_t *h, uint8_t ep_addr, uint16_t mps);
// コントロール転送 (set_device で指定したアドレスへ)
bool usb_disp_udh_host_control(usb_disp_udh_host_t *h, const uint8_t setup[8], void *data,
                      uint16_t *actual_len);
// コントロール転送 (任意アドレスへ)
// ハブ (addr) と下流デバイス (別 addr)を同時に扱うハブ対応エニュメレーション用
bool usb_disp_udh_host_control_addr(usb_disp_udh_host_t *h, uint8_t addr, uint8_t ep0_mps,
                           const uint8_t setup[8], void *data,
                           uint16_t *actual_len);

// ---- バルク OUT ストリーム ----
uint32_t usb_disp_udh_host_bulk_write(usb_disp_udh_host_t *h, const void *data, uint32_t len);
bool usb_disp_udh_host_bulk_flush(usb_disp_udh_host_t *h, uint32_t timeout_ms);
uint32_t usb_disp_udh_host_bulk_pending(usb_disp_udh_host_t *h);

// ---- 診断 ----

#define USB_DISP_UDH_DIAG_RISE_RUNS 8

typedef struct {
    uint32_t total, dp_hi, dm_hi, se1;                  // 生ラインステート統計
    int8_t rise_idx[USB_DISP_UDH_DIAG_RISE_RUNS];       // 放電後 High までのサンプル番号
                                                        // (-250ns/step, -1=窓内に無し)
    uint8_t rise_line[USB_DISP_UDH_DIAG_RISE_RUNS];     // 立ち上がった線 (usb_disp_udh_line_t)
} usb_disp_udh_diag_t;

// 接続検出のデバッグ用ライン診断 (core0 から呼ぶ、150ms ブロック)
// バスを一旦切断状態に落として検出を保留し、
// 生レベル統計と放電後立ち上がりプロファイルを測る。
// 終了後は通常の検出に戻る。
void usb_disp_udh_host_line_diag(usb_disp_udh_host_t *h, usb_disp_udh_diag_t *out);

// 状態遷移カウンタ (接続シーケンスのどこで落ちるかの切り分け用)
typedef struct {
    uint32_t detect_ok;    // 接続検出成立 → バスリセット開始
    uint32_t warmup_fail;  // ウォームアップ中に bus_ok 喪失 → 検出へ戻り
    uint32_t run_enter;    // RUN (転送可能) 到達
    uint32_t sof_se0;      // SOF 送信前の SE0 二度読み成立 (切断扱い)
} usb_disp_udh_dbg_counters_t;
void usb_disp_udh_host_dbg_counters(usb_disp_udh_host_t *h, usb_disp_udh_dbg_counters_t *out);

// ---- 統計 ----
uint64_t usb_disp_udh_host_stat_bytes(usb_disp_udh_host_t *h);
uint32_t usb_disp_udh_host_stat_naks(usb_disp_udh_host_t *h);
uint32_t usb_disp_udh_host_stat_errors(usb_disp_udh_host_t *h);
uint32_t usb_disp_udh_host_stat_dead_resets(usb_disp_udh_host_t *h);
uint32_t usb_disp_udh_host_frame(usb_disp_udh_host_t *h);

#ifdef __cplusplus
}
#endif

#endif // USB_DISP_UDH_HOST_H_

