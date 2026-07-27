---
layout: default
title: Hardware List
nav_order: 4
---

# Hardware Compatibility List

Only a limited number of adapters can output Full HD (1920 x 1080 @ 60Hz).

**I recommend the DisplayLink DL-165 and DL-195.**



## Compatibility reports welcome

If you get one of the adapters marked "Untested" below working, I would be grateful if you could share the result - this is entirely voluntary, but every report makes this list more useful for everyone.

Please contact me via [this email address](https://pc-jp.net/){:target="_blank" rel="noopener noreferrer"} or on [X.com(@pcjpnet)](https://x.com/pcjpnet){:target="_blank" rel="noopener noreferrer"}.

When reporting, please include the following:

 1. Flash the test firmware by following the "[Quick Start]({{ '/' | relative_url }}#quick-start)" instructions on the [Home Page]({{ '/' | relative_url }}), and take a photo of the information shown on the screen.
 1. A photo of the disassembled adapter in which the chip's part number is readable.



## Supported Chip

The following chips are currently supported:


| Chip | Max Resolution | Supported Platforms |
|---|---|---|
| DisplayLink DL-120    | 1280x1024 / 1400x1050 | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-160    | 1600x1200 / 1680x1050 | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-115    | 1024x600              | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-125    | 1280x1024 / 1440x900  | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-165    | 1600x1200 / 1920x1080 | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-195    | 1920x1200 / 2048x1152 | RP2040, RP2350, <br>ESP32-S2, ESP32-S3, ESP32-P4, <br>Teensy 4.x, PC |
| DisplayLink DL-3xxx   | -                     | Not supported. It is encrypted. |
| DisplayLink DL-4xxx   | -                     | Not supported. It is encrypted. |
| DisplayLink DL-5xxx   | -                     | Not supported. It is encrypted. |
| DisplayLink DL-6xxx   | -                     | Not supported. It is encrypted. |
| MCT Trigger 2         | 1600x1200 / 1680x1050 | Not supported. No public protocol information. |
| MCT Trigger 5         | 1920x1200             | Not tested. Hardware not yet acquired. |
| MCT Trigger 6         | 3840x2160 *           | ESP32-P4, PC |
| MacroSilicon MS912x   | 1920x1080             | ESP32-P4, PC |
| MacroSilicon MS913x   | 1920x1080             | ESP32-P4, PC |
| GUD Protocol          | -                     | Not supported. |
| Fresco Logic FL2000   | -                     | Not supported. NO FRAME BUFFER. |
| SiS + USB Controller  | -                     | Hardware is difficult to source. |
| SMSC / Microchip UFX6000     | -                     | Hardware is difficult to source. |
| SMSC / Microchip UFX7000     | -                     | Hardware is difficult to source. |
| Grain Media GM12U320  | -                     | Hardware is difficult to source. |
| STMicro SPEAr family  | -                     | Hardware is difficult to source. |


 - I recommend the DisplayLink DL-165 and DL-195.
 - DisplayLink chips from the third generation onwards use a fully encrypted communication process.
While some of the encryption appears to have been analyzed, this has not yet resulted in the ability to display an image. [https://github.com/FireBurn/vino-scripts](https://github.com/FireBurn/vino-scripts){:target="_blank" rel="noopener noreferrer"}
 - DisplayLink chips from the third generation onwards also require USB High-Speed communication. Even if the encryption were bypassed, support would be limited to the ESP32-P4 and the PC.
 - The MCT Trigger 6 chip requires USB High-Speed communication, and the video data must be JPEG-encoded. Support is limited to the ESP32-P4 and the PC.
 - The MCT Trigger 6 chip supports 4K resolution. However,the library's default build caps the mode width at 2048 (`USB_DISP_MAX_WIDTH`); 4K would need `-DUSB_DISP_MAX_WIDTH=3840` and far more RAM than an MCU has, so treat 4K as PC (libusb) territory.
 - The MacroSilicon MS91xx chips require USB High-Speed communication, so they are supported only on the ESP32-P4 and the PC.
 - Fresco Logic FL2000 chip does not have a frame buffer; it requires data input at the same rate as the output frame rate and functions solely as a simple interface converter. This is not practical for an MCU.





## USB Graphics Display Adapter


| Manufacturer | Model Number | Output | Chip | Status |
|---|---|---|---|---|
| Hewlett Packard       | [NL571AA / 584670-001]({{ '/assets/images/hardware/NL571AA.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}  | DVI   | **DL-165**    | ✅ Supported |
| Plugable              | UGA-125               | DVI   | DL-125?   | Untested |
| Plugable              | UGA-M165              | DVI   | DL-165?   | Untested |
| Plugable              | UGA-165               | DVI   | DL-165?   | Untested |
| Plugable              | USB-VGA-165           | VGA   | DL-165?   | Untested |
| Plugable              | UGA-2K-A              | DVI   | DL-195?   | Untested |
| StarTech              | USB2HDMI              | HDMI  | DL-165?   | Untested |
| StarTech              | USB2DVIPRO2           | DVI   | DL-195?   | Untested |
| StarTech              | USB2VGAPRO2           | VGA   | DL-195?   | Untested |
| WAVLINK               | [WL-UG17D1]({{ '/assets/images/hardware/WL-UG17D1.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}             | DVI   | **DL-165**   | ✅ Supported |
| WAVLINK               | WL-UG17D2             | DVI   | DL-165?   | Untested |
| Diamond Multimedia    | BVU160                | DVI   | DL-160?   | Untested |
| Diamond Multimedia    | BVU165                | DVI   | DL-165?   | Untested |
| Diamond Multimedia    | BVU165LT              | DVI   | DL-165?   | Untested |
| Diamond Multimedia    | BVU195                | DVI   | DL-195?   | Untested |
| Eaton / Tripp Lite    | U244-001-R            | DVI   | DL-165?   | Untested |
| Eaton / Tripp Lite    | U244-001-VGA          | VGA   | DL-165?   | Untested |
| Eaton / Tripp Lite    | U244-001-VGA-R        | VGA   | DL-165?   | Untested |
| Eaton / Tripp Lite    | U244-001-HDMI-R       | HDMI  | DL-165?   | Untested |
| IOGEAR                | GUC2015V              | VGA   | ?         | Untested |
| IOGEAR                | GUC2020DW6            | DVI   | ?         | Untested |
| IOGEAR                | GUC2025H              | HDMI  | ?         | Untested |
| EVGA                  | 100-U2-UV12           | DVI   | ?         | Untested |
| EVGA                  | 100-U2-UV16           | DVI   | ?         | Untested |
| EVGA                  | 100-U2-UV19           | DVI   | DL-195?   | Untested |
| Kensington            | K33907                | DVI   | ?         | Untested |
| Kensington            | K33928                | DVI   | ?         | Untested |
| Sewell                | SW-22857              | DVI   | ?         | Untested |
| SIIG                  | JU-DV0112-S1          | DVI   | ?         | Untested |
| Sabrent               | UGA-2K-195            | DVI   | DL-195?   | Untested |
| アイ・オー・データ    | [USB-RGB]({{ '/assets/images/hardware/USB-RGB.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | VGA   | DL-120    | ✅ Supported |
| アイ・オー・データ    | [USB-RGB/D]({{ '/assets/images/hardware/USB-RGB_D.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}             | DVI   | DL-160    | ✅ Supported |
| アイ・オー・データ    | [USB-RGB2]({{ '/assets/images/hardware/USB-RGB2.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}              | VGA   | DL-125    | ✅ Supported |
| アイ・オー・データ    | [USB-RGB/D2]({{ '/assets/images/hardware/USB-RGB_D2.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | DVI   | **DL-195**    | ✅ Supported |
| アイ・オー・データ    | USB-RGB/D2S           | DVI   | ?         | Untested |
| バッファロー          | [GX-DVI/U2]({{ '/assets/images/hardware/GX-DVI_U2.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}             | DVI   | DL-160    | ✅ Supported |
| バッファロー          | [GX-DVI/U2AI]({{ '/assets/images/hardware/GX-DVI_U2AI.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}           | DVI   | **DL-195**    | ✅ Supported[^GX-DVIU2AI] |
| バッファロー          | [GX-DVI/U2B]({{ '/assets/images/hardware/GX-DVI_U2B.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | DVI   | **DL-165**    | ✅ Supported |
| バッファロー          | [GX-DVI/U2C]({{ '/assets/images/hardware/GX-DVI_U2C.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | DVI   | **DL-195**    | ✅ Supported |
| バッファロー          | [GX-HDMI/U2]({{ '/assets/images/hardware/GX-HDMI_U2.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | HDMI  | **DL-165**    | ✅ Supported |
| エレコム / Logitec    | LDE-SX010U            | VGA   | ?         | Untested |
| エレコム / Logitec    | LDE-SX015U            | VGA   | ?         | Untested |
| エレコム / Logitec    | [LDE-WX015U]({{ '/assets/images/hardware/LDE-WX015U.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | DVI   | **DL-195**    | ✅ Supported |
| ラトックシステム      | [REX-USBDVI]({{ '/assets/images/hardware/REX-USBDVI.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | DVI   | DL-160    | ✅ Supported |
| ラトックシステム      | [REX-USBDVI2]({{ '/assets/images/hardware/REX-USBDVI2.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}           | DVI   | **DL-195**    | ✅ Supported |
| グリーンハウス        | [GH-USB-DVIA]({{ '/assets/images/hardware/GH-USB-DVIA.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}           | DVI   | **DL-195**    | ✅ Supported |
| グリーンハウス        | GH-USB-VGAFHD         | VGA   | ?         | Untested |
| エアリア              | [SD-U2VDH]({{ '/assets/images/hardware/SD-U2VDH.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}              | DVI   | **DL-165**    | ✅ Supported[^SD-U2VDH] |
| ノバック              | [NV-CV100UH]({{ '/assets/images/hardware/NV-CV100UH.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | HDMI  | **DL-165**    | ✅ Supported[^NV-CV100UH] |
| サンワサプライ        | [AD-USB23HD]({{ '/assets/images/hardware/AD-USB23HD.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}            | HDMI  | **DL-165**    | ✅ Supported |
| サンワサプライ        | AD-USB24VGA           | VGA   | ?         | Untested |
| サンワサプライ        | [500-KC007(N)]({{ '/assets/images/hardware/500-KC007.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}          | HDMI  | **DL-165**    | ✅ Supported |
| サンワサプライ        | 500-KC002(N)          | VGA   | ?         | Untested |
| SANKA / アユート      | KDU211                | VGA   | ?         | Untested |
| SANKA / アユート      | KDU221                | DVI   | ?         | Untested |
| SANKA / アユート      | [KDU231]({{ '/assets/images/hardware/KDU231.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}                | HDMI  | **DL-165**    | ✅ Supported |



[^GX-DVIU2AI]: When using the device on a Windows PC, the automatic driver installation feature interferes with operation. It does not work with WinUSB; please use libusbK or libusb0. libusbK is recommended as it is the more modern option.
[^SD-U2VDH]: Model number on the back: "FG-ADVI-D1-FLHD-1PBN-NA-AR01"
[^NV-CV100UH]: It has a built-in USB hub.





## Docking Station with built-in DisplayLink


| Manufacturer | Model Number | Output | Chip | Status |
|---|---|---|---|---|
| 東芝              | dynadock U (PA3575)           | VGA / DVI | ?         | Untested |
| 東芝              | dynadock U10 (PA3541)         | VGA / DVI | ?         | Untested |
| 東芝              | dynadock V10 (PA3778)         | DVI       | ?         | Untested |
| Lenovo            | 0A33942                       | DVI       | ?         | Untested |
| Kensington        | K33367                        | VGA       | ?         | Untested |
| Kensington        | K33415                        | VGA       | ?         | Untested |
| Kensington        | K33930                        | DVI       | ?         | Untested |
| Kensington        | K33926                        | DVI       | ?         | Untested |
| Kensington        | K33951                        | DVI       | ?         | Untested |
| Kensington        | K33955                        | DVI       | ?         | Untested |
| Targus            | ACP50                         | VGA       | ?         | Untested |
| Targus            | ACP50US                       | VGA       | ?         | Untested |
| Targus            | ACP51                         | DVI       | ?         | Untested |
| Targus            | ACP51US                       | DVI       | ?         | Untested |
| Targus            | ACP51USZ                      | DVI       | ?         | Untested |
| Plugable          | UD-160-A                      | DVI       | ?         | Untested |
| Hewlett Packard   | MultiSeat t100 Thin Client    | VGA       | ?         | Untested |
| Hewlett Packard   | MultiSeat t150 Thin Client    | VGA       | ?         | Untested |




## USB Monitor with built-in DisplayLink


| Manufacturer | Model Number | Size | Resolution | Chip | Status |
|---|---|---|---|---|---|
| Samsung       | LD190G            | 18.5  | 1360x768  | ?         | Untested |
| Samsung       | LD220G            | 21.5  | 1920x1080 | ?         | Untested |
| DoubleSight   | DS-90U            | 9.0   | 1024x600  | ?         | Untested |
| AOC           | e1649Fwu          | 15.6  | 1366x768  | ?         | Untested |
| センチュリー  | LCD-4300U         | 4.3   | 800x480   | ?         | Untested |
| センチュリー  | [LCD-8000U]({{ '/assets/images/hardware/LCD-8000U.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}         | 8.0   | 800x600   | DL-120    | ✅ Supported |
| センチュリー  | LCD-8000UD        | 8.0   | 800x600   | ?         | Untested[^LCD-8000UD] |
| センチュリー  | LCD-8000U2        | 8.0   | 800x600   | ?         | Untested |
| センチュリー  | LCD-8000U2B       | 8.0   | 800x600   | ?         | Untested |
| センチュリー  | LCD-8000U2W       | 8.0   | 800x600   | ?         | Untested |
| センチュリー  | LCD-8000U2BV2     | 8.0   | 800x600   | ?         | Untested |
| センチュリー  | LCD-8000U2WV2     | 8.0   | 800x600   | ?         | Untested |
| センチュリー  | LCD-10000U        | 10.1  | 1366x768  | ?         | Untested |
| Nanovision    | UM-710            | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-710S           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-720S           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-720F           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-730            | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-740            | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-740R           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-750            | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760            | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760R           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760RF          | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760C           | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760CF          | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760-OF         | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760C-OF        | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760CH-OF       | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-760C-SMK       | 7.0   | 800x480   | ?         | Untested |
| Nanovision    | UM-1000           | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1010A          | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1050           | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080           | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080-NB        | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080-OF        | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080C-G        | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080C-G-NB     | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080C-OF       | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080H          | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080H-NB       | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080H-OF       | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080CH-G       | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080CH-G-NB    | 10.1  | 1024x600  | ?         | Untested |
| Nanovision    | UM-1080CH-OF      | 10.1  | 1024x600  | ?         | Untested |


 - Note that depending on the model-number suffix, some Century products are USB 3.0 versions. These do not currently work with this library.


[^LCD-8000UD]: Features DVI output; can also be used as an adapter.






## Partially Supported and Unsupported Hardware


| Manufacturer | Model Number | Output | Chip | Status |
|---|---|---|---|---|
| Unknown (China)       | [MS9122]({{ '/assets/images/hardware/MS9122.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | HDMI  | MS9122      | ✅ Supported (ESP32-P4, PC only) |
| Unknown (China)       | [MS9132]({{ '/assets/images/hardware/MS9132.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | HDMI  | MS9132      | ✅ Supported (ESP32-P4, PC only) |
| StarTech              | [USB2VGAE3]({{ '/assets/images/hardware/USB2VGAE3.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | VGA  | T2-285A       | Not supported. |
| StarTech              | [USB2DVIE3]({{ '/assets/images/hardware/USB2DVIE3.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | DVI  | T2-285B       | Not supported. |
| j5create              | [JUA330]({{ '/assets/images/hardware/JUA330.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | DVI  | T6-688SL      | ✅ Supported (ESP32-P4, PC only) |
| j5create              | [JUA350]({{ '/assets/images/hardware/JUA350.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}               | HDMI  | T6-688SL      | ✅ Supported (ESP32-P4, PC only) |
| アイ・オー・データ    | [USB-RGB3/D]({{ '/assets/images/hardware/USB-RGB3_D.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}       | DVI   | DL-3100       | Not supported (DL-3xxx is encrypted) |
| アイネックス          | [AMC-USBHDA]({{ '/assets/images/hardware/AMC-USBHDA.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"}       | HDMI  | FL2000-100-DX | Not supported. NO FRAME BUFFER. | 
| Lenovo            | [M01060 (P/N: 45K1610, ASM: 51J0246, FRU: 51J0452)]({{ '/assets/images/hardware/M01060.jpg' | relative_url }}){:target="_blank" rel="noopener noreferrer"} | DVI       | STMicro SPEAr | Not supported. |







## Notes





