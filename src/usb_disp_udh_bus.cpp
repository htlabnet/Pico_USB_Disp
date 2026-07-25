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

// Pico (RP2040/RP2350) 専用実装
#include "usb_disp.h"

#if USB_DISP_PORT_PICO

#include <string.h>

#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"
#include "hardware/structs/pads_bank0.h"
#include "hardware/structs/bus_ctrl.h"
#include "hardware/regs/sysinfo.h"

#include "pio/usb_tx.pio.h"  // 一次ソースは pio/usb_tx.pio
#include "pio/usb_rx.pio.h"  // 一次ソースは pio/usb_rx.pio

#include "usb_disp_udh_encode.h"
#include "usb_disp_udh_bus.h"

// pio/usb_tx.pio / usb_rx.pio 由来の pioasm 生成シンボル
#define USB_DISP_UDH_IRQ_TX_EOP_MASK   (1u << IRQ_TX_EOP)
#define USB_DISP_UDH_IRQ_RX_COMP_MASK  (1u << IRQ_RX_EOP)
#define USB_DISP_UDH_IRQ_RX_START_MASK (1u << IRQ_RX_START)
#define USB_DISP_UDH_IRQ_RX_ALL_MASK                                   \
    ((1u << IRQ_RX_EOP) | (1u << IRQ_RX_BS_ERR) | (1u << IRQ_RX_START) | \
     (1u << DECODER_TRIGGER))

// 事前エンコード済み ACK パケット (全バス共通)
static uint8_t s_ack_enc[8];
static uint8_t s_ack_enc_len;
static bool s_global_ready = false;

// クロック依存の固定遅延
// usb_disp_udh_bus_reconfigure() で毎回更新、
// 設定クロックが何であっても実時間が一定になる。
// 値は precompute しておき、ホット経路 (per-SOF 等) では
// clock_get_hz を呼ばずにこの値を読むだけにする (フラッシュアクセス回避)
static uint32_t s_cyc_sample = 48;  // -250ns: E9 放電読みのサンプル間隔
static uint32_t s_cyc_drain = 8;    // -40ns : E9 入力段ドレイン

// ---------------------------------------------------------------
// CRC (Pico-PIO-USB usb_crc.c より、ステートレス・全バス共有)
// ---------------------------------------------------------------

static const uint8_t __not_in_flash("usb_disp_udh_crc5tbl") s_crc5_tbl[32] = {
    0x00, 0x0b, 0x16, 0x1d, 0x05, 0x0e, 0x13, 0x18, 0x0a, 0x01,
    0x1c, 0x17, 0x0f, 0x04, 0x19, 0x12, 0x14, 0x1f, 0x02, 0x09,
    0x11, 0x1a, 0x07, 0x0c, 0x1e, 0x15, 0x08, 0x03, 0x1b, 0x10,
    0x0d, 0x06,
};

uint8_t __not_in_flash_func(usb_disp_udh_crc5)(uint16_t data11) {
    uint16_t data = data11 ^ 0x1F;
    const uint8_t lsb = (data >> 1) & 0x1F;
    const uint8_t msb = (data >> 6) & 0x1F;
    uint8_t crc = (data & 1) ? 0x14 : 0x00;
    crc = s_crc5_tbl[(lsb ^ crc)];
    crc = s_crc5_tbl[(msb ^ crc)];
    return (uint8_t)(crc ^ 0x1F);
}

static uint16_t __not_in_flash("usb_disp_udh_crc") s_crc16_tbl[256];

static void crc16_tbl_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t crc = i;
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : (crc >> 1);
        }
        s_crc16_tbl[i] = (uint16_t)crc;
    }
}

uint16_t __not_in_flash_func(usb_disp_udh_crc16)(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc = (uint16_t)((crc >> 8) ^ s_crc16_tbl[(crc ^ data[i]) & 0xFF]);
    }
    return crc ^ 0xFFFF;
}

static inline uint16_t crc16_update(uint16_t crc, uint8_t byte) {
    return (uint16_t)((crc >> 8) ^ s_crc16_tbl[(crc ^ byte) & 0xFF]);
}

