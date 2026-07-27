---
layout: default
title: API Reference
nav_order: 5
---

# Pico_USB_Disp - API Reference

All functions use C linkage and can be called from both C and C++.

The handle `usb_disp_t *` refers to one display managed by the library.



## Lifecycle

### `void usb_disp_init(void)`

Initializes the subsystem. Call once, first.


### `usb_disp_t *usb_disp_add(uint8_t port, uint8_t pin_dp, uint8_t pin_dm, uint16_t width = 0, uint16_t height = 0, bool ignore_edid = false)`

Registers a display port (call before `usb_disp_start()`).

- `port` - zero-based host controller number
  - Pico: PIO block number (RP2040 = 0..1 / RP2350 = 0..2; one port occupies one block)
  - ESP32: 0 only (the P4's second controller - USB 1.1 OTG FS - is not supported yet; support is planned)
  - Teensy 4.x: 0 only (the built-in USB host port)
  - PC: device number in discovery order
- `pin_dp` / `pin_dm` - GPIO numbers for D+ / D-. **Pico only**; the host pins are fixed on every other platform.
  - Pico: both must be given explicitly and must be adjacent GPIOs (reversed order is fine = swapped wiring is supported). Non-adjacent pins are a registration error and return NULL.
  - ESP32 / Teensy / PC: ignored, and **omittable in C++** - `usb_disp_add(0)` is enough. Passing pins anyway is harmless, so a sketch written for the Pico still builds unchanged on these platforms.
- `width` / `height` - resolution applied at connection time.
  **0, 0 (omitted) = automatic selection** (by default, the largest resolution the sink advertises in its EDID that fits the chip -> if the EDID is unreadable, the largest entry from the built-in list). See `edid_policy` below to select the sink's native/preferred timing instead. 
  If the requested mode cannot be set, the library also falls back to automatic selection, so check the actual resolution with `usb_disp_width/height()` after READY.
- `ignore_edid` - `true` = never read EDID; use `width`/`height` only. Same as `usb_disp_config_t.ignore_edid` (see below).
- Returns: the handle (NULL if the limit is reached or the pins are invalid)

```c
usb_disp_add(0, 16, 17);                    // automatic resolution
usb_disp_add(1, 19, 18, 800, 600);          // swapped wiring, fixed 800x600
usb_disp_add(0, 16, 17, 1920, 1080, true);  // force 1080p, ignore EDID

// ESP32 / Teensy / PC - the pin arguments can be dropped
usb_disp_add(0);                            // automatic resolution
usb_disp_add(0, 0, 0, 800, 600);            // fixed 800x600
```

> Resolution is the **4th and 5th** argument. `usb_disp_add(0, 800, 600)` puts 800 and 600 into the pin arguments (where they are ignored), silently leaving the resolution automatic - pad the pins with `0, 0` or use `usb_disp_add_cfg()`.

> C callers must pass all six arguments (the defaults are C++ only). Omitting the pins in a Pico build is a compile error (`too few arguments`), so the short form can never silently reach a target that actually needs the pins.

### `usb_disp_t *usb_disp_add_cfg(const usb_disp_config_t *cfg)`

Port registration via a struct. Use it for settings `usb_disp_add()` does not cover:


```c
typedef struct {
    uint8_t  port;         // host controller number (same as add)
    uint8_t  pin_dp;       // [Pico] GPIO for D+
    uint8_t  pin_dm;       // [Pico] GPIO for D- (must be adjacent)
    uint16_t width;        // 0 = automatic selection
    uint16_t height;
    uint8_t  refresh_hz;   // 0 = 60Hz
    bool     no_auto_mode; // true: skip automatic mode setup on connect
                           //       (the app calls the set_mode family itself)
    bool     shadow_fb;    // true: enable the shadow FB (differential updates) on connect
    bool     depth24;      // true: run in 24-bit color (RGB888)
    bool     ignore_edid;  // true: never read EDID; use width/height only
    usb_disp_edid_policy_t edid_policy;  // how automatic selection uses EDID
} usb_disp_config_t;
```


#### `edid_policy` - how automatic selection reads EDID

```c
typedef enum {
    USB_DISP_EDID_POLICY_MAX = 0,     // default
    USB_DISP_EDID_POLICY_PREFERRED,
} usb_disp_edid_policy_t;
```

| Policy | Behaviour |
|---|---|
| `MAX` (default)   | Of everything the EDID advertises - base DTDs, established and standard timings, and the CEA extension's VICs and DTDs - pick the **largest that fits the chip's pixel limit**. Anything beyond the adapter's `max_area` or `USB_DISP_MAX_WIDTH` is filtered out first, so a 4K TV lands on the largest mode the adapter can actually drive (usually 1080p). For DL adapters whose exact SKU cannot be identified, automatic selection additionally caps itself at the smallest SKU of the generation (unidentified DL-1x0: 1,470,000 px = DL-120 class / unidentified DL-1x5: 1,310,720 px = DL-115 class) to avoid picking a mode the chip cannot drive; an explicit `usb_disp_set_mode()` is not affected by this cap. |
| `PREFERRED`       | Use the base block's **preferred timing** (the sink's declared native resolution) and ignore extension blocks. |

