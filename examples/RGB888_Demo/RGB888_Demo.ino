//
// ######################################################################
//
//    RGB888_Demo - 24bit カラー (RGB888) のサンプル
//
//    888 と 565 のグラデーションを並べて違いを見る
//    'd' キー で 16 / 24 bit を切替できる
//
//    24bit 対応チップ: DL-1xx (全プラットフォーム) /
//                      T6・MS912x・MS913x (ESP32-P4, PC)
//    非対応環境では自動的に 16bit で動く (Mode 表示で確認可)
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//
// 画面レイアウト (720p):
//   y   0..159 : グレー水平グラデーション (888 経路 = update_888)
//   y 160..319 : グレー水平グラデーション (565 経路 = update)
//   → 24bpp が効いていれば上段は滑らか、下段は 32 段のバンディング
//   y 320..399 : R グラデ (888) / y 400..479 : G (888) / y 480..559 : B (888)
//   → 色化けが無ければプレーン分割 (565+下位ビット面) の解釈が正しい
//   y 560..719 : カラーバー (565) = 従来動作の回帰確認
//
// シリアル: i=状態 d=深度16/24切替 (切替後は再描画)
//
#include <usb_disp.h>

static usb_disp_t *disp;
static bool shown = false;

static uint8_t row888[USB_DISP_MAX_WIDTH * 3];
static uint16_t row565[USB_DISP_MAX_WIDTH];

void setup() {
    Serial.begin(115200);
    usb_disp_set_log(true);

    usb_disp_init();
    usb_disp_config_t cfg = {};
    cfg.port = 0;
    cfg.pin_dp = 16;
    cfg.pin_dm = 17;
    cfg.width = 1280;
    cfg.height = 720;
    cfg.depth24 = true;          // ← 24bit カラー要求
    disp = usb_disp_add_cfg(&cfg);
    usb_disp_start();
}

static void draw_test(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    uint16_t band_h = h / 9;

    // 1) グレー 888
    for (uint16_t x = 0; x < w; x++) {
        uint8_t v = (uint8_t)((uint32_t)x * 255 / (w - 1));
        row888[x * 3 + 0] = v;
        row888[x * 3 + 1] = v;
        row888[x * 3 + 2] = v;
    }
    usb_disp_update_888(disp, 0, 0, w, band_h * 2, row888, 0);

    // 2) グレー 565
    for (uint16_t x = 0; x < w; x++) {
        uint8_t v = (uint8_t)((uint32_t)x * 255 / (w - 1));
        row565[x] = (uint16_t)(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
    }
    usb_disp_update(disp, 0, band_h * 2, w, band_h * 2, row565, 0);

    // 3) R / G / B グラデーション (888)
    for (int ch = 0; ch < 3; ch++) {
        for (uint16_t x = 0; x < w; x++) {
            uint8_t v = (uint8_t)((uint32_t)x * 255 / (w - 1));
            row888[x * 3 + 0] = (ch == 2) ? v : 0;   // B
            row888[x * 3 + 1] = (ch == 1) ? v : 0;   // G
            row888[x * 3 + 2] = (ch == 0) ? v : 0;   // R
        }
        usb_disp_update_888(disp, 0, (uint16_t)(band_h * (4 + ch)), w, band_h,
                            row888, 0);
    }

    // 4) カラーバー (565)
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    for (uint16_t x = 0; x < w; x++) row565[x] = colors[x * 8 / w];
    usb_disp_update(disp, 0, (uint16_t)(band_h * 7), w,
                    (uint16_t)(h - band_h * 7), row565, 0);

    usb_disp_flush(disp, 1000);
    Serial.print("drawn at depth=");
    Serial.println(usb_disp_depth(disp));
}

void loop() {
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            Serial.print("connected ");
            Serial.print(usb_disp_width(disp));
            Serial.print("x");
            Serial.print(usb_disp_height(disp));
            Serial.print(" depth=");
            Serial.println(usb_disp_depth(disp));
            draw_test();
            shown = true;
        } else {
            shown = false;
        }
    }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'i') {
            Serial.print("ready=");
            Serial.print(usb_disp_ready(disp));
            Serial.print(" depth=");
            Serial.print(usb_disp_depth(disp));
            Serial.print(" ");
            Serial.print(usb_disp_width(disp));
            Serial.print("x");
            Serial.println(usb_disp_height(disp));
        } else if (c == 'd' && shown) {
            uint8_t next = (usb_disp_depth(disp) == 24) ? 16 : 24;
            Serial.print("switching depth to ");
            Serial.println(next);
            usb_disp_set_depth(disp, next);
            draw_test();
        }
    }
    delay(10);
}