// ---------------------------------------------------------------
// 初期化
// ---------------------------------------------------------------

void usb_disp_udh_bus_global_init(void) {
    if (s_global_ready) {
        return;
    }
    crc16_tbl_init();
    usb_disp_udh_encode_init();

    // バスファブリック上で DMA を優先 (TX FIFO を切らさない)
    bus_ctrl_hw->priority =
        BUSCTRL_BUS_PRIORITY_DMA_R_BITS | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;

    uint8_t ack[2] = {USB_DISP_UDH_USB_SYNC, USB_DISP_UDH_PID_ACK};
    s_ack_enc_len = usb_disp_udh_encode_tx(ack, 2, s_ack_enc);

    s_global_ready = true;
}

// 現在の sys クロックを取得し、PIO ビットクロック分周と固定遅延を再算出する。
// 初期化時と再構成 (接続/リセット) の度に呼ぶ → クロック設定が変わっても追従する。
static void usb_disp_udh_bus_refresh_clock(usb_disp_udh_bus_t *b) {
    uint32_t hz = clock_get_hz(clk_sys);

    // PIO の TX(48MHz)/EOP検出(96MHz) 分周を実クロックから再設定。
    // NRZI デコーダ(sm_rx) は最速動作 (div=1) なので調整不要。
    pio_sm_set_clkdiv(b->pio, b->sm_tx, (float)hz / 48000000.0f);
    pio_sm_set_clkdiv(b->pio, b->sm_eop, (float)hz / 96000000.0f);

    // 固定遅延を実クロックからサイクル数に換算 (ceil, 最低1)
    s_cyc_sample = (uint32_t)(((uint64_t)hz * 250u + 999999999u) / 1000000000u);
    s_cyc_drain = (uint32_t)(((uint64_t)hz * 40u + 999999999u) / 1000000000u);
    if (!s_cyc_sample) s_cyc_sample = 1;
    if (!s_cyc_drain) s_cyc_drain = 1;
}