`MAX` is the default because it keeps application layout simple: you can place widgets at fixed coordinates for a known resolution and let the display scale, instead of querying the size and re-laying-out. `PREFERRED` is there for the opposite case - driving a panel at exactly its native resolution so nothing is rescaled.

Both policies fall back to the built-in mode list (descending area, EDID-listed entries first) when the EDID is unreadable or nothing fits.

```c
usb_disp_set_edid_policy(d, USB_DISP_EDID_POLICY_PREFERRED);
usb_disp_set_auto_mode(d);   // apply it now (the setter alone does not re-select)
```

| Function | Description |
|---|---|
| `void usb_disp_set_edid_policy(d, policy)`        | Change the policy (before or after connection) |
| `usb_disp_edid_policy_t usb_disp_edid_policy(d)`  | Current policy |


#### `ignore_edid` - fixed resolution without EDID

By default the library reads the monitor's EDID and lets it influence the mode: the preferred timing is used for automatic selection, the built-in fallback list prefers modes the EDID lists, and a late-arriving EDID promotes the mode after READY.

That is the right behaviour for a real monitor, but it can still backfire behind a relay device - an HDMI analyzer, splitter, capture box, or KVM - whose EDID under-reports what it can display. (The library reads CEA extension blocks too, which covers most such devices; see *EDID extension blocks* below. `ignore_edid` is the escape hatch for the rest.)

Setting `ignore_edid = true` makes the library perform **no EDID access at all**: the mode comes from `width`/`height` (or, if those are 0, from the built-in list in descending area order), and none of the EDID-driven promotion/demotion paths run. An explicit `usb_disp_read_edid()` from your own code still works - the flag only stops the library from acting on EDID by itself.

```c
// via the struct...
usb_disp_config_t cfg = {};
cfg.port = 0; cfg.pin_dp = 16; cfg.pin_dm = 17;
cfg.width = 1920; cfg.height = 1080;
cfg.ignore_edid = true;          // force 1080p regardless of what EDID says
usb_disp_t *d = usb_disp_add_cfg(&cfg);

// ...or directly as the 6th argument of usb_disp_add()
usb_disp_t *d = usb_disp_add(0, 16, 17, 1920, 1080, true);
```

It can also be toggled at runtime:

| Function | Description |
|---|---|
| `void usb_disp_set_ignore_edid(d, bool on)`   | Same as the config field; may be called before or after connection. Enabling it does not change an already-selected mode - call `usb_disp_set_mode()` if you want to re-force one |
| `bool usb_disp_ignore_edid(d)`                | Current setting |


### `void usb_disp_start(void)`

Starts the host for all registered ports (call once, after the add calls).
On the Pico this takes up residence on core1. When the sketch defines `setup1()`/`loop1()` on arduino-pico, or a FreeRTOS menu option is selected, the library automatically switches to manual mode (below).


### `void usb_disp_start_manual(void)` / `void usb_disp_task(void)`

