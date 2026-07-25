//
// ######################################################################
//
//    FixedResolution - 解像度指定のサンプル
//
//    解像度の決まり方は4通り
//      1. 自動 (既定)     : usb_disp_add(0, 16, 17)
//         接続時に EDID から自動選択
//      2. 接続時に固定    : usb_disp_add(0, 16, 17, 800, 600)
//         チップ上限超過などで設定できない場合はフォールバック
//         (実際の解像度は READY 後に usb_disp_width/heightで確認)
//      3. 実行中に変更    : usb_disp_set_mode(d, w, h) など
//         (このサンプルではシリアルの数字キーで切り替えられる)
//      4. EDID を無視して固定 : usb_disp_add(0, 16, 17, 1920, 1080, true)
//         (6番目の引数。usb_disp_set_ignore_edid でも切り替え可)
//         HDMI アナライザ / スプリッタ / キャプチャ / KVM を挟むと、
//         中継機が「実際に出せる解像度より小さい EDID」を返すことがある。
//         ライブラリは CEA 拡張ブロック (VIC) まで見るので大抵はそれで
//         正しく判定できるが、それでも足りない経路ではこれで強制する
//
//    自動選択 (a) のポリシーは2種類:
//      MAX (既定) : EDID が申告する解像度のうちチップ上限に収まる最大
//      PREFERRED  : ベースブロックの preferred timing (ネイティブ解像度)
//                   パネル等倍で出したい場合
//
//    ※  1366x768のテレビ等、FullHDは受けられるがパネル解像度が小さい場合、
//        MAXでFullHD→テレビ側でダウンコンバートになり、
//        PREFERREDでパネル等倍1366x768となる
//
//    シリアル (115200bps) コマンド:
//      1=1920x1080
//      2=1600x1200
//      3=1280x1024
//      4=1024x768
//      5=800x600
//      6=1280x720@50 (CVT-RB 計算タイミングの例)
//      a=自動選択
//      p=自動選択ポリシー切替
//      e=EDID 無視のON/OFF
//      i=状態表示
//
//    注意: モニタ/キャプチャが対応しない解像度は黒画面になることがある。
//    その場合も別の解像度に戻せば復帰する。
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
    usb_disp_set_log(true);           // モード設定の様子が見えるようログON

    usb_disp_init();
    disp = usb_disp_add(0, 16, 17, 800, 600);   // 800x600 固定で接続
    usb_disp_start();
}

// 画面全体に市松模様 + 解像度が分かる縁取りを描く
static void draw_pattern(void) {
    uint16_t w = usb_disp_width(disp), h = usb_disp_height(disp);
    for (uint16_t y = 0; y < h; y += BAND_LINES) {
        uint16_t lines = (y + BAND_LINES <= h) ? BAND_LINES : (h - y);
        for (uint16_t ln = 0; ln < lines; ln++) {
            uint16_t *row = &band[ln * w];
            uint16_t yy = y + ln;
            for (uint16_t x = 0; x < w; x++) {
                if (x < 8 || x >= w - 8 || yy < 8 || yy >= h - 8)
                    row[x] = 0xF800;                       // 外周 = 赤枠
                else
                    row[x] = (((x / 40) + (yy / 40)) & 1) ? 0xFFFF : 0x0000;
            }
        }
        usb_disp_update(disp, 0, y, w, lines, band, w);
    }
    usb_disp_flush(disp, 1000);
    Serial.print("pattern drawn at ");
    Serial.print(w);
    Serial.print("x");
    Serial.println(h);
}

// モード変更 → 成否表示 → 再描画 (モード変更後は画面内容が不定なため)
static void change_mode(uint16_t w, uint16_t h) {
    if (usb_disp_set_mode(disp, w, h)) {
        draw_pattern();
    } else {
        Serial.println("set_mode failed (chip limit? see log)");
    }
}

void loop() {
    if (usb_disp_poll(disp)) {
        if (usb_disp_ready(disp)) {
            // 800x600 が設定できたか (できなければ自動選択の解像度になる)
            Serial.print("connected at ");
            Serial.print(usb_disp_width(disp));
            Serial.print("x");
            Serial.println(usb_disp_height(disp));
            draw_pattern();
            shown = true;
        } else {
            shown = false;
        }
    }

    if (shown && Serial.available()) {
        int c = Serial.read();
        switch (c) {
        case '1': change_mode(1920, 1080); break;
        case '2': change_mode(1600, 1200); break;
        case '3': change_mode(1280, 1024); break;
        case '4': change_mode(1024, 768);  break;
        case '5': change_mode(800, 600);   break;
        case '6':
            // 内蔵リストに無いタイミングは CVT-RB で計算される例 (720p50)
            if (usb_disp_set_mode_hz(disp, 1280, 720, 50)) draw_pattern();
            break;
        case 'a':
            if (usb_disp_set_auto_mode(disp)) draw_pattern();
            break;
        case 'p': {
            // 自動選択ポリシーの切替 (その場で反映するため自動選択し直す)
            bool pref = (usb_disp_edid_policy(disp) ==
                         USB_DISP_EDID_POLICY_PREFERRED);
            usb_disp_set_edid_policy(disp, pref ? USB_DISP_EDID_POLICY_MAX
                                                : USB_DISP_EDID_POLICY_PREFERRED);
            Serial.print("edid_policy=");
            Serial.println(pref ? "max" : "preferred");
            if (usb_disp_set_auto_mode(disp)) draw_pattern();
            break;
        }
        case 'e':
            // EDID 無視のトグル。ON にすると以後ライブラリは EDID を読まず、
            // set_mode / 自動選択とも EDID に引き摺られなくなる
            usb_disp_set_ignore_edid(disp, !usb_disp_ignore_edid(disp));
            Serial.print("ignore_edid=");
            Serial.println(usb_disp_ignore_edid(disp) ? "on" : "off");
            break;
        case 'i':
            Serial.print("chip=");
            Serial.print(usb_disp_chip_name(disp));
            Serial.print(" ");
            Serial.print(usb_disp_width(disp));
            Serial.print("x");
            Serial.print(usb_disp_height(disp));
            Serial.print(" max_area=");
            Serial.print((uint32_t)usb_disp_max_area(disp));
            Serial.print(" ignore_edid=");
            Serial.print(usb_disp_ignore_edid(disp) ? "on" : "off");
            Serial.print(" policy=");
            Serial.print(usb_disp_edid_policy(disp) ==
                         USB_DISP_EDID_POLICY_PREFERRED ? "preferred" : "max");
            // EDID (拡張ブロック込み) が何に対応していると言っているか
            uint16_t mw = 0, mh = 0;
            if (usb_disp_edid_max_mode(disp, &mw, &mh)) {
                Serial.print(" edid_max=");
                Serial.print(mw);
                Serial.print("x");
                Serial.print(mh);
            }
            Serial.print(" edid=");
            Serial.print(usb_disp_edid_size(disp));
            Serial.print("B supports:");
            for (uint8_t i = 0; i < usb_disp_builtin_mode_count(); i++) {
                const usb_disp_mode_t *m = usb_disp_builtin_mode(i);
                if (usb_disp_edid_supports(disp, m->width, m->height)) {
                    Serial.print(" ");
                    Serial.print(m->width);
                    Serial.print("x");
                    Serial.print(m->height);
                }
            }
            Serial.println();
            break;
        }
    }
}


