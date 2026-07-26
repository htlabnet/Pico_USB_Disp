//
// ######################################################################
//
//    LovyanGFX_Demo - Pico_USB_Disp + LovyanGFX サンプル
//
//    必要ライブラリ: LovyanGFX (ライブラリマネージャで導入)
//    ※ Teensy 4.x は LovyanGFX の 1.2.27 以降のリリースで対応
//
//    LGFX_USB_Disp クラスは <usb_disp.h> だけで使える
//
//      lcd.init() は USB ディスプレイの接続完了 (READY) 後に呼ぶ。
//      (解像度が接続時に決まるため)
//      読み出し不可・回転 0 固定。スプライトは全機能可。
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <LovyanGFX.hpp>
#include <usb_disp.h>

static usb_disp_t *disp;
static LGFX_USB_Disp lcd;               // LovyanGFX デバイス
static LGFX_Sprite ball(&lcd);          // 動き回るボール (スプライト)
static bool gui_up = false;

// ボールの1フレームあたり移動量。スプライトはこのぶんの背景マージンを
// 持たせ、前回位置の消し残りを同じ1回の転送で上書きする
// (「消してから描く」と、消えた瞬間が画面に走査されてチラつくため)
enum { BALL_DX = 7, BALL_DY = 5 };

void setup() {
    Serial.begin(115200);

    usb_disp_init();
    disp = usb_disp_add(0, 16, 17);     // 解像度は接続時に EDID から自動選択
    usb_disp_start();
}

// 接続完了後に一度だけ画面を作る
static void gui_start(void) {
    lcd.setUsbDisp(disp);
    lcd.init();                         // ここで解像度が確定

    int w = lcd.width(), h = lcd.height();
    lcd.fillScreen(TFT_NAVY);
    lcd.setTextColor(TFT_WHITE, TFT_NAVY);
    lcd.setTextSize(3);
    lcd.setCursor(40, 40);
    lcd.printf("Pico_USB_Disp + LovyanGFX  %dx%d", w, h);

    // 図形いろいろ
    lcd.drawRect(30, 100, w - 60, h - 140, TFT_CYAN);
    for (int i = 0; i < 8; i++) {
        lcd.fillCircle(120 + i * 90, 200, 36,
                       lcd.color565(255 - i * 30, i * 30, 128));
    }
    lcd.drawLine(30, h - 60, w - 30, h - 60, TFT_YELLOW);

    // ボールスプライト (64x64 + 背景マージン) を作る
    ball.createSprite(64 + BALL_DX * 2, 64 + BALL_DY * 2);
    ball.fillScreen(TFT_NAVY);
    ball.fillCircle(BALL_DX + 32, BALL_DY + 32, 30, TFT_ORANGE);
    ball.fillCircle(BALL_DX + 22, BALL_DY + 22, 10, TFT_WHITE);

    gui_up = true;
}

void loop() {
    if (usb_disp_poll(disp)) {          // 接続/切断イベント
        if (usb_disp_ready(disp)) {
            gui_start();
        } else {
            gui_up = false;
            ball.deleteSprite();
        }
    }

    if (gui_up) {
        // ボールを跳ね回らせる (可動域は枠線や円と重ならない内側だけ)
        int xmin = 40, xmax = lcd.width() - 40 - 64;
        int ymin = 260, ymax = lcd.height() - 80 - 64;
        static int x = 100, y = 300, dx = BALL_DX, dy = BALL_DY;
        x += dx; y += dy;
        if (x < xmin || x > xmax) { dx = -dx; x += dx; }
        if (y < ymin || y > ymax) { dy = -dy; y += dy; }
        // 背景マージン込みで一括転送 (前回位置の消去を兼ねる)
        ball.pushSprite(x - BALL_DX, y - BALL_DY);
        delay(16);
    }
}