bool usb_disp_udh_bus_init(usb_disp_udh_bus_t *b, PIO pio, uint8_t pin_dp, uint8_t pin_dm) {
    usb_disp_udh_bus_global_init();

    memset(b, 0, sizeof(*b));
    b->pio = pio;
    b->pin_dp = pin_dp;
    b->pin_dm = pin_dm;
    // D+ が下の GPIO なら DPDM、D- が下なら DMDP の TX プログラムを使う
    // (2ビットシンボルのピン割当が逆になるため命令列が異なる。
    //  RX/EOP は D+/D- を個別指定するのでピン順に依存しない)
    const bool dpdm = pin_dp < pin_dm;

    b->sm_tx = 0;
    b->sm_rx = 1;
    b->sm_eop = 2;
    pio_sm_claim(pio, b->sm_tx);
    pio_sm_claim(pio, b->sm_rx);
    pio_sm_claim(pio, b->sm_eop);

    // TX DMA チャネル
    b->tx_ch = (uint8_t)dma_claim_unused_channel(true);
    dma_channel_config conf = dma_channel_get_default_config(b->tx_ch);
    channel_config_set_read_increment(&conf, true);
    channel_config_set_write_increment(&conf, false);
    channel_config_set_transfer_data_size(&conf, DMA_SIZE_8);
    channel_config_set_dreq(&conf, pio_get_dreq(pio, b->sm_tx, true));
    channel_config_set_high_priority(&conf, true);
    dma_channel_set_config(b->tx_ch, &conf, false);
    dma_channel_set_write_addr(b->tx_ch, &pio->txf[b->sm_tx], false);

    // TX プログラムはエンコード済みシンボルが絶対アドレス 0..3 を指すため
    // 必ずオフセット 0 に配置する (各ポートは専用 PIO ブロックを持つ)
    const pio_program_t *tx_prog =
        dpdm ? &usb_tx_dpdm_program : &usb_tx_dmdp_program;
    if (!pio_can_add_program_at_offset(pio, tx_prog, 0)) {
        usb_disp_log("[BUS] PIO TX program load refused "
                     "(no space or pio_version mismatch)");
        return false;
    }
    pio_add_program_at_offset(pio, tx_prog, 0);
    b->offset_tx = 0;
    usb_tx_fs_program_init(pio, b->sm_tx, b->offset_tx, b->pin_dp, b->pin_dm);

    uint32_t sideset_fj_lk = pio_encode_sideset(
        2, dpdm ? usb_tx_dpdm_FJ_LK : usb_tx_dmdp_FJ_LK);
    b->tx_start_instr =
        (uint16_t)(pio_encode_jmp(b->offset_tx + 4u) | sideset_fj_lk);
    b->tx_reset_instr =
        (uint16_t)(pio_encode_jmp(b->offset_tx + 2u) | sideset_fj_lk);

    // RX: NRZI デコーダ
    if (!pio_can_add_program(pio, &usb_nrzi_decoder_program)) {
        usb_disp_log("[BUS] PIO RX program load refused "
                     "(no space or pio_version mismatch)");
        return false;
    }
    b->offset_rx = (uint8_t)pio_add_program(pio, &usb_nrzi_decoder_program);
    usb_rx_fs_program_init(pio, b->sm_rx, b->offset_rx, b->pin_dp, b->pin_dm,
                           -1);
    b->rx_reset_instr = (uint16_t)pio_encode_jmp(b->offset_rx);
    b->rx_reset_instr2 = (uint16_t)pio_encode_set(pio_x, 0);

    // RX: エッジ/EOP 検出
    if (!pio_can_add_program(pio, &usb_edge_detector_program)) {
        usb_disp_log("[BUS] PIO EOP program load refused "
                     "(no space or pio_version mismatch)");
        return false;
    }
    b->offset_eop = (uint8_t)pio_add_program(pio, &usb_edge_detector_program);
    eop_detect_fs_program_init(pio, b->sm_eop, b->offset_eop, b->pin_dp,
                               b->pin_dm, true, -1);

    // ピン駆動設定
    gpio_set_slew_rate(b->pin_dp, GPIO_SLEW_RATE_FAST);
    gpio_set_slew_rate(b->pin_dm, GPIO_SLEW_RATE_FAST);
    gpio_set_drive_strength(b->pin_dp, GPIO_DRIVE_STRENGTH_12MA);
    gpio_set_drive_strength(b->pin_dm, GPIO_DRIVE_STRENGTH_12MA);

    usb_disp_udh_bus_reconfigure(b);
    return true;
}

void usb_disp_udh_bus_reconfigure(usb_disp_udh_bus_t *b) {
    // 現在のクロックへ追従 (分周と固定遅延を再算出)
    usb_disp_udh_bus_refresh_clock(b);
    pio_sm_clear_fifos(b->pio, b->sm_tx);
    pio_sm_exec(b->pio, b->sm_tx, b->tx_reset_instr);
}

// ---------------------------------------------------------------
// ラインステート
// ---------------------------------------------------------------

usb_disp_udh_line_t __not_in_flash_func(usb_disp_udh_bus_line_state)(usb_disp_udh_bus_t *b) {
#ifdef PICO_RP2350
    // RP2350-E9 エラッタ (rev A2/B0 まで): 内部プルダウンでの入力ラッチ。
    uint32_t const chip_id =
        *((io_ro_32 *)(SYSINFO_BASE + SYSINFO_CHIP_ID_OFFSET));
    uint32_t const chip_version =
        (chip_id & SYSINFO_CHIP_ID_REVISION_BITS) >>
        SYSINFO_CHIP_ID_REVISION_LSB;
    if (chip_version <= 2) {
        hw_clear_bits(&pads_bank0_hw->io[b->pin_dp], PADS_BANK0_GPIO0_IE_BITS);
        hw_clear_bits(&pads_bank0_hw->io[b->pin_dm], PADS_BANK0_GPIO0_IE_BITS);
        // 実クロックから算出した -40ns ドレイン
        // (busy_wait_at_least_cycles は static inline アセンブリなので RAM 実行のまま。
        // s_cyc_drain は reconfigure 時に更新済み)
        busy_wait_at_least_cycles(s_cyc_drain);
        hw_set_bits(&pads_bank0_hw->io[b->pin_dp], PADS_BANK0_GPIO0_IE_BITS);
        hw_set_bits(&pads_bank0_hw->io[b->pin_dm], PADS_BANK0_GPIO0_IE_BITS);
    }
#endif
    uint8_t dp = gpio_get(b->pin_dp) ? 0 : 1;  // 入力は INVERT 済み
    uint8_t dm = gpio_get(b->pin_dm) ? 0 : 1;
    return (usb_disp_udh_line_t)((dm << 1) | dp);
}

