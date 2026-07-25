//
// ######################################################################
//
//    ColorBars - Pico_USB_Disp 最小サンプル
//
//    カラーバーと動く四角を表示する
//    GUI ライブラリなし・素の API だけのデモ
//    シリアル (115200bps) コマンド:
//      i = 状態表示
//      m = モード再設定 (画面が黒くなった時)
//      x = 再エニュメ
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <usb_disp.h>

static usb_disp_t *disp;
static bool shown = false;

// 8 ライン分の帯バッファ (2048x8x2B = 32KB)
#define BAND_LINES 8
static uint16_t band[USB_DISP_MAX_WIDTH * BAND_LINES];

void setup() {
    Serial.begin(115200);

    usb_disp_set_log(true);           // ライブラリのログを Serial へ (不要なら消す)
    usb_disp_init();
    disp = usb_disp_add(0, 16, 17);   // port0, D+=GPIO16, D-=GPIO17, 解像度自動
    usb_disp_start();
}

// カラーバー全面描画 (8色 x 縦帯)
static void draw_color_bars(void) {
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0,   // 白 黄 シアン 緑
        0xF81F, 0xF800, 0x001F, 0x0000,   // マゼンタ 赤 青 黒
    };
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    uint32_t t0 = millis();
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (y + BAND_LINES <= h) ? BAND_LINES : (h - y);
        for (uint16_t ln = 0; ln < lines; ln++) {
            uint16_t *row = &band[ln * w];
            for (uint16_t x = 0; x < w; x++)
                row[x] = colors[x * 8 / w];
        }
        usb_disp_update(disp, 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp, 1000);
    Serial.print("full frame ");
    Serial.print(millis() - t0);
    Serial.println(" ms");
}

void loop() {
    // 接続状態の変化 (接続完了/切断) で true
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            draw_color_bars();
            shown = true;
        } else {
            shown = false;
        }
    }

    if (shown) {
        // 動く四角
        static uint16_t px = 0;
        uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
        usb_disp_fill(disp, px, h / 2 - 32, 64, 64, 0x0000);
        px = (px + 16) % (w - 64);
        usb_disp_fill(disp, px, h / 2 - 32, 64, 64, 0xFD20);
        usb_disp_flush(disp, 1000);
        delay(33);
    }

    if (Serial.available()) {
        int c = Serial.read();
        if (c == 'i') {
            Serial.print("ready=");
            Serial.print(usb_disp_ready(disp));
            Serial.print(" chip=");
            Serial.print(usb_disp_chip_name(disp));
            Serial.print(" ");
            Serial.print(usb_disp_width(disp));
            Serial.print("x");
            Serial.print(usb_disp_height(disp));
            Serial.print(" tx=");
            Serial.print((uint32_t)(usb_disp_stat_bytes(disp) / 1024));
            Serial.println(" KB");
        } else if (c == 'm' && usb_disp_ready(disp)) {
            usb_disp_set_auto_mode(disp);
            draw_color_bars();
        } else if (c == 'x') {
            usb_disp_force_reenum(disp);
        }
    }
}
