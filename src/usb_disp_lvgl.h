//
// ######################################################################
//
//    usb_disp_lvgl - LVGL v9 連携ヘルパー (Arduino 用)
//
//    usb_disp.h から自動 include される
//      (スケッチが <lvgl.h> を include している場合のみ)
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#ifndef USB_DISP_LVGL_H_
#define USB_DISP_LVGL_H_

#include <stdlib.h>
#include <lvgl.h>
#if defined(ARDUINO)
#include <Arduino.h>
#endif

#include "usb_disp.h"

// LVGL → USB ディスプレイ転送 (部分矩形更新)
// usb_disp_lvgl_create が接続する
static inline void usb_disp_lvgl_flush_cb_(lv_display_t *lvd,
                                           const lv_area_t *area,
                                           uint8_t *px_map) {
    usb_disp_t *d = (usb_disp_t *)lv_display_get_user_data(lvd);
    uint16_t w = (uint16_t)(area->x2 - area->x1 + 1);
    uint16_t h = (uint16_t)(area->y2 - area->y1 + 1);
    if (lv_display_get_color_format(lvd) == LV_COLOR_FORMAT_RGB888) {
        // LVGL RGB888 バッファ (3B/px, B,G,R) はライブラリの 888 入力と同一
        usb_disp_update_888(d, (uint16_t)area->x1, (uint16_t)area->y1, w, h,
                            px_map, w);
    } else {
        usb_disp_update_565(d, (uint16_t)area->x1, (uint16_t)area->y1, w, h,
                            (const uint16_t *)px_map, w);
    }
    if (lv_display_flush_is_last(lvd)) usb_disp_flush(d, 100);
    lv_display_flush_ready(lvd);
}

//  USB ディスプレイを LVGL 画面として登録する。
//    d         : READY 状態の usb_disp ハンドル (未接続なら NULL を返す)
//    buf_lines : 描画バッファの行数 (省略時 16。確保できなければ半分ずつ縮小)
//    cf        : LV_COLOR_FORMAT_RGB565 (既定) または LV_COLOR_FORMAT_RGB888
//  戻り値: lv_display_t* (バッファ確保失敗は NULL)
static inline lv_display_t *usb_disp_lvgl_create(usb_disp_t *d,
                                                 uint16_t buf_lines = 16,
                                                 lv_color_format_t cf =
                                                     LV_COLOR_FORMAT_RGB565) {
    if (!d || !usb_disp_ready(d)) return NULL;
    if (cf != LV_COLOR_FORMAT_RGB888) cf = LV_COLOR_FORMAT_RGB565;
    if (cf == LV_COLOR_FORMAT_RGB888)
        usb_disp_set_depth(d, 24);   // 可能なら 24bit 出力へ (不可でも動く)

    if (!lv_is_initialized()) {
        lv_init();
#if defined(ARDUINO)
        lv_tick_set_cb([]() -> uint32_t { return (uint32_t)millis(); });
#endif
    }

    uint16_t w = usb_disp_width(d), h = usb_disp_height(d);
    uint8_t px_bytes = (cf == LV_COLOR_FORMAT_RGB888) ? 3 : 2;
    void *buf = NULL;
    size_t sz = 0;
    for (uint16_t lines = buf_lines; lines > 0 && !buf; lines /= 2) {
        sz = (size_t)w * lines * px_bytes;
        buf = malloc(sz);
    }
    if (!buf) return NULL;

    lv_display_t *lvd = lv_display_create(w, h);
    if (!lvd) {
        free(buf);
        return NULL;
    }
    lv_display_set_user_data(lvd, d);
    lv_display_set_color_format(lvd, cf);
    lv_display_set_flush_cb(lvd, usb_disp_lvgl_flush_cb_);
    lv_display_set_buffers(lvd, buf, NULL, (uint32_t)sz,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    return lvd;
}

#endif // USB_DISP_LVGL_H_

