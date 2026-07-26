//
// ######################################################################
//
//    LovyanGFX panel for Panel_USB_Disp
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#pragma once

#include <LovyanGFX.hpp>

#include "../usb_disp.h"

namespace lgfx {

struct Panel_USB_Disp : public lgfx::Panel_Device {
    Panel_USB_Disp(usb_disp_t *disp = nullptr) : _disp(disp) {
        // HAL はバルク送信をバッファに蓄積し flush で送出する
        _auto_display = true;
    }

    void setUsbDisp(usb_disp_t *disp) { _disp = disp; }
    usb_disp_t *getUsbDisp(void) const { return _disp; }

    bool init(bool use_reset) override {
        (void)use_reset;
        // usb_disp 側の接続完了 (モード設定済み) が前提
        if (!_disp || !usb_disp_ready(_disp) || usb_disp_width(_disp) == 0) {
            return false;
        }
        _cfg.panel_width = _cfg.memory_width = usb_disp_width(_disp);
        _cfg.panel_height = _cfg.memory_height = usb_disp_height(_disp);
        _cfg.offset_x = _cfg.offset_y = 0;
        _cfg.readable = false;
        _cfg.bus_shared = false;
        _write_depth = _req24 ? color_depth_t::rgb888_3Byte
                              : color_depth_t::rgb565_2Byte;
        _read_depth = _write_depth;
        _width = _cfg.panel_width;
        _height = _cfg.panel_height;
        return true;
    }

    void initBus(void) override {}
    void releaseBus(void) override {}

    void beginTransaction(void) override {}
    void endTransaction(void) override {}

    color_depth_t setColorDepth(color_depth_t depth) override {
        // RGB565 (既定) と RGB888 のみ対応。24bit を選ぶとライブラリ側も
        // 24bit 出力に切替を試みる (チップ非対応でも 565 変換で動く)
        _req24 = ((int)depth & color_depth_t::bit_mask) >= 24;
        _write_depth = _req24 ? color_depth_t::rgb888_3Byte
                              : color_depth_t::rgb565_2Byte;
        _read_depth = _write_depth;
        if (_disp) usb_disp_set_depth(_disp, _req24 ? 24 : 16);
        return _write_depth;
    }

    void setInvert(bool) override {}
    void setRotation(uint_fast8_t r) override {
        // 回転非対応 (rotation 0 固定)。幅/高さのみ確定させる
        _rotation = 0;
        (void)r;
        _width = _cfg.panel_width;
        _height = _cfg.panel_height;
    }
    void setSleep(bool flg_sleep) override {
        if (_disp) usb_disp_blank(_disp, flg_sleep);
    }
    void setPowerSave(bool) override {}

    void writeCommand(uint32_t, uint_fast8_t) override {}
    void writeData(uint32_t, uint_fast8_t) override {}

    void initDMA(void) override {}
    void waitDMA(void) override {}
    bool dmaBusy(void) override { return false; }
    void waitDisplay(void) override {
        if (_disp) usb_disp_flush(_disp, 100);
    }
    bool displayBusy(void) override { return false; }
    void display(uint_fast16_t, uint_fast16_t, uint_fast16_t,
                 uint_fast16_t) override {
        // auto display により描画トランザクション毎に呼ばれる
        // タイムアウトは1フレーム分の送信残に対して十分な値
        if (_disp) usb_disp_flush(_disp, 100);
    }
    bool isReadable(void) const override { return false; }
    bool isBusShared(void) const override { return false; }

    // ---- 描画 ----

    void setWindow(uint_fast16_t xs, uint_fast16_t ys, uint_fast16_t xe,
                   uint_fast16_t ye) override {
        _xs = xs; _ys = ys; _xe = xe; _ye = ye;
        _cx = xs; _cy = ys;
    }

    void drawPixelPreclipped(uint_fast16_t x, uint_fast16_t y,
                             uint32_t rawcolor) override {
        if (_req24) {
            // raw = バス順パック (低バイトから R,G,B) → ライブラリは B,G,R
            uint8_t px[3] = {(uint8_t)(rawcolor >> 16),
                             (uint8_t)(rawcolor >> 8), (uint8_t)rawcolor};
            usb_disp_update_888(_disp, (uint16_t)x, (uint16_t)y, 1, 1, px, 1);
            return;
        }
        uint16_t c = swap565_to_native(rawcolor);
        usb_disp_update_565(_disp, (uint16_t)x, (uint16_t)y, 1, 1, &c, 1);
    }

    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y,
                                 uint_fast16_t w, uint_fast16_t h,
                                 uint32_t rawcolor) override {
        if (_req24) {
            fill888(x, y, w, h, rawcolor);
            return;
        }
        usb_disp_fill(_disp, (uint16_t)x, (uint16_t)y, (uint16_t)w,
                      (uint16_t)h, swap565_to_native(rawcolor));
    }

    void writeBlock(uint32_t rawcolor, uint32_t length) override {
        // 現在ウィンドウのカーソル位置から length ピクセルを単色で埋める
        uint16_t c = _req24 ? (uint16_t)0 : swap565_to_native(rawcolor);
        while (length) {
            uint32_t n = _xe - _cx + 1;
            if (n > length) n = length;
            if (_req24) {
                fill888(_cx, _cy, n, 1, rawcolor);
            } else {
                usb_disp_fill(_disp, (uint16_t)_cx, (uint16_t)_cy,
                              (uint16_t)n, 1, c);
            }
            length -= n;
            advance_cursor(n);
        }
    }

