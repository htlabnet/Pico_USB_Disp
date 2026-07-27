# Pico_USB_Disp


Drive a monitor from your microcontroller with a USB graphics display adapter.

This library implements USB display protocols on resource-constrained embedded platforms without requiring Linux or a desktop operating system.

With a supported adapter, a Raspberry Pi Pico or an ESP32 board can provide Full HD display output (1920 × 1080 at 60 Hz) using only two USB data lines (D+ and D-) for communication.

Because the adapter generates a standards-compliant video signal, it generally provides broader monitor compatibility than implementations that use simplified DVI signaling, such as PicoDVI.




## Overview

See [introduction page](https://htlabnet.github.io/Pico_USB_Disp).

日本語での解説記事は[こちら](https://htlab.net/electronics/mcu/raspberrypi/pico-usb-display-library/)にあります。



## Installation

**Arduino** - Search for **Pico_USB_Disp** in the Library Manager and install it.

**ESP-IDF** - Add the component to your project:

```bash
idf.py add-dependency "htlabnet/pico_usb_disp^1.0.0"
```

See the [Library Usage](https://htlabnet.github.io/Pico_USB_Disp/usage/) for wiring, and code examples.





