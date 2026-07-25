---
layout: default
title: Library Usage
nav_order: 2
---

# Pico_USB_Disp - Library Usage

**For simple usage instructions, please see the [Quick Start]({{ '/' | relative_url }}#quick-start) section on the [Home Page]({{ '/' | relative_url }}).**

This library turns a USB display adapter (DisplayLink DL-1xx, etc.) into an external display that you can drive from an MCU.
The default color format is RGB565 (16-bit); 24-bit RGB888 is also supported.

| Platform | USB host | Supported chips |
|---|---|---|
| Raspberry Pi Pico / Pico 2 (RP2040 / RP2350)  | Implemented in PIO (Full-Speed)   | DisplayLink DL-1xx |
| ESP32-S2 / S3                                 | USB-OTG (Full-Speed)              | DisplayLink DL-1xx |
| ESP32-P4                                      | USB-OTG (High-Speed)              | DL-1xx + MCT Trigger 6 + MS91xx |
| Teensy 4.0 / 4.1                              | Built-in EHCI (High-Speed)        | DisplayLink DL-1xx |
| PC (Windows / Linux / macOS)                  | libusb                            | DL-1xx + MCT Trigger 6 + MS91xx |



## 1. Installation (Arduino)

Search for **Pico_USB_Disp** in the Library Manager and install it.
Use the following board cores:

- Pico / Pico 2: [**Arduino-Pico** (earlephilhower core)](https://github.com/earlephilhower/arduino-pico){:target="_blank" rel="noopener noreferrer"}
- ESP32-S2 / S3 / P4: [**Arduino core for the ESP32 family of SoCs** (Espressif core 3.x)](https://github.com/espressif/arduino-esp32){:target="_blank" rel="noopener noreferrer"}
- Teensy 4.0 / 4.1: [**Teensyduino** (PJRC core)](https://www.pjrc.com/teensy/td_download.html){:target="_blank" rel="noopener noreferrer"}


## 2. Wiring

**Raspberry Pi Pico / Pico 2** - Add an extra USB connector (e.g. a board-mount USB-A female connector) and plug the adapter into it:


| Adapter side | Pico side |
|---|---|
| D+        | GPIO16 (configurable) |
| D-        | GPIO17 (must be the GPIO adjacent to D+; reversed order is also OK) |
| VBUS (5V) | VBUS (5V). Watch the current draw of bus-powered adapters |
| GND       | GND |

If you are using a board equipped with a USB connector, simply connect the adapter.

**ESP32-S2 / S3 / P4** - Just plug the adapter into the board's USB-OTG connector (an OTG adapter cable may be required). No GPIO configuration is needed.

**Teensy 4.1** - Use the 5-pin USB host header in the middle of the board (solder pins and use a USB host cable, e.g. [PJRC's](https://www.pjrc.com/store/cable_usb_host_t36.html){:target="_blank" rel="noopener noreferrer"}). The 5V and GND pins of the header power the adapter. No GPIO configuration is needed.

**Teensy 4.0** - The host D+/D- pads are on the bottom side of the board; wire them to a USB-A female connector together with 5V (VUSB) and GND.



## 3. Minimal code

```cpp
#include <usb_disp.h>

usb_disp_t *disp;

void setup() {
    usb_disp_init();                    // 1. Initialize
    disp = usb_disp_add(0, 16, 17);     // 2. Register a port (D+=GPIO16, D-=GPIO17)
    usb_disp_start();                   // 3. Start the host
}

void loop() {
    usb_disp_poll(disp);                // 4. Call periodically (handles connection)
    if (usb_disp_ready(disp)) {
        // 5. Once connected, draw whenever you like
        usb_disp_fill(disp, 100, 100, 200, 150, 0xF800);  // red rectangle
        usb_disp_flush(disp, 100);
    }
}
```

The two GPIO numbers are only meaningful on the Pico. On **ESP32-S2/S3/P4, Teensy 4.x and PC** the host pins are fixed, so you can drop them entirely:

```cpp
disp = usb_disp_add(0);             // ESP32 / Teensy / PC: port number only
```

Passing pins on those platforms is harmless (they are ignored), so the Pico form above still builds everywhere - handy when you want one sketch for several boards.

That is all it takes. No clock setup, no logging setup, no resolution setup (the library handles everything automatically; at connection time it picks the largest resolution the display advertises in its EDID that the adapter chip can drive - see `edid_policy` in the API reference if you would rather have the display's native/preferred timing).



### Drawing model

- The default color format is **RGB565** (16-bit). `usb_disp_update()` transfers a pixel array to an arbitrary rectangle, and `usb_disp_fill()` fills a rectangle with a solid color.
- **24-bit color (RGB888)** is also supported: use `usb_disp_update_888()`, or switch the display depth itself with `usb_disp_set_depth()`. 
See [24-bit color in the API Reference]({{ '/api/' | relative_url }}#24-bit-color-rgb888) for details.
- Call `usb_disp_flush()` at the end of each batch of drawing (it pushes out any buffered data).
- The frame buffer lives inside the adapter, so **anything you have drawn stays on screen without being resent**. 
Updating only the parts that change is sufficient.


### Connect / Disconnect events

`usb_disp_poll()` returns true at the moment the display becomes READY and at the moment it is disconnected:

```cpp
if (usb_disp_poll(disp)) {
    if (usb_disp_ready(disp)) {
        // Connected: the resolution is now known. Draw the initial screen.
    } else {
        // Unplugged: it will re-enumerate automatically when reconnected.
    }
}
```


## 4. Using GUI libraries

### LovyanGFX

Install LovyanGFX and include it; the `LGFX_USB_Disp` class becomes available automatically (no extra include needed):

```cpp
#include <LovyanGFX.hpp>
#include <usb_disp.h>

usb_disp_t *disp;
LGFX_USB_Disp lcd;

// Once, after READY:
lcd.setUsbDisp(disp);
lcd.init();
lcd.fillScreen(TFT_NAVY);
lcd.drawCircle(960, 540, 100, TFT_CYAN);
```

Limitations: no readback / rotation fixed to 0. Color depth is 16-bit by default; 24-bit is available via `lcd.setColorDepth(24)`. 
Sprites work with full functionality. -> See the `LovyanGFX_Demo` example.

Note: **not available on Teensy 4.x** - LovyanGFX itself does not support the i.MX RT platform (its platform layer fails to compile; nothing this library can work around). On Teensy, use LVGL instead.


### LVGL (v9)

Install lvgl, then copy the `lv_conf.h` bundled with the `LVGL_Demo` example directly into your libraries folder (next to the lvgl folder):

- Windows: `C:\Users\<username>\Documents\Arduino\libraries\lv_conf.h`
- macOS: `/Users/<username>/Documents/Arduino/libraries/lv_conf.h`
- Linux: `/home/<username>/Arduino/libraries/lv_conf.h`

The code is a single line after READY:

```cpp
#include <lvgl.h>
#include <usb_disp.h>

lv_display_t *lvd = usb_disp_lvgl_create(disp);   // the LVGL display is ready
// From here it is normal LVGL. Call lv_timer_handler() in loop().
```

-> See the `LVGL_Demo` example.


## 5. Pico-specific topics

### About core1

On the Pico, the USB host runs **permanently on core1** (started by `usb_disp_start()`). 
If your sketch does not use `setup1()`/`loop1()`, you do not need to think about it.

If your sketch does use `setup1()`/`loop1()`, core1 belongs to the sketch, so the library automatically switches to "manual mode". 
In that case, keep calling the service function from loop1:

```cpp
void setup1() {}
void loop1() { usb_disp_task(); }   // Just this. Do not mix in other work.
```

-> See the `TaskMode` example.

FreeRTOS mode (the "FreeRTOS SMP" menu option) also switches to manual mode. 
In that case add a delay - `loop1(){ usb_disp_task(); delay(1); }` - so that other tasks get CPU time.


### Clock

PIO USB requires sys_clk to be a multiple of 12 MHz. If it is not, the library **automatically adjusts it downward** (default 150 MHz -> 144 MHz). 
The core voltage (vreg) is never changed. If you want a higher clock, set it yourself at the top of setup() (e.g. 240 MHz is a multiple of 12, so it is used as-is).



## 6. Troubleshooting

| Symptom | Fix |
|---|---|
| Nothing is displayed                          | Call `usb_disp_set_log(true)` and watch the log. It shows enumeration progress |
| ESP32-S3: no device is ever detected, and/or the log spams `E HUB: Failed to issue root port reset` | Two known causes. (1) arduino-esp32 core **3.3.11** enables the usb_host enumeration-filter feature in its prebuilt libraries, and enumeration silently hangs when no filter callback is set - the current version of this library sets the callback and works on 3.3.11; if you hit this with an older copy of the library, update it (or use core 3.3.10). (2) After re-flashing the board, a bus-powered DL device (e.g. a DL-120 monitor) can be left wedged because the host vanished mid-operation - dev-board VBUS is hardwired, so software cannot power-cycle it: **physically replug the display** |
| The screen suddenly went black                | Unplugging/replugging on the monitor side can drop the output -> re-run `usb_disp_set_auto_mode()` (or set_mode) and redraw (this applies to both DL-1xx and MS913x) |
| Black screen at an unsupported resolution     | Switch back to a resolution the monitor supports with `usb_disp_set_mode()` |
| The resolution collapses to something tiny (e.g. 720x480) behind an HDMI analyzer / splitter / capture box / KVM | Such devices commonly list only 480p in their EDID base block and put 720p/1080p in the CEA extension block. The library reads extension blocks, so ask it with `usb_disp_edid_supports(d, w, h)` before demoting a mode yourself. If the device under-reports even there, force it: `usb_disp_add(port, dp, dm, w, h, true)` or `usb_disp_set_ignore_edid()` |
| Nothing works at all on Pico                  | Check the D+/D- wiring (adjacent GPIOs) and the 5V VBUS supply |
| Nothing works at all on Teensy                | Use the USB **host** port (4.1 = the 5-pin header / 4.0 = the bottom pads), not the micro-USB device port |
| No image when using setup1/loop1              | Make sure you are calling `loop1(){ usb_disp_task(); }` |



## 7. Examples

| Example | Description |
|---|---|
| ColorBars         | Minimal use of the raw API |
| DisplayInfo       | Showing MCU / chip / resolution / EDID / FPS on screen (the Web Flasher firmware; one photo of this screen makes a compatibility report) |
| FixedResolution   | Fixing the resolution (including `ignore_edid`) and changing it at runtime |
| LovyanGFX_Demo    | Using the library with LovyanGFX (not on Teensy - LovyanGFX has no i.MX RT support) |
| LVGL_Demo         | Using the library with LVGL v9 |
| MultiDisplay      | Driving two displays at once (Pico / Pico 2) |
| RGB888_Demo       | 24-bit color (RGB888): side-by-side gradients to compare banding against 16-bit, with runtime depth switching |
| ShadowFB          | Reducing transfer volume with differential updates (DL-1xx adapters only; ESP32 / Teensy 4.1 with PSRAM / PC) |
| TaskMode          | Coexisting with setup1()/loop1() (Pico / Pico 2) |