    void writePixels(pixelcopy_t *param, uint32_t length,
                     bool use_dma) override {
        (void)use_dma;
        // ウィンドウ内をカーソル順に埋める (行単位で変換して転送)
        while (length) {
            uint32_t n = _xe - _cx + 1;
            if (n > length) n = length;
            param->fp_copy(_lb.u16, 0, n, param);
            send_line(_cx, _cy, n);
            length -= n;
            advance_cursor(n);
        }
    }

    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w,
                    uint_fast16_t h, pixelcopy_t *param,
                    bool use_dma) override {
        (void)use_dma;
        uint32_t sx32 = param->src_x32;
        do {
            param->fp_copy(_lb.u16, 0, w, param);
            send_line(x, y, w);
            param->src_x32 = sx32;
            param->src_y++;
            y++;
        } while (--h);
    }

    void writeImageARGB(uint_fast16_t, uint_fast16_t, uint_fast16_t,
                        uint_fast16_t, pixelcopy_t *) override {
        // 読み出し不可のためアルファ合成は非対応
    }

    // ---- 読み出し系 (非対応) ----
    uint32_t readCommand(uint_fast16_t, uint_fast8_t, uint_fast8_t) override {
        return 0;
    }
    uint32_t readData(uint_fast8_t, uint_fast8_t) override { return 0; }
    void readRect(uint_fast16_t, uint_fast16_t, uint_fast16_t, uint_fast16_t,
                  void *, pixelcopy_t *) override {}

    // ---- 画面内コピー (DL COPY16、ピクセル再送なし) ----
    void copyRect(uint_fast16_t dst_x, uint_fast16_t dst_y, uint_fast16_t w,
                  uint_fast16_t h, uint_fast16_t src_x,
                  uint_fast16_t src_y) override {
        usb_disp_copy(_disp, (uint16_t)src_x, (uint16_t)src_y,
                      (uint16_t)dst_x, (uint16_t)dst_y, (uint16_t)w,
                      (uint16_t)h);
    }

protected:
    usb_disp_t *_disp;
    bool _req24 = false;             // 24bit カラー選択中 (setColorDepth)
    uint_fast16_t _cx = 0, _cy = 0;  // ウィンドウ内書き込みカーソル
    union {
        uint16_t u16[USB_DISP_MAX_WIDTH];      // 565 用
        uint8_t u8[USB_DISP_MAX_WIDTH * 3];    // 888 用 (r,g,b バス順)
    } _lb;

    // LovyanGFX のパネルバイト列 (ビッグエンディアン/swap565) → ネイティブ u16
    static inline uint16_t swap565_to_native(uint32_t raw) {
        return __builtin_bswap16((uint16_t)raw);
    }

    // _lb に fp_copy された1行を現在深度で送出する
    inline void send_line(uint_fast16_t x, uint_fast16_t y, uint32_t n) {
        if (_req24) {
            // LGFX は r,g,b 順 → ライブラリは B,G,R (入れ替え)
            for (uint32_t i = 0; i < n; i++) {
                uint8_t t = _lb.u8[i * 3];
                _lb.u8[i * 3] = _lb.u8[i * 3 + 2];
                _lb.u8[i * 3 + 2] = t;
            }
            usb_disp_update_888(_disp, (uint16_t)x, (uint16_t)y, (uint16_t)n,
                                1, _lb.u8, n);
        } else {
            for (uint32_t i = 0; i < n; i++)
                _lb.u16[i] = __builtin_bswap16(_lb.u16[i]);
            usb_disp_update_565(_disp, (uint16_t)x, (uint16_t)y, (uint16_t)n,
                                1, _lb.u16, n);
        }
    }

    // 単色塗り (888): raw = 低バイトから R,G,B のバス順パック
    inline void fill888(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w,
                        uint_fast16_t h, uint32_t raw) {
        uint8_t r = (uint8_t)raw, g = (uint8_t)(raw >> 8),
                b = (uint8_t)(raw >> 16);
        for (uint_fast16_t i = 0; i < w; i++) {
            _lb.u8[i * 3 + 0] = b;
            _lb.u8[i * 3 + 1] = g;
            _lb.u8[i * 3 + 2] = r;
        }
        usb_disp_update_888(_disp, (uint16_t)x, (uint16_t)y, (uint16_t)w,
                            (uint16_t)h, _lb.u8, 0);
    }
    inline void advance_cursor(uint32_t n) {
        _cx += n;
        if (_cx > _xe) {
            _cx = _xs;
            _cy = (_cy >= _ye) ? _ys : _cy + 1;
        }
    }
};

}  // namespace lgfx

// デバイスクラス (パネル内蔵)
class LGFX_USB_Disp : public lgfx::LGFX_Device {
public:
    LGFX_USB_Disp(usb_disp_t *disp = nullptr) : _panel_instance(disp) {
        setPanel(&_panel_instance);
    }
    void setUsbDisp(usb_disp_t *disp) { _panel_instance.setUsbDisp(disp); }
    usb_disp_t *getUsbDisp(void) const { return _panel_instance.getUsbDisp(); }

private:
    lgfx::Panel_USB_Disp _panel_instance;
};


