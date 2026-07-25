//
// ######################################################################
//
//    usb_disp_model - VID/PID 型番リスト
//
//    Copyright (C) 2026
//      Hideto Kikuchi / PJラボ (@pcjpnet) - https://pc-jp.net/
//
// ######################################################################
//

#ifndef USB_DISP_MODEL_H_
#define USB_DISP_MODEL_H_

#include <stdint.h>

#include "usb_disp.h"   // usb_disp_chip_t

// ---- 型番リスト (VID/PID 製品型番) ----
typedef struct {
    uint16_t vid;
    uint16_t pid;
    const char *name;
} usb_disp_model_t;

static const usb_disp_model_t usb_disp_models[] = {
    //  VID     PID     型番
    { 0x17E9, 0x01AC, "BUFFALO GX-DVI/U2" },
    { 0x17E9, 0x01BB, "CENTURY LCD-8000U" },
    { 0x17E9, 0x01D7, "HP NL571AA" },
    { 0x17E9, 0x0221, "BUFFALO GX-DVI/U2AI" },
    { 0x17E9, 0x4304, "I-O DATA USB-RGB3/D" },
    { 0x0711, 0x5601, "j5create JUA350" },


};

// ---- DL-1x0 チップ確定リスト ----
// DL の世代は status dword で判るが、世代内の型番 (120/160 等) はチップが
// 自己申告する max_area (0x5F ベンダーディスクリプタ) の閾値で判定している
// 0x5F に応答しない個体はこの判定ができないため、
// 分解して実チップを確認できた製品だけをここに登録する
typedef struct {
    uint16_t vid;
    uint16_t pid;
    usb_disp_chip_t chip;
} usb_disp_model_chip_t;

static const usb_disp_model_chip_t usb_disp_model_chips[] = {
    //  VID     PID     実チップ
    { 0x17E9, 0x01AC, USB_DISP_CHIP_DL160 },// BUFFALO GX-DVI/U2


};

#endif  // USB_DISP_MODEL_H_

