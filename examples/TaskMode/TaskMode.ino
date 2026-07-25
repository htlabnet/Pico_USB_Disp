//
// ######################################################################
//
//    TaskMode - setup1()/loop1() と共存する手動サービスモードのサンプル
//
//      ※ Raspberry Pi Pico / Pico 2 専用
//
//    arduino-pico でスケッチが setup1()/loop1() を定義すると core1 は
//    スケッチのものになるため、ライブラリは core1 を起動できない
//    (usb_disp_start() が自動検出して手動モードに切り替わる)
//    その場合は loop1() から usb_disp_task() を休みなく呼び続けること
//    e コマンドで EEPROM.commit (フラッシュ書き込み) との共存も試せる
//
//    シリアルコマンド
//      i=状態
//      m=モード再設定
//      x=再エニュメ
//      e=EEPROM commit
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <usb_disp.h>
#include <EEPROM.h>

static usb_disp_t *disp;
static bool shown = false;

#define BAND_LINES 8
static uint16_t band[USB_DISP_MAX_WIDTH * BAND_LINES];

void setup() {
    Serial.begin(115200);

    usb_disp_init();
    disp = usb_disp_add(0, 16, 17);
    usb_disp_start();   // setup1/loop1 があるので手動モードへ自動フォールバック
}

// ---- core1: USB ホストサービス専用 ----
void setup1() {}
void loop1() {
    usb_disp_task();
}

// ---- core0: 描画と CLI ----
static void draw_color_bars(void) {
    static const uint16_t colors[8] = {
        0xFFFF, 0xFFE0, 0x07FF, 0x07E0, 0xF81F, 0xF800, 0x001F, 0x0000,
    };
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
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
}

void loop() {
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            draw_color_bars();
            shown = true;
        } else {
            shown = false;
        }
    }

    if (shown) {
        static uint16_t px = 0;
        uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
        usb_disp_fill(disp, px, h / 2 - 32, 64, 64, 0x0000);
        px = (px + 16) % (w - 64);
        usb_disp_fill(disp, px, h / 2 - 32, 64, 64, 0x07E0);   // 緑の箱
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
            Serial.print(" tx=");
            Serial.print((uint32_t)(usb_disp_stat_bytes(disp) / 1024));
            Serial.println(" KB");
        } else if (c == 'm' && usb_disp_ready(disp)) {
            usb_disp_set_auto_mode(disp);
            draw_color_bars();
        } else if (c == 'x') {
            usb_disp_force_reenum(disp);
        } else if (c == 'e') {
            // フラッシュ書き込み共存試験
            // 書き込み中は core1 (usb_disp_task)が一時停止するが表示は保持される
            EEPROM.begin(256);
            static uint8_t v = 0;
            EEPROM.write(0, ++v);
            uint32_t t0 = millis();
            bool ok = EEPROM.commit();
            Serial.print("EEPROM.commit ");
            Serial.print(ok ? "OK" : "FAIL");
            Serial.print(" (");
            Serial.print(millis() - t0);
            Serial.println(" ms)");
        }
    }
}