// RP2350-E9 エラッタ対策 : 放電付きラインステート読み
// SE0 を駆動して放電した後、解放直後 -2µs の窓で「先に立ち上がった側」を読む。
// 本物のデバイスのプルアップは窓内に復帰し、E9 ラッチのリーク再充電はまだ届かない。
// バスに SE0 グリッチが出るため接続確立前 (デバウンスループ) 専用。
// 強駆動での線容量放電は <100ns で完了するので 1µs で十分。
usb_disp_udh_line_t __not_in_flash_func(usb_disp_udh_bus_line_state_forced)(usb_disp_udh_bus_t *b) {
    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_LOW);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_LOW);
    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_HIGH);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_HIGH);
    busy_wait_us(1);

    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_NORMAL);

    usb_disp_udh_line_t result = USB_DISP_UDH_LINE_SE0;
    for (uint8_t i = 0; i < 8; i++) {
        busy_wait_at_least_cycles(s_cyc_sample);  // 実クロック換算の -250ns
        uint8_t dp = gpio_get(b->pin_dp) ? 0 : 1;
        uint8_t dm = gpio_get(b->pin_dm) ? 0 : 1;
        if (dp || dm) {
            result = (usb_disp_udh_line_t)((dm << 1) | dp);
            break;
        }
    }

    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_NORMAL);
    return result;
}

// 放電後の立ち上がりプロファイル
// line_state_forced と同じ放電を行い、最初に High が読めたサンプル番号
// (-250ns/step) を返す。窓内に立ち上がらなければ -1。
int8_t __not_in_flash_func(usb_disp_udh_bus_line_rise_profile)(usb_disp_udh_bus_t *b,
                                                   usb_disp_udh_line_t *line_out,
                                                   uint8_t max_samples) {
    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_LOW);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_LOW);
    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_HIGH);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_HIGH);
    busy_wait_us(1);  // line_state_forced と同じ (2.5µs 未満、リセット誤認防止)

    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_NORMAL);

    int8_t idx = -1;
    usb_disp_udh_line_t line = USB_DISP_UDH_LINE_SE0;
    for (uint8_t i = 0; i < max_samples; i++) {
        busy_wait_at_least_cycles(s_cyc_sample);  // -250ns/step
        uint8_t dp = gpio_get(b->pin_dp) ? 0 : 1;
        uint8_t dm = gpio_get(b->pin_dm) ? 0 : 1;
        if (dp || dm) {
            line = (usb_disp_udh_line_t)((dm << 1) | dp);
            idx = (int8_t)i;
            break;
        }
    }

    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_NORMAL);
    if (line_out) *line_out = line;
    return idx;
}