Manual service mode (only meaningful on the Pico). Call `start_manual()` instead of `usb_disp_start()`, then keep calling `usb_disp_task()` continuously from a context you can dedicate to it (arduino-pico's `loop1()` is the usual choice). 
On ESP32 / Teensy / PC, `start_manual` = `start` and `task` does nothing beyond housekeeping (empty implementations so shared code runs unchanged).


### `bool usb_disp_poll(usb_disp_t *d)`

Call periodically (every loop iteration is fine). Handles connection, disconnection, enumeration, and automatic mode setup. 
Returns **true on the transition to READY, on disconnect, or when a late-arriving EDID switches the mode after READY** (use it as a redraw trigger).


## Querying state

| Function | Description |
|---|---|
| `bool usb_disp_ready(d)`                              | Whether the display is connected and drawable |
| `uint16_t usb_disp_width(d)` / `usb_disp_height(d)`   | Current resolution (valid after READY) |
| `usb_disp_chip_t usb_disp_chip(d)`                    | Detected chip (e.g. USB_DISP_CHIP_DL165) |
| `const char *usb_disp_chip_name(d)`                   | Chip name string (e.g. "DL-165") |
| `const char *usb_disp_model(d)`                       | Model string for known products (e.g. "BUFFALO GX-DVI/U2"). NULL if not in the list (src/usb_disp_model.h) |
| `const char *usb_disp_model_name(vid, pid)`           | Same, but by VID/PID directly (no connection needed) |
| `uint32_t usb_disp_max_area(d)`                       | The chip's pixel-count limit (0 = unknown) |
| `uint16_t usb_disp_vid(d)` / `usb_disp_pid(d)`        | VID / PID of the connected device |
| `uint64_t usb_disp_stat_bytes(d)`                     | Cumulative bytes sent (for performance checks) |
| `usb_disp_udh_host_t *usb_disp_get_host(d)`           | **Pico only**: access to the PIO USB host's statistics (declared in `src/usb_disp_udh_host.h`; used by examples/DisplayInfo) |
| `uint8_t usb_disp_count(void)`                        | Number of registered ports |
| `usb_disp_t *usb_disp_at(uint8_t idx)`                | Handle at index idx |
| `uint8_t usb_disp_index(d)`                           | Handle -> index |

`usb_disp_chip_t` members (prefix `USB_DISP_CHIP_`): `UNKNOWN`, `DL120`, `DL160`, `DL1X0` (Gen 1.0, SKU unidentified), `DL115`, `DL125`, `DL165`, `DL195`, `DL1X5` (Gen 1.5, SKU unidentified), `T6`, `MS912X`, `MS913X`.



## Drawing (after READY)

For `update` and `fill`, coordinates that extend beyond the screen are clipped at the right and bottom edges (a fully off-screen rectangle draws nothing and returns true). `usb_disp_copy` does **not** clip - a rectangle that goes out of range is rejected and returns false.


### Rectangle updates (3 forms; use whichever you like)

```c
// Generic form: fmt selects the pixel format (in C++ fmt may be omitted = RGB565)
bool usb_disp_update(d, x, y, w, h, const void *px, stride_px,
                     usb_disp_fmt_t fmt);
// Fixed-format versions
bool usb_disp_update_565(d, x, y, w, h, const uint16_t *rgb565, stride_px);
bool usb_disp_update_888(d, x, y, w, h, const uint8_t *px, stride_px);
```

- `fmt` is `USB_DISP_FMT_RGB565` (2 B/px) or `USB_DISP_FMT_RGB888` (3 B/px, memory order B,G,R = same as LVGL's RGB888).
- `stride_px` is the number of pixels per row of the source array (`w` if it equals the rectangle width; 0 = repeat the same row). 
  This shape can be called directly from an LVGL flush_cb.
- The input format is independent of the display depth (usb_disp_set_depth); the library converts when they differ.



### `bool usb_disp_fill(d, x, y, w, h, color)`

Solid fill.

### `bool usb_disp_copy(d, sx, sy, dx, dy, w, h)`

Rectangle copy within the screen. Useful for scrolling. Overlap is handled automatically. Readback is not possible. 
On DL chips this maps to a native COPY command with no pixel retransmission; on T6 / MS912x / MS913x the copy happens in the internal frame buffer and the full frame is re-sent on the next transfer.


### `bool usb_disp_flush(d, timeout_ms)`

Pushes out buffered data. **Always call at the end of a batch of drawing** (the chip holds the last command until more data arrives).


### `bool usb_disp_blank(d, bool on)`

Blank / unblank the screen.


## Changing the resolution (mode)

All of these reject modes exceeding the chip's pixel-count limit and return false. 
Additional per-chip constraints: the **T6** only accepts modes present in the device's own mode table (width/height/refresh must match an entry), and **MS912x/MS913x** only support their known modes - 1920x1080 and 1280x720. 
Screen contents are undefined after a mode change, so redraw everything.

| Function | Description |
|---|---|
| `bool usb_disp_set_mode(d, w, h)`                             | Set a mode at 60Hz (built-in list first -> CVT-RB computation otherwise) |
| `bool usb_disp_set_mode_hz(d, w, h, hz)`                      | Same, with an explicit refresh rate |
| `bool usb_disp_set_mode_ex(d, const usb_disp_mode_t *m)`      | Fully specified timing (advanced) |
| `bool usb_disp_set_auto_mode(d)`                              | Redo automatic selection (EDID first) |
| `uint8_t usb_disp_builtin_mode_count(void)`                   | Number of built-in modes |
| `const usb_disp_mode_t *usb_disp_builtin_mode(uint8_t idx)`   | Get a built-in mode (largest area first) |
| `bool usb_disp_current_mode(d, usb_disp_mode_t *out)`         | Get the current mode including timing. false if not set |
| `uint16_t usb_disp_refresh_hz(d)`                             | Output refresh rate [Hz] (computed from the timing; independent of the MCU's drawing rate) |


```c
typedef struct {
    uint16_t width, height;    // active resolution
    uint32_t pclk_khz;         // pixel clock [kHz]
    uint16_t hfp, hsync, hbp;  // horizontal timing
    uint16_t vfp, vsync, vbp;  // vertical timing
} usb_disp_mode_t;
```



## 24-bit color (RGB888)

The default is 16-bit (RGB565). Switching to 24-bit displays gradients in photos etc. without banding (1.5x the bandwidth - suited to slow signage).

| Support | Chip | Method |
|---|---|---|
| ○ | DL-1x0 / DL-1x5  | 16+8 dual plane (verified on real DL-165 hardware) |
| ○ | MS913x           | RGB24 sent directly (no conversion) |
| ○ | MS912x           | The UYVY conversion source becomes 888 (better luma precision) |
| ○ | T6               | JPEG encoder with 888 input |


| Function | Description |
|---|---|
| `usb_disp_config_t.depth24 = true`                        | Run in 24-bit from connection time |
| `bool usb_disp_set_depth(d, bits)`                        | Switch between 16 / 24 (while READY this re-sets the mode = clears the screen; redraw required) |
| `uint8_t usb_disp_depth(d)`                               | Effective depth (requesting 24 on unsupported setups yields 16) |
| `bool usb_disp_update_888(d, x, y, w, h, px, stride_px)`  | Rectangle update. px = 3 B/px, memory order **B,G,R** (same as LVGL's RGB888) |


In either mode, both `usb_disp_update` (565) and `usb_disp_update_888` may be used (the library converts). 
Mixing is allowed - e.g. "GUI in 565, photo areas in 888". 
The shadow FB (differential updates) also works in 24-bit (allocated at 3 B/px).


From GUI libraries:
- LVGL: `usb_disp_lvgl_create(d, 16, LV_COLOR_FORMAT_RGB888)`
- LovyanGFX: `lcd.setColorDepth(24)` (before or after init)


## Shadow frame buffer (differential updates)

When enabled, update/fill compare against the previous screen contents and send **only the changed parts**. 
On screens with a lot of static content the transfer volume drops dramatically. 
The shadow FB applies to the span-update protocol (**DisplayLink DL-1xx**) only - full-frame protocols (T6 / MS912x / MS913x) always send whole frames, so it never activates there: `usb_disp_set_shadow()` returns false when called while READY, and a request made before connect is dropped at connection time with a "shadow FB not applicable" log. 
Memory is allocated from PSRAM on ESP32, from the add-on PSRAM (`extmem_malloc`) on Teensy 4.1, and from the heap on PC (not supported on Pico = always false; on a Teensy without PSRAM fitted it also stays disabled).


| Function | Description |
|---|---|
| `bool usb_disp_set_shadow(d, bool on)`            | Enable/disable (right after enabling, the screen is synchronized to black) |
| `bool usb_disp_shadow_active(d)`                  | Whether it is currently enabled |
| `const uint16_t *usb_disp_shadow_row(d, row)`     | Debug: pointer to a shadow row (NULL when disabled, and always NULL while running in 24-bit - the shadow is then stored at 3 B/px, which does not fit the uint16_t view) |


`usb_disp_config_t.shadow_fb = true` enables it automatically at connection time.



## Miscellaneous

| Function | Description |
|---|---|
| `uint16_t usb_disp_read_edid(d, buf, len)`            | Read the monitor's EDID from offset 0 (returns bytes read, 0 = failed) |
| `uint16_t usb_disp_read_edid_at(d, off, buf, len)`    | Same, from a byte offset - use it to fetch extension blocks |
| `uint16_t usb_disp_edid_size(d)`                      | Total EDID size including extension blocks (0 = unreadable / `ignore_edid`) |
| `bool usb_disp_edid_supports(d, w, h)`                | Whether the sink advertises this resolution anywhere in its EDID (see below) |
| `bool usb_disp_edid_max_mode(d, &w, &h)`              | Largest advertised resolution that fits the chip - what `EDID_POLICY_MAX` selects |
| `void usb_disp_force_reenum(d)`                       | Force re-enumeration (virtual replug) |
| `bool usb_disp_supported_device(vid, pid)`            | Whether this VID/PID is supported |


### EDID extension blocks

EDID is a 128-byte base block plus optional 128-byte extension blocks (the count is in byte 126). The library reads them up to `USB_DISP_EDID_MAX_BLOCKS` (default 2 blocks total = base + one extension; see *Limits and tuning* below) and, for CEA-861 extensions (tag `0x02`), parses both the detailed timings and the Video Data Block's short video descriptors (VIC codes).

This matters because a relay device - an HDMI analyzer, splitter, capture box, or KVM - commonly puts only 480p in its base block and everything else in the CEA extension. A real measured example (an analyzer reporting monitor name `VA-1838`):

```
base block  : preferred timing 720x480, established 640x480 only
CEA ext     : VIC 2* 1 3 4 5 16 17 18 19 20 31 6 7 21 22
              (VIC 4 = 1280x720p60, VIC 16 = 1920x1080p60)
              ext DTDs: 720x480, 1280x720, 1920x540(=1080i)
```

Reading only the base block makes such a device look like a 480p sink. `usb_disp_edid_supports(d, 1920, 1080)` returns true here because it sees VIC 16.

Use `usb_disp_edid_supports()` when your app wants to sanity-check a resolution before forcing it, instead of comparing against the base block's preferred timing. `usb_disp_edid_max_mode()` answers the softer question "will the sink cope with this?" - a 1080p monitor usually does not list 1280x720 anywhere in its EDID, yet it displays it fine, so compare against the advertised maximum rather than requiring an exact listing. Both may trigger a full EDID read (hundreds of control transfers on DL-1xx, which reads one byte per transfer), so call them on connect - not every frame.

Limits and tuning:

- `USB_DISP_EDID_MAX_BLOCKS` (default 2 = base + 1 extension) caps how many blocks are read and sizes the internal buffer (`128 * N` bytes). Raise it with `-DUSB_DISP_EDID_MAX_BLOCKS=n` if you have a sink with more than one extension block.
- **DL-1x0/1x5 can only read the first 256 bytes** - the chip's EDID register takes an 8-bit byte offset, so base + one extension is the hardware limit regardless of the setting. T6 and MS912x/MS913x have no such limit.
- Extension blocks are checksum-verified before use; a corrupted one is logged and ignored rather than parsed.

Extension data feeds the default `EDID_POLICY_MAX` selection, the fallback search, `usb_disp_edid_supports()` and `usb_disp_edid_max_mode()`. Note that interlaced detailed timings are skipped - CEA extensions often carry 1080i as a `1920x540` DTD, which would otherwise be programmed as a 540-line mode.


## Logging

By default nothing is printed.

| Function | Description |
|---|---|
| `void usb_disp_set_log(bool on)`              | Arduino: true sends the log to Serial |
| `void usb_disp_log(const char *fmt, ...)`     | Weak function. Define it in your app to completely replace the log destination (all platforms; set_log is then irrelevant) |


```cpp
// Example of replacing the log destination (just define it)
void usb_disp_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);   // e.g. to stderr
    va_end(ap); fprintf(stderr, "\n");
}
```


## Arduino auto-glue (no extra include)

`<usb_disp.h>` detects when your sketch includes the following libraries and enables the integration automatically:


### LovyanGFX -> the `LGFX_USB_Disp` class

```cpp
#include <LovyanGFX.hpp>
#include <usb_disp.h>
LGFX_USB_Disp lcd;
// After READY: lcd.setUsbDisp(d); lcd.init(); then normal LovyanGFX
```

Limitations: no readback / rotation fixed to 0. Color depth is 16 (default) or 24, selectable via `lcd.setColorDepth(24)`. 
Teensy 4.x requires LovyanGFX 1.2.27 or later (earlier releases do not support the i.MX RT).



### LVGL v9 -> `usb_disp_lvgl_create()`

```cpp
#include <lvgl.h>
#include <usb_disp.h>
// After READY:
lv_display_t *lvd = usb_disp_lvgl_create(d /*, buf_lines = 16 */);
// Returns with lv_init / tick / draw buffers / flush_cb already set up.
// Then just call lv_timer_handler() in loop().
```


## Compile-time configuration (advanced)

| Macro | Default | Description |
|---|---|---|
| `USB_DISP_MAX_WIDTH`          | 2048                                                      | Maximum supported horizontal resolution (line buffer length; can be reduced on RAM-constrained boards) |
| `USB_DISP_MAX`                | Pico = number of PIOs / PC = 4 / ESP32 = 1 / Teensy = 1   | Maximum number of simultaneous displays. Only tunable with `-D` on PC (libusb); fixed on ESP32/Teensy, and derived from `USB_DISP_UDH_MAX_PORTS` on the Pico |
| `USB_DISP_UDH_MAX_PORTS`      | RP2040 = 2 / RP2350 = 3                                   | Pico only: number of PIO USB host ports to provision. Each port reserves a 16 KB ring in .bss, so lowering it saves RAM on the RP2040 |
| `USB_DISP_EDID_MAX_BLOCKS`    | 2                                                         | How many 128-byte EDID blocks are read (base + extensions) and the size of the internal buffer (128 x N bytes). See *EDID extension blocks* above |
| `USB_DISP_PSRAM_LIMIT_KB`     | 0 (unlimited)                                             | ESP32 only: cap on the total PSRAM the library may allocate for frame buffers (shadow FB etc.) |
| `USB_DISP_TEENSY_HUBS`        | 2                                                         | Teensy only: number of USBHub instances for adapters behind a hub / dock (0 saves RAM) |
| `USB_DISP_TEENSY_SETTLE_MS`   | 1500                                                      | Teensy only: delay [ms] from connect detection to enumeration start. DL chips need 1-2 s of internal startup; enumerating earlier (USBHost_t36's default is 100 ms) wedges their EP0 (verified on DL-165) |
| `USB_DISP_ESP32_SETTLE_MS`    | 200                                                       | ESP32 only: disconnected period [ms] before the USB host stack starts (lets a DL chip left "warm" by an MCU reboot start fresh) |
| `USB_DISP_UDH_HS_DIAG`        | 0                                                         | Pico only, debug: classify and count handshake results in the PIO host (used by examples/DisplayInfo to distinguish "device not answering" from "receiving our own transmission") |

The platform-detection macros `USB_DISP_PORT_PICO` / `USB_DISP_PORT_ESP32` / `USB_DISP_PORT_TEENSY` / `USB_DISP_PORT_LIBUSB` are defined to 0/1 by `usb_disp.h` and can be used for conditional code in sketches that target several platforms (examples/DisplayInfo does this).








