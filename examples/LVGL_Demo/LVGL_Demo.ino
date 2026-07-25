//
// ######################################################################
//
//    LVGL_Demo - Pico_USB_Disp + LVGL (v9) サンプル
//
//    必要ライブラリ: lvgl v9.x (ライブラリマネージャで導入)
//
//    以下の手順で lv_conf.h の準備が必要
//    このスケッチと同じフォルダにある lv_conf.h を、Arduino の
//    libraries フォルダ直下にコピーする。編集は不要。
//    コピー先は
//    Windows: C:\Users\<ユーザー名>\Documents\Arduino\libraries\lv_conf.h
//    macOS  : /Users/<ユーザー名>/Documents/Arduino/libraries/lv_conf.h
//    Linux  : /home/<ユーザー名>/Arduino/libraries/lv_conf.h
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#include <lvgl.h>
#include <usb_disp.h>

static usb_disp_t *disp;
static lv_display_t *lvd;

// ---- チャート (2x2 配置、タイマーでランダム更新) ----
#define CELL_PAD    4     // 画面端とセル間の隙間 [px]
#define LINE_PTS    48    // 折れ線/エリアの点数
#define SCAT_PTS    32    // 散布図の点数
#define BAR_PTS     12    // 棒の本数

static lv_obj_t *ch_line, *ch_scat, *ch_bar, *ch_area;
static lv_chart_series_t *s_line1, *s_line2;   // 折れ線 2系列
static lv_chart_series_t *s_scat;              // 散布図
static lv_chart_series_t *s_bar1, *s_bar2;     // 棒 2系列
static lv_chart_series_t *s_area;              // エリア (塗り付き折れ線)

// ランダムウォーク (0..100 に収める)
static int32_t walk(int32_t v, int32_t step) {
    v += (int32_t)lv_rand(0, step * 2) - step;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    return v;
}

// 各チャート共通の見た目 + タイトルラベル
static lv_obj_t *make_chart(lv_obj_t *parent, const char *title,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_pos(chart, x, y);
    lv_obj_set_size(chart, w, h);
    lv_obj_set_style_bg_color(chart, lv_color_hex(0x1b2430), 0);
    lv_obj_set_style_border_color(chart, lv_color_hex(0x3a4a5f), 0);
    lv_obj_set_style_radius(chart, 4, 0);
    lv_chart_set_div_line_count(chart, 5, 7);
    lv_obj_set_style_line_color(chart, lv_color_hex(0x2c3a4c), LV_PART_MAIN);

    lv_obj_t *label = lv_label_create(chart);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9fb6cc), 0);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, 4, 0);
    return chart;
}

// 120ms 毎: 全チャートへランダムデータを流し込む
static void chart_tick(lv_timer_t *t) {
    LV_UNUSED(t);
    static int32_t v1 = 50, v2 = 30, va = 60;

    v1 = walk(v1, 9);
    v2 = walk(v2, 6);
    lv_chart_set_next_value(ch_line, s_line1, v1);
    lv_chart_set_next_value(ch_line, s_line2, v2);

    lv_chart_set_next_value2(ch_scat, s_scat,
                             (int32_t)lv_rand(0, 200),
                             (int32_t)lv_rand(0, 100));

    lv_chart_set_next_value(ch_bar, s_bar1, (int32_t)lv_rand(10, 100));
    lv_chart_set_next_value(ch_bar, s_bar2, (int32_t)lv_rand(10, 100));

    va = walk(va, 12);
    lv_chart_set_next_value(ch_area, s_area, va);
}

