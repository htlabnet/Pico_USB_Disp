//
// ######################################################################
//
//    MultiDisplay - 複数 USB ディスプレイ同時出力のサンプル
//                    (Raspberry Pi Pico / Pico 2 専用)
//
//    Pico は PIO ブロック 1 個につき USB ホストポートを 1 つ作れる
//      RP2040 = 最大 2 ポート / RP2350 = 最大 3 ポート
//    このサンプルは 2 ポート構成:
//      ポート0: D+=GPIO16, D-=GPIO17
//      ポート1: D+=GPIO18, D-=GPIO19
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <usb_disp.h>

#define NUM_PORTS 2
static usb_disp_t *disp[NUM_PORTS];
static bool shown[NUM_PORTS];

#define BAND_LINES 8
static uint16_t band[USB_DISP_MAX_WIDTH * BAND_LINES];

// 画面ごとのテーマ色
static const uint16_t theme[NUM_PORTS] = { 0x001F /*青*/, 0xF800 /*赤*/ };

void setup() {
    Serial.begin(115200);

    usb_disp_init();
    disp[0] = usb_disp_add(0, 16, 17);   // PIO0
    disp[1] = usb_disp_add(1, 18, 19);   // PIO1
    usb_disp_start();
}

// 縞パターン全面描画 (テーマ色と黒の横縞)
static void draw_stripes(int n) {
    uint16_t w = usb_disp_width(disp[n]), h = usb_disp_height(disp[n]);
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (y + BAND_LINES <= h) ? BAND_LINES : (h - y);
        uint16_t color = ((y / 64) & 1) ? theme[n] : 0x0000;
        for (uint32_t i = 0; i < (uint32_t)w * lines; i++) band[i] = color;
        usb_disp_update(disp[n], 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp[n], 1000);
}

void loop() {
    for (int n = 0; n < NUM_PORTS; n++) {
        if (!disp[n]) continue;

        if (usb_disp_poll(disp[n])) {
            if (usb_disp_ready(disp[n])) {
                Serial.print("display ");
                Serial.print(n);
                Serial.print(": ");
                Serial.print(usb_disp_chip_name(disp[n]));
                Serial.print(" ");
                Serial.print(usb_disp_width(disp[n]));
                Serial.print("x");
                Serial.println(usb_disp_height(disp[n]));
                draw_stripes(n);
                shown[n] = true;
            } else {
                Serial.print("display ");
                Serial.print(n);
                Serial.println(": disconnected");
                shown[n] = false;
            }
        }

        if (shown[n]) {
            // 白い四角がそれぞれの画面を左右に往復する
            static uint16_t px[NUM_PORTS];
            uint16_t w = usb_disp_width(disp[n]), h = usb_disp_height(disp[n]);
            usb_disp_fill(disp[n], px[n], h / 2 - 32, 64, 64, 0x0000);
            px[n] = (px[n] + 16) % (w - 64);
            usb_disp_fill(disp[n], px[n], h / 2 - 32, 64, 64, 0xFFFF);
            usb_disp_flush(disp[n], 1000);
        }
    }
    delay(33);
}