// パッドの叩き直し
void usb_disp_udh_bus_pad_kick(usb_disp_udh_bus_t *b) {
    // TX SM を解放位置へ強制ジャンプ
    // (ピン駆動のまま停止していると自分の駆動で SE1 が読め続け接続検出が成立しない)
    pio_sm_clear_fifos(b->pio, b->sm_tx);
    pio_sm_exec(b->pio, b->sm_tx, b->tx_reset_instr);

    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_LOW);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_LOW);
    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_HIGH);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_HIGH);
    busy_wait_us(100);
    gpio_set_oeover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_oeover(b->pin_dm, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(b->pin_dp, GPIO_OVERRIDE_NORMAL);
    gpio_set_outover(b->pin_dm, GPIO_OVERRIDE_NORMAL);

    gpio_pull_down(b->pin_dp);
    gpio_pull_down(b->pin_dm);

    hw_clear_bits(&pads_bank0_hw->io[b->pin_dp], PADS_BANK0_GPIO0_IE_BITS);
    hw_clear_bits(&pads_bank0_hw->io[b->pin_dm], PADS_BANK0_GPIO0_IE_BITS);
    busy_wait_us(5);
    hw_set_bits(&pads_bank0_hw->io[b->pin_dp], PADS_BANK0_GPIO0_IE_BITS);
    hw_set_bits(&pads_bank0_hw->io[b->pin_dm], PADS_BANK0_GPIO0_IE_BITS);
}

// ---------------------------------------------------------------
// TX
// ---------------------------------------------------------------

void __not_in_flash_func(usb_disp_udh_bus_tx_start)(usb_disp_udh_bus_t *b, uint8_t *encoded,
                                           uint16_t len) {
    uint32_t const stall_mask = 1u << (PIO_FDEBUG_TXSTALL_LSB + b->sm_tx);

    // 前パケットの末尾シンボル排出完了を保証
    b->pio->fdebug = stall_mask;
    while ((b->pio->fdebug & stall_mask) == 0) {
        continue;
    }

    pio_sm_exec(b->pio, b->sm_tx, b->tx_start_instr);
    dma_channel_transfer_from_buffer_now(b->tx_ch, encoded, len);
    b->pio->irq = USB_DISP_UDH_IRQ_TX_EOP_MASK;
}

void __not_in_flash_func(usb_disp_udh_bus_tx_wait_eop)(usb_disp_udh_bus_t *b) {
    while ((b->pio->irq & USB_DISP_UDH_IRQ_TX_EOP_MASK) == 0) {
        continue;
    }
    b->pio->irq = USB_DISP_UDH_IRQ_TX_EOP_MASK;
}

// TX EOP 待ちと受信アーミングを融合
void __not_in_flash_func(usb_disp_udh_bus_tx_wait_eop_and_arm)(usb_disp_udh_bus_t *b) {
    io_rw_32 *irq_reg = &b->pio->irq;
    while ((*irq_reg & USB_DISP_UDH_IRQ_TX_EOP_MASK) == 0) {
        continue;
    }
    *irq_reg = USB_DISP_UDH_IRQ_TX_EOP_MASK | USB_DISP_UDH_IRQ_RX_ALL_MASK;
}

// ---------------------------------------------------------------
// RX
// ---------------------------------------------------------------

void __not_in_flash_func(usb_disp_udh_bus_prepare_receive)(usb_disp_udh_bus_t *b) {
    pio_sm_set_enabled(b->pio, b->sm_rx, false);
    pio_sm_clear_fifos(b->pio, b->sm_rx);
    pio_sm_restart(b->pio, b->sm_rx);
    pio_sm_exec(b->pio, b->sm_rx, b->rx_reset_instr);
    pio_sm_exec(b->pio, b->sm_rx, b->rx_reset_instr2);
    pio_sm_set_enabled(b->pio, b->sm_rx, true);
}

void __not_in_flash_func(usb_disp_udh_bus_start_receive)(usb_disp_udh_bus_t *b) {
    b->pio->irq = USB_DISP_UDH_IRQ_RX_ALL_MASK;
}

void __not_in_flash_func(usb_disp_udh_bus_rx_disable)(usb_disp_udh_bus_t *b) {
    pio_sm_set_enabled(b->pio, b->sm_rx, false);
    b->rx_buf[0] = 0;
    b->rx_buf[1] = 0;
}

// パケット先頭 (falling edge) 検出を待つ
static inline bool __not_in_flash_func(wait_for_rx_start)(usb_disp_udh_bus_t *b) {
    uint32_t start = time_us_32();
    while (time_us_32() - start <= 3) {
        if ((b->pio->irq & USB_DISP_UDH_IRQ_RX_START_MASK) != 0) {
            return true;
        }
    }
    return false;
}

