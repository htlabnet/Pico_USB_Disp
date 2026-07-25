---
layout: default
title: Home
nav_order: 1
permalink: /
---

# Pico_USB_Disp



[![Pico_USB_Disp]({{ '/assets/images/pico-usb-display-library-1200px.jpg' | relative_url }})]({{ '/assets/images/pico-usb-display-library-2400px.jpg' | relative_url }})



Drive a monitor from your microcontroller with a USB graphics display adapter.

This library implements USB display protocols on resource-constrained embedded platforms without requiring Linux or a desktop operating system.

With a supported adapter, a Raspberry Pi Pico or an ESP32 board can provide Full HD display output (1920 × 1080 at 60 Hz) using only two USB data lines (D+ and D-) for communication.

Because the adapter generates a standards-compliant video signal, it generally provides broader monitor compatibility than implementations that use simplified DVI signaling, such as PicoDVI.



{% for item in site.aux_links %}
 - [{{ item[0] }}]({{ item[1][0] }})
{% endfor %}



## Quick Start

 1. Get a supported USB display adapter. DisplayLink DL-165 and DL-195 adapters are recommended. ([Click here for a list of supported hardware](hardware/))
 1. Use an [RP2040](https://learn.adafruit.com/adafruit-feather-rp2040-with-usb-type-a-host){:target="_blank" rel="noopener noreferrer"} or [RP2350](https://www.waveshare.com/wiki/RP2350-USB-A){:target="_blank" rel="noopener noreferrer"} board with a USB host connector, an ESP32-S2/S3/P4 board with a USB OTG connector (the [ESP32-S3](https://docs.espressif.com/projects/esp-dev-kits/en/latest/esp32s3/esp32-s3-devkitc-1/){:target="_blank" rel="noopener noreferrer"} is recommended), or a [Teensy 4.x](https://www.pjrc.com/store/teensy41.html){:target="_blank" rel="noopener noreferrer"} board with a USB host connector.
 1. Flash the firmware using the [Web Flasher](flasher/).
 1. Connect the display adapter to the board's USB host port. The firmware will initialize the adapter and start video output automatically.
 1. See [Library Usage](usage/) to integrate Pico_USB_Disp into your project and explore the examples.



## Overview

USB display adapters are normally designed for desktop operating systems and require proprietary or platform-specific drivers.

This library implements the required USB communication protocol directly on an MCU, allowing supported adapters to be used without Windows, macOS, or Linux.

 - Draw to a USB display with `#include <usb_disp.h>` and only a few lines of code.
 - Automatically select the clock and display mode from the monitor's EDID.
 - Automatic integration with LovyanGFX / LVGL v9.
 - Build with the Arduino IDE, Raspberry Pi Pico SDK, or ESP-IDF.
 - The library also supports Windows, Linux, and macOS. On these platforms, it communicates with the display adapter through libusb.



## Documentation

 - [Library Usage](usage/) - Installation, wiring, basic usage, integration with GUI libraries, troubleshooting, and examples.
 - [Web Flasher](flasher/) - Flash the test firmware directly from a supported browser.
 - [Hardware List](hardware/) - Find tested and potentially compatible adapters.
 - [API Reference](api/) - Function and configuration reference.
 - [Protocol Notes](protocol/) - Technical documentation for each chip's on-wire protocol.



## Third-Party Code

This project includes a modified subset of [sekigon-gonnoc/Pico-PIO-USB](https://github.com/sekigon-gonnoc/Pico-PIO-USB){:target="_blank" rel="noopener noreferrer"}.

All display-protocol implementations were written from scratch based on my own analysis and publicly available drivers. No source code from other display drivers is included.



## Credits

 - Author: Hideto Kikuchi / PJラボ ([@pcjpnet](https://x.com/pcjpnet){:target="_blank" rel="noopener noreferrer"}) - [https://pc-jp.net/](https://pc-jp.net/){:target="_blank" rel="noopener noreferrer"}



## License

MIT License.

See the LICENSE file in the [GitHub Repository]({{ site.aux_links["GitHub Repository"][0] }}) for details.



