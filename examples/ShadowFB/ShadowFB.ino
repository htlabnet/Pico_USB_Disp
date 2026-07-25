//
// ######################################################################
//
//    ShadowFB - シャドウフレームバッファ (差分更新) のサンプル
//
//      ※ DisplayLink (DL-1xx) アダプタ専用です (T6/MS91xx は
//         フルフレーム型のためシャドウFBは効かない)。
//      ※ PSRAM が必要です。 Pico / Teensy4.0 非対応。
//
//    シャドウ FB を有効にすると、update/fill は前回の画面内容と比較して
//    「変化したところだけ」を USB へ送る。静止部分の多い GUI では
//    転送量が桁違いに減り、実効フレームレートが大きく向上する。
//
//    このデモは 2 秒ごとに全く同じ全画面パターンを再描画し、
//    実際に送信されたバイト数をシリアルに表示する:
//      シャドウ ON  → 2回目以降はほぼ 0 KB (差分なし)
//      シャドウ OFF → 毎回全ピクセル分を送信
//    s コマンドで ON/OFF を切り替えて比較できる。
//
//    シリアルコマンド
//      i=状態
//      s=シャドウON/OFF切替
//      m=モード再設定
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <usb_disp.h>

static usb_disp_t *disp;
static bool shown = false;

#define BAND_LINES 8
static uint16_t band[USB_DISP_MAX_WIDTH * BAND_LINES];

void setup() {
    Serial.begin(115200);

    usb_disp_init();

    // shadow_fb = true で接続時に自動有効化 (メモリが無ければ無効のまま)
    usb_disp_config_t cfg = {};
    cfg.port = 0;
    cfg.pin_dp = 16;    // Pico 用
    cfg.pin_dm = 17;
    cfg.shadow_fb = true;
    disp = usb_disp_add_cfg(&cfg);

    usb_disp_start();
}

// 市松模様の全面描画 (毎回同じ内容)
static void draw_pattern(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (y + BAND_LINES <= h) ? BAND_LINES : (h - y);
        for (uint16_t ln = 0; ln < lines; ln++) {
            uint16_t *row = &band[ln * w];
            uint16_t yy = y + ln;
            for (uint16_t x = 0; x < w; x++)
                row[x] = (((x / 60) + (yy / 60)) & 1) ? 0x07FF : 0x2104;
        }
        usb_disp_update(disp, 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp, 1000);
}

void loop() {
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            Serial.print("connected: ");
            Serial.print(usb_disp_chip_name(disp));
            Serial.print("  shadow=");
            Serial.println(usb_disp_shadow_active(disp) ? "ON" : "OFF");
            shown = true;
        } else {
            shown = false;
        }
    }

    if (shown) {
        // 2秒ごとに「同じ画面」を全面再描画して送信量を測る
        static uint32_t last = 0;
        if (millis() - last >= 2000) {
            last = millis();
            uint64_t tx0 = usb_disp_stat_bytes(disp);
            uint32_t t0 = millis();
            draw_pattern();
            uint32_t dt = millis() - t0;
            uint32_t kb = (uint32_t)((usb_disp_stat_bytes(disp) - tx0) / 1024);
            Serial.print("full redraw: sent ");
            Serial.print(kb);
            Serial.print(" KB in ");
            Serial.print(dt);
            Serial.print(" ms  (shadow=");
            Serial.print(usb_disp_shadow_active(disp) ? "ON" : "OFF");
            Serial.println(")");
        }
    }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'i') {
            Serial.print("ready=");
            Serial.print(usb_disp_ready(disp));
            Serial.print(" chip=");
            Serial.print(usb_disp_chip_name(disp));
            Serial.print(" shadow=");
            Serial.println(usb_disp_shadow_active(disp) ? "ON" : "OFF");
        } else if (c == 's' && usb_disp_ready(disp)) {
            bool on = !usb_disp_shadow_active(disp);
            usb_disp_set_shadow(disp, on);   // ON にした直後は画面が黒で同期される
            Serial.print("shadow -> ");
            Serial.println(usb_disp_shadow_active(disp) ? "ON" : "OFF");
        } else if (c == 'm' && usb_disp_ready(disp)) {
            usb_disp_set_auto_mode(disp);
        }
    }
}