#if USB_DISP_UDH_HS_DIAG
usb_disp_udh_hs_diag_t g_udh_hs_diag;
#endif

uint8_t __not_in_flash_func(usb_disp_udh_bus_wait_handshake)(usb_disp_udh_bus_t *b) {
#if USB_DISP_UDH_HS_DIAG
    g_udh_hs_diag.total++;
#endif
    if (!wait_for_rx_start(b)) {
#if USB_DISP_UDH_HS_DIAG
        g_udh_hs_diag.no_start++;
#endif
        return 0;
    }

    uint8_t idx = 0;
    uint32_t start = time_us_32();
    while (time_us_32() - start <= 7) {
        if (idx < 2 && pio_sm_get_rx_fifo_level(b->pio, b->sm_rx)) {
            b->rx_buf[idx++] = (uint8_t)(pio_sm_get(b->pio, b->sm_rx) >> 24);
            start = time_us_32();
        } else if ((b->pio->irq & USB_DISP_UDH_IRQ_RX_COMP_MASK) != 0) {
            break;
        }
    }

    if (idx != 2 || b->rx_buf[0] != USB_DISP_UDH_USB_SYNC) {
#if USB_DISP_UDH_HS_DIAG
        if (idx < 2) g_udh_hs_diag.short_pkt++;
        else         g_udh_hs_diag.bad_sync++;
#endif
        return 0;
    }
#if USB_DISP_UDH_HS_DIAG
    switch (b->rx_buf[1]) {
    case USB_DISP_UDH_PID_ACK:   g_udh_hs_diag.pid_ack++;  break;
    case USB_DISP_UDH_PID_NAK:   g_udh_hs_diag.pid_nak++;  break;
    case USB_DISP_UDH_PID_OUT:   g_udh_hs_diag.pid_own++;  break;  // 自トークン
    case USB_DISP_UDH_PID_SOF:   g_udh_hs_diag.pid_sof++;  break;  // 自 SOF
    case USB_DISP_UDH_PID_DATA0:
    case USB_DISP_UDH_PID_DATA1: g_udh_hs_diag.pid_data++; break;  // 自データ
    default:                     g_udh_hs_diag.pid_other++;
                                 g_udh_hs_diag.last_pid = b->rx_buf[1]; break;
    }
#endif
    return b->rx_buf[1];
}

int16_t __not_in_flash_func(usb_disp_udh_bus_receive_packet_and_ack)(usb_disp_udh_bus_t *b) {
    if (!wait_for_rx_start(b)) {
        return -1;
    }

    uint16_t crc = 0xFFFF;
    uint16_t crc_prev = 0xFFFF;
    uint16_t crc_prev2 = 0xFFFF;
    uint16_t crc_receive = 0xFFFF;
    bool crc_match = false;
    int16_t idx = 0;

    uint32_t start = time_us_32();
    while (time_us_32() - start <= 7) {
        if (pio_sm_get_rx_fifo_level(b->pio, b->sm_rx)) {
            uint8_t data = (uint8_t)(pio_sm_get(b->pio, b->sm_rx) >> 24);
            if (idx < (int16_t)sizeof(b->rx_buf)) {
                b->rx_buf[idx] = data;
            }
            start = time_us_32();

            if (idx >= 2) {
                crc_prev2 = crc_prev;
                crc_prev = crc;
                crc = crc16_update(crc, data);
                crc_receive = (uint16_t)((crc_receive >> 8) | (data << 8));
                crc_match = ((uint16_t)(crc_receive ^ 0xFFFF) == crc_prev2);
            }
            idx++;
        } else if ((b->pio->irq & USB_DISP_UDH_IRQ_RX_COMP_MASK) != 0) {
            if (idx >= 4 && crc_match) {
                usb_disp_udh_bus_tx_start(b, s_ack_enc, s_ack_enc_len);
                usb_disp_udh_bus_tx_wait_eop(b);
                return (int16_t)(idx - 4);
            }
            break;
        }
    }
    return -1;
}

#endif

