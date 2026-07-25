---
layout: default
title: Protocol Notes
nav_order: 6
---

# USB Display Adapter Protocol Notes

This page documents the wire protocols of the display chips supported by Pico_USB_Disp. 
None of these chips have official public documentation; the information here comes from open-source drivers, from published reverse-engineering work, and from this project's own experiments on real hardware.


Primary sources:

- **DisplayLink DL-1xx**: the Linux kernel framebuffer driver [udlfb](https://github.com/torvalds/linux/blob/master/drivers/video/fbdev/udlfb.c){:target="_blank" rel="noopener noreferrer"} (GPL-2.0)
- **MacroSilicon MS912x**: [rhgndf/ms912x](https://github.com/rhgndf/ms912x){:target="_blank" rel="noopener noreferrer"} (GPL-2.0, based on Wireshark capture analysis)
- **MacroSilicon MS913x**: [fathonix/ms9132-drm-linux](https://github.com/fathonix/ms9132-drm-linux){:target="_blank" rel="noopener noreferrer"} (MacroSilicon's official Linux DRM driver, GPL-2.0)
- **MCT Trigger 6**: [cyrozap/mct-usb-display-adapter-re](https://github.com/cyrozap/mct-usb-display-adapter-re){:target="_blank" rel="noopener noreferrer"} (protocol RE) and [mcttrigger/triggerdm](https://github.com/mcttrigger/triggerdm){:target="_blank" rel="noopener noreferrer"} (MCT's official ChromeOS driver, GPL-2.0)

The library contains no code from these drivers - each protocol layer was implemented from scratch using them as references.



## Overview and comparison

|                                   | DL-1x0 / DL-1x5               | MS912x                    | MS913x                        | MCT Trigger 6 |
|---|---|---|---|---|
| USB speed required                | Full-Speed OK                 | High-Speed                | High-Speed                    | High-Speed |
| Control channel                   | Vendor control requests       | HID Feature reports       | HID Feature reports           | Vendor control requests |
| Pixel format on the wire          | RGB565 (BE), RLE compressed<br>(24 bpp: + an 8 bpp plane) | UYVY 4:2:2                | RGB24 (B,G,R)                 | Baseline JPEG |
| Partial (rectangle) updates       | Yes (any address)             | Yes (16 px granularity)   | No (full frames only)         | Yes (CLIP command) |
| Image persists without traffic    | Yes (indefinitely)            | Yes                       | **No** (~12 s, then blank)    | Yes |
| Keepalive needed                  | No                            | No                        | Yes (resend every ~2 s)       | No |


Because DL-1xx works over Full-Speed USB and supports partial updates with a persistent frame buffer, it is the only family usable on the Pico's PIO host and the ESP32-S2/S3. 
The other three require a High-Speed host (ESP32-P4 or PC).

Note: the partial-update rows above describe the chips/protocols themselves. This library's MS912x/MS913x and T6 backends currently always send **full frames**; only the DL-1xx backend performs partial (span) updates on the wire.



---

## 1. DisplayLink DL-1x5 family (DL-115 / DL-125 / DL-165 / DL-195)

Generation-1.5 DisplayLink chips ("Ollie", USB 2.0 era), VID `0x17E9`. The generation-1.0 DL-1x0 family ("Alex", DL-120 / DL-160) speaks the same protocol.

### 1.1 USB configuration

- One vendor-class (0xFF) interface
- **Bulk OUT EP 0x01**: command / pixel stream
- Interrupt IN EP (unused by this library)
- The same configuration is exposed when the device falls back to Full-Speed (verified on real hardware) - this is what makes Pico support possible.


### 1.2 Control requests

| bmRequestType | bRequest | wValue | wIndex | Data | Purpose |
|---|---|---|---|---|---|
| 0x40 | 0x12 | 0           | 0     | 16-byte key   | Channel selection (the standard key disables encryption) |
| 0xC0 | 0x02 | i \<\< 8    | 0xA1  | 2 bytes in    | EDID read (one byte at a time; `buf[1]` holds the value) |


The standard channel key:

```
57 CD DC A7 1C 88 5E 15 60 FE C6 97 16 3D 47 F2
```


### 1.3 Bulk command stream

Every command sent to the bulk OUT endpoint starts with `0xAF`:

| Command | Format | Meaning |
|---|---|---|
| `AF 20 reg val`                   | 4 bytes   | Register write |
| `AF 6B addr[3] count data...`     | variable  | RLE-compressed pixel write, base16 plane (RGB565) |
| `AF 60 addr[3] count data...`     | variable  | Raw (uncompressed) write, base8 plane (24 bpp only) |
| `AF 6A dst[3] count src[3]`       | 9 bytes   | On-screen rectangle copy, base16 plane |
| `AF 62 dst[3] count src[3]`       | 9 bytes   | On-screen rectangle copy, base8 plane (24 bpp only) |
| `AF A0`                           | 2 bytes   | Flush - forces execution of buffered commands |
| repeated `AF`                     | -         | Padding (no-op) |

All addresses are **byte** addresses into the device frame buffer, and every `count` field encodes 256 as `0`.


### 1.4 Video registers

Register writes are wrapped in a lock sequence using register `0xFF`:
write `0xFF <- 0x00` (lock) -> set the registers -> write `0xFF <- 0xFF` (unlock = apply).

Timing register values are not raw numbers but **16-bit LFSR counts**: the value obtained by stepping an LFSR (taps 15, 4, 2, 1) N times from the initial value 0xFFFF.


| Register | Encoding | Contents |
|---|---|---|
| 0x00          | 0x00      | Color depth (0 = 16 bpp, 1 = 24 bpp - see [Section 1.6](#16-24-bpp-mode-dual-plane)) |
| 0x01 / 0x03   | LFSR16    | Horizontal display start / end (from sync start: xds = HBP + HSYNC) |
| 0x05 / 0x07   | LFSR16    | Vertical display start / end |
| 0x09          | LFSR16    | Horizontal total - 1 |
| 0x0B          | LFSR16(1) | Horizontal sync start |
| 0x0D          | LFSR16    | Horizontal sync end (HSYNC + 1) |
| 0x0F          | raw BE    | Horizontal pixel count |
| 0x11          | LFSR16    | Vertical total |
| 0x13          | LFSR16(0) | Vertical sync start |
| 0x15          | LFSR16    | Vertical sync end (VSYNC) |
| 0x17          | raw BE    | Vertical line count |
| 0x1B          | raw LE    | Pixel clock / 5 kHz |
| 0x1F          | -         | Blanking (0x00 = display on, 0x01-0x07 = blanking variants) |
| 0x20-0x22     | 24-bit    | base16 plane start address (set to 0) |
| 0x26-0x28     | 24-bit    | base8 plane start address (set to `width * height * 2` = end of the base16 plane) |


### 1.5 RLE pixel write (`AF 6B`)


```
AF 6B  A2 A1 A0  N  { <raw_cnt> <pixel_BE16>... | <repeat_cnt> } ...
```


- `A2 A1 A0`: byte address in the device frame buffer (`(y * xres + x) * 2`)
- `N`: total pixel count of this command (max 256; 256 is encoded as 0)
- Pixels are RGB565 **big-endian**. This command writes the base16 plane; in 24 bpp mode the discarded low-order bits are written separately (see [Section 1.6](#16-24-bpp-mode-dual-plane))
- Encoding alternates between raw runs (`raw_cnt` literal pixels) and repeat runs (`repeat_cnt` = number of extra repetitions of the previous pixel)
- A solid-color run writes 256 pixels in a single 10-byte command - `AF 6B 00 00 00 00 01 F8 00 FF` paints 256 red pixels, versus 512 bytes raw (~51x). Worst case (every pixel different) is 519 bytes, i.e. a slight expansion over raw

Because the write address is arbitrary, **partial updates are possible**, which pairs very well with dirty-rectangle-based UIs such as LVGL.


### 1.6 24 bpp mode (dual plane)

Writing `0x01` to register `0x00` switches the chip to 24 bpp, in which the frame buffer becomes **two separate planes**:

| Plane | Start address | Bytes/px | Contents | Write | Copy |
|---|---|---|---|---|---|
| base16 | 0                    | 2 | RGB565 (BE) - identical to 16 bpp mode | `AF 6B` (RLE)   | `AF 6A` |
| base8  | `width * height * 2` | 1 | the low-order bits that RGB565 discards | `AF 60` (raw)   | `AF 62` |

The base8 byte carries exactly the bits lost to 565 quantization:

```
bit   7  6  5    4  3    2  1  0
      R2 R1 R0   G1 G0   B2 B1 B0
```

Combining the planes yields R = 5+3, G = 6+2, B = 5+3 - a full RGB888 pixel. 
This split is the same one used by libdlo's `dlo_grfx.c` (`DLO_RG16` / `DLO_GB16` / `DLO_RGB8`).

Practical notes:

- The base8 plane has **no compression** (`AF 60` is a raw write), so only the base16 half benefits from RLE. A 24 bpp update therefore costs roughly 1.5x its 16 bpp equivalent, which is why 16 bpp remains the default.
- The base8 start address is programmed into registers `0x26-0x28` as part of the mode set, so switching depth requires re-running the mode sequence.
- The two planes must be kept consistent: an on-screen rectangle copy in 24 bpp issues `AF 6A` for base16 **and** `AF 62` for base8.
- Verified on real DL-165 hardware.


### 1.7 Display persistence and HPD disturbances

Verified on a real DL-165: after mode setup and drawing, bulk traffic can stop completely and the chip keeps scanning out from its internal frame buffer - **the image was still displayed after 5 minutes of total silence** (zero USB bandwidth while idle; no keepalive of any kind is required).

However, an HPD-like event on the monitor side (monitor unplugged or a capture device closing) can drop the video output; the screen goes black and stays black. 
Pixel writes alone do not bring it back - **re-writing the full mode register sequence ([Section 1.4](#14-video-registers)) restores the output immediately**. 
This is why the library re-runs mode setup when recovery is needed, and why the usage page recommends `usb_disp_set_auto_mode()` when the screen unexpectedly goes black.


---

## 2. MacroSilicon MS912x family

USB2-generation USB->HDMI/VGA adapter chips.
The USB3 generation (MS913x) uses a **different** protocol - see [Section 3](#3-macrosilicon-ms913x-family).

### 2.1 USB configuration

- HID interface (control) + vendor interface with **bulk OUT EP 0x04** (video)
- Note: the USB3-generation chips expose a degraded HID-only configuration when they fall back to Full-Speed, so video is impossible from an FS host.
  This is one reason MS91xx support is limited to High-Speed hosts.


### 2.2 Register access (via HID class requests)

Control uses 8-byte HID Feature reports (wValue = 0x0300):

- Write: `SET_REPORT(0x21, 0x09, 0x0300, 0)` with data `[A6 addr d0..d5]`
- Read: `SET_REPORT` with `[B5 addrH addrL 0...]`, then `GET_REPORT(0xA1, 0x01, 0x0300, 0)` returns 8 bytes -> `data[3]` is the value

| Register | Purpose |
|---|---|
| 0x01 (write6)             | Resolution: width / height / pixfmt (each BE16) |
| 0x02 (write6)             | Mode number: mode / width / height (each BE16) |
| 0x03, 0x04, 0x05 (write6) | Mode-set sequence control |
| 0x07 (write6)             | Power: `[01 02 00...]` = ON, all zero = OFF |
| 0x32 (read)               | Monitor connection state (1 = connected) |
| 0xC000+ (read)            | EDID (one byte at a time) |


### 2.3 Mode-set sequence

 1. `0x04 <- [00...]` (stop output)
 1. Read `0x30`, `0x33`, `0xC620` (status check)
 1. `0x03 <- [03 00...]`
 1. `0x01 <- [wH wL hH hL fmtH fmtL]` (pixfmt: `0x2200` = UYVY)
 1. `0x02 <- [mH mL wH wL hH hL]` (the mode number is a fixed value per resolution)
 1. `0x04 <- [01 00...]`, `0x05 <- [01 00...]` (start output)


Common mode numbers: 640x480 = `0x4000`, 800x600 = `0x4200`, 1024x768 = `0x4700`, 1280x720 = `0x4F00`, 1280x1024 = `0x6000`, 1600x1200 = `0x7300`, 1920x1080 = `0x8100`.

When changing modes at runtime, power off (`0x07 <- all zero`) first, then power on and set the new resolution - skipping the power cycle leaves the display timing and frame buffer width inconsistent (garbled output).


### 2.4 Frame transfer (bulk OUT EP 0x04)

Each rectangle update is sent as:

```
Header 8B : FF 00 <x/16> <yH> <yL> <w/16> <hH> <hL>
Data      : UYVY 4:2:2 (2 bytes/pixel; 2 pixels = [U Y0 V Y1])
EOF 8B    : FF C0 00 00 00 00 00 00
```

- x and width are in **16-pixel units**
- Color conversion is BT.601 limited range
- No compression: one Full HD frame = 4.15 MB

Quirks verified on real hardware:

- **Double buffering**: each transfer (EOF marker) flips the chip's two internal buffers, and a rectangle lands only in the back buffer. 
  Send each rectangle **twice** (or send the union of the last two damage regions, like the DRM driver does) so both buffers stay consistent. 
  (This library sidesteps the issue by always sending the full frame - every flip fully repaints the back buffer, so no two-pass trick is needed.)
- **Persistence**: like DL-1xx, the image is retained after traffic stops (verified still displayed 2 minutes after stopping). No keepalive needed.


---

## 3. MacroSilicon MS913x family

USB3-generation chips. 
Despite the similar name, this is a different protocol from MS912x: control is a command set carried over the same HID Feature transport, and video is uncompressed RGB24 full frames.


### 3.1 Control commands (op = 0xA6, second byte = sub_op)

Register read/write uses the same `0xB5` / `0xB6` HID Feature mechanism as MS912x. Video control uses these:

```
sub_op 0x00 TRIGGER_FRAME  [A6 00 index delay(=100) 0 0 0 0]
sub_op 0x01 IN_INFO        [A6 01 wH wL hH hL color_in byte_sel]
sub_op 0x02 OUT_INFO       [A6 02 index(=vic) color_out wH wL hH hL]
sub_op 0x03 TRANS_MODE     [A6 03 mode(0=FRAME) 0 0 0 0 0]
sub_op 0x04 TRANSFER       [A6 04 enable 0 0 0 0 0]
sub_op 0x05 VIDEO_ENABLE   [A6 05 enable 0 0 0 0 0]
sub_op 0x07 POWER          [A6 07 on data(=2) 0 0 0 0]
```

- `color_in = 0x21` (RGB888 in, YUV422 out). The chip performs the RGB->YUV conversion; the host sends packed 24 bpp. RGB565 input is listed in the driver enums but does not actually work.
- `vic` is a mode code; the values used are 1280x720@60 = **79** (matches the high byte of the MS912x mode number `0x4F00`) and 1920x1080@60 = **16** (the standard CEA-861 VIC for 1080p60).
- Important registers: `0x32` = HPD, `0xC000+` = EDID, `0xD003` = current frame index, `0xFB07` bit1 = HDMI TX mute.


### 3.2 Frame transfer (bulk OUT EP 0x04)

```
frame_index = read(0xD003) ? 1 : 0        // initialize once from current value
loop:
    send the full frame as RGB24 (w*h*3 bytes, byte order B,G,R)
    send a zero-length packet (ZLP)        // frame delimiter
    frame_index ^= 1
    TRIGGER_FRAME(frame_index, delay=100)
    first frame only: VIDEO_ENABLE(1) + HDMI unmute
```

The chip converts RGB24 -> YUV422 and drives the HDMI output. Only full-frame mode works; partial updates are not available.


### 3.3 Keepalive requirement

Unlike every other chip on this page, the MS913x **blanks the HDMI output roughly 12 seconds after the last frame** (registers stay normal; only the output stops). 
MacroSilicon's official driver resends the previous frame every 2 seconds even without screen updates, and the library does the same from its poll function. After a blank, resending frames is not enough - the library must also re-issue `VIDEO_ENABLE(1)` and the HDMI unmute.


---

## 4. MCT Trigger 6 (T6)

Magic Control Technology's USB 3.x display chip, found in adapters such as the j5create JUA350 (current revision) and StarTech USB32HDES Rev2.
Note that Trigger 2 / 5 / 6 are three completely different protocols; only T6 is supported by this library.

The distinguishing feature of T6: **every frame is a complete baseline JPEG file**. 
The chip contains a hardware JPEG decoder and decodes into VRAM. 
On the ESP32-P4 the library pairs this with the P4's hardware JPEG *encoder*, which is what makes 1080p60-class output possible from a microcontroller.


### 4.1 USB configuration

- One vendor-class interface, `bcdUSB 3.10`, three endpoints:
  - EP 0x81 IN bulk (unused by this library)
  - **EP 0x02 OUT bulk** - video / audio / firmware sessions
  - EP 0x83 IN interrupt, 64 B - status events
- Works over High-Speed (512 B bulk); Full-Speed lacks the bandwidth by two orders of magnitude, so Pico-class hosts cannot drive it.


### 4.2 Main control requests (vendor, `0x40` OUT / `0xC0` IN)

| bmReq | bReq | Purpose |
|---|---|---|
| 0x40 | 0x03 | Video output ON/OFF (wValue = output index, wIndex = 0/1) |
| 0x40 | 0x12 | **Mode set** - 32-byte timing structure, little-endian |
| 0x40 | 0x31 | SOFTWARE_READY - sent after a mode set, before enabling output |
| 0xC0 | 0x80 | EDID block read (128 B) |
| 0xC0 | 0x84 | Number of supported modes (first 2 bytes, little-endian) |
| 0xC0 | 0x87 | Connector state (0 = disconnected, 1 = connected) |
| 0xC0 | 0x88 | VRAM size in MB |
| 0xC0 | 0x89 | Supported mode array (same 32 B structure as 0x12; byte offset via wIndex) |
| 0xC0 | 0xB0 | Adapter info (hardware type, firmware versions, serial) |

The 32-byte mode structure contains the pixel clock (kHz), refresh rate, full horizontal/vertical timing, PLL parameters, and sync polarities - all fields are known.


### 4.3 Video path (bulk OUT EP 0x02)

Each frame consists of two bulk transfers:

**1. Selector - a 32-byte `BULK_CMD_HEADER`** (all fields little-endian):

| Offset | Field | Value for video |
|---|---|---|
| 0     | Session number    | 0 = Video (3 = Audio, 5 = ROM/FW) |
| 4     | PayloadLength     | 48 + JPEG size + 1024 |
| 8     | PayloadAddress    | VRAM address where the JPEG bitstream is stored |
| 12    | PacketLength      | = PayloadLength when sent in one shot |
| 16    | PacketOffset      | 0 |
| 20    | PacketFlags       | 0 (0x01 = more fragments follow) |


**2. Session payload - a 48-byte `VIDEO_FLIP_HEADER` + JPEG file + 1024 zero bytes:**

| Offset | Field | Value for a full-frame flip |
|---|---|---|
| 0     | Command           | **3 = FLIP_PRIMARY** (1 = CLIP_PRIMARY for partial updates) |
| 4     | PayloadSize       | JPEG size + 1024 |
| 8     | FenceID           | 0 = no ACK; non-zero -> completion event on the interrupt EP |
| 12    | TargetFormat      | 6 = NV12 (decode target format) |
| 16    | Y/UV pitch        | ceil32(width) |
| 20    | Y data FB offset  | VRAM address of the decode target (= scanout source) |
| 24    | UV data offset    | Y offset + Y block size |
| 32    | SourceFormat      | 13 = JPEG |
| 47    | Flag byte         | 0x80 for the first ~10 frames after a mode set and whenever the command-ring address wraps back to 0; otherwise 0 |


A FLIP decodes the JPEG into the given frame-buffer address as NV12 and switches the scanout source to it. 
The scanout then keeps displaying that buffer - like DL-1xx, **no keepalive is needed** and the image persists without further traffic.

**Partial updates** use Command = 1 (CLIP): the JPEG rectangle is decoded into the currently displayed frame buffer without switching scanout. 
The rectangle position is encoded by adding `y * pitch + x` into the Y/UV offsets (the header's StartX/StartY fields are ignored by the hardware; the 16-byte alignment of the Y offset means x must be a multiple of 16). 
This was established by this project's own experiments - the official driver only ever uses FLIP, and this library also currently sends full-frame FLIPs only.

**JPEG requirements** (verified on hardware): a standard baseline JPEG file (SOI to EOI, JFIF APP0 allowed), 4:2:0 or 4:4:4 subsampling, quality 30-95 tested, dimensions need not be multiples of 8/16.


---

## 5. Chips that are not supported, and why

- **DisplayLink DL-3xxx and later**: the whole exchange is encrypted.
  Parts of the encryption have been analyzed publicly, but nobody has reached the point of displaying an image. These generations also require High-Speed USB.
- **MCT Trigger 2 / 5**: T2 has no public reverse-engineering work at all. T5 (non-compressed frames) is planned once hardware is obtained.
- **Microchip (SMSC) UFX6000 / UFX7000**: the hardware is difficult to source, so no analysis has been done.
- **Fresco Logic FL2000**: has **no frame buffer at all** - the host must stream pixels continuously at the output frame rate (it is essentially a USB-to-parallel-RGB converter). 
  At USB 2.0 speeds this limits it to 640x480, and a frame-buffer-less design does not fit this library's retained-image model on an MCU.





