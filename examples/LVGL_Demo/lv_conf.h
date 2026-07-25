//
// ######################################################################
//
//    lv_conf.h - Pico_USB_Disp 向け LVGL 設定 (動作確認済み)
//
//    このファイルを Arduino の libraries フォルダ直下 (lvgl フォルダの
//    「隣」) にコピーして使う。編集は不要。コピー先:
//      Windows: C:\Users\<ユーザー名>\Documents\Arduino\libraries\lv_conf.h
//      macOS  : /Users/<ユーザー名>/Documents/Arduino/libraries/lv_conf.h
//      Linux  : /home/<ユーザー名>/Arduino/libraries/lv_conf.h
//    (スケッチブック保存場所を変更している場合は、Arduino IDE の
//     基本設定に表示されるフォルダの下の libraries/)
//    (未定義の項目は lvgl の lv_conf_internal.h のデフォルトが使われる)
//
// ######################################################################
//

#ifndef LV_CONF_H
#define LV_CONF_H

// RGB565 (usb_disp_lvgl_create() の既定フォーマットに合わせた設定。
// ライブラリ自体は RGB888 出力にも対応 - cf 引数に
// LV_COLOR_FORMAT_RGB888 を渡す。docs/api.md 参照)
#define LV_COLOR_DEPTH 16

// LVGL 内部ヒープ
#define LV_MEM_SIZE (64 * 1024U)

// tick は usb_disp_lvgl_create() が lv_tick_set_cb(millis) を接続する

// 描画に浮動小数点は不要
#define LV_USE_FLOAT 0

// LVGL 自身のログは無効 (シリアルログと干渉させない)
#define LV_USE_LOG 0

// フォント
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

// テーマ/ウィジェットはデフォルト設定 (ほぼ全て有効) を使用

#endif // LV_CONF_H