// 接続完了後に一度だけ LVGL 画面を作る
static void gui_start(void) {
    lvd = usb_disp_lvgl_create(disp);   // これだけで LVGL 画面が使える
    if (!lvd) return;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x0e141b), 0);

    // 画面を 2x2 に4分割し、隙間 CELL_PAD でギリギリに敷き詰める
    // (800x600 なら1セル 394x294。より大きい解像度はそのまま拡大)
    int32_t W = usb_disp_width(disp), H = usb_disp_height(disp);
    int32_t cw = (W - CELL_PAD * 3) / 2;
    int32_t ch = (H - CELL_PAD * 3) / 2;
    int32_t x0 = CELL_PAD, x1 = CELL_PAD * 2 + cw;
    int32_t y0 = CELL_PAD, y1 = CELL_PAD * 2 + ch;

    // --- 左上: 折れ線 (2系列のランダムウォーク) ---
    ch_line = make_chart(scr, "Line", x0, y0, cw, ch);
    lv_chart_set_point_count(ch_line, LINE_PTS);
    lv_chart_set_axis_range(ch_line, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_line1 = lv_chart_add_series(ch_line, lv_palette_main(LV_PALETTE_CYAN),
                                  LV_CHART_AXIS_PRIMARY_Y);
    s_line2 = lv_chart_add_series(ch_line, lv_palette_main(LV_PALETTE_ORANGE),
                                  LV_CHART_AXIS_PRIMARY_Y);

    // 解像度とチップ名 (動作報告用) は折れ線セルの下辺に重ねて表示
    lv_obj_t *info = lv_label_create(ch_line);
    lv_label_set_text_fmt(info, "Pico_USB_Disp + LVGL  %ux%u  (%s)",
                          (unsigned)W, (unsigned)H, usb_disp_chip_name(disp));
    lv_obj_set_style_text_color(info, lv_color_hex(0x6e88a5), 0);
    lv_obj_align(info, LV_ALIGN_BOTTOM_LEFT, 4, 0);

    // --- 右上: 散布図 (点のみ、毎回ランダムな座標) ---
    ch_scat = make_chart(scr, "Scatter", x1, y0, cw, ch);
    lv_chart_set_type(ch_scat, LV_CHART_TYPE_SCATTER);
    lv_chart_set_point_count(ch_scat, SCAT_PTS);
    lv_chart_set_axis_range(ch_scat, LV_CHART_AXIS_PRIMARY_X, 0, 200);
    lv_chart_set_axis_range(ch_scat, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_line_width(ch_scat, 0, LV_PART_ITEMS);   // 線を消して点だけ
    s_scat = lv_chart_add_series(ch_scat, lv_palette_main(LV_PALETTE_GREEN),
                                 LV_CHART_AXIS_PRIMARY_Y);

    // --- 左下: 棒グラフ (2系列) ---
    ch_bar = make_chart(scr, "Bar", x0, y1, cw, ch);
    lv_chart_set_type(ch_bar, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(ch_bar, BAR_PTS);
    lv_chart_set_axis_range(ch_bar, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    s_bar1 = lv_chart_add_series(ch_bar, lv_palette_main(LV_PALETTE_BLUE),
                                 LV_CHART_AXIS_PRIMARY_Y);
    s_bar2 = lv_chart_add_series(ch_bar, lv_palette_main(LV_PALETTE_RED),
                                 LV_CHART_AXIS_PRIMARY_Y);

    // --- 右下: エリア (塗りつぶし付き折れ線) ---
    ch_area = make_chart(scr, "Area", x1, y1, cw, ch);
    lv_chart_set_point_count(ch_area, LINE_PTS);
    lv_chart_set_axis_range(ch_area, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_obj_set_style_bg_opa(ch_area, LV_OPA_40, LV_PART_ITEMS);   // 線の下を塗る
    s_area = lv_chart_add_series(ch_area, lv_palette_main(LV_PALETTE_PURPLE),
                                 LV_CHART_AXIS_PRIMARY_Y);

    // 初期データを埋めてからタイマーで流し続ける
    for (int i = 0; i < LINE_PTS; i++) chart_tick(NULL);
    lv_timer_create(chart_tick, 120, NULL);
}

void setup() {
    Serial.begin(115200);

    usb_disp_init();
    disp = usb_disp_add(0, 16, 17);   // 解像度は接続時に EDID から自動選択
    usb_disp_start();
}

void loop() {
    if (usb_disp_poll(disp)) {        // 接続/切断イベント
        if (usb_disp_ready(disp) && !lvd) gui_start();
    }

    if (lvd) lv_timer_handler();      // LVGL を回す
    delay(5);
}


