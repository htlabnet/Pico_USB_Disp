
cmake_minimum_required(VERSION 3.13)


# ---- ESP-IDF ----
if(ESP_PLATFORM)
    # usb=usb_host / esp_timer=HALの時刻取得
    # T6 の HW JPEG エンコーダ (esp_driver_jpeg) は P4 のみ存在
    set(_pud_reqs usb esp_timer)
    if(IDF_TARGET STREQUAL "esp32p4")
        list(APPEND _pud_reqs esp_driver_jpeg)
    endif()
    idf_component_register(
        SRCS
            src/usb_disp.cpp
            src/usb_disp_prot_dl-1xx.cpp
            src/usb_disp_prot_t6.cpp
            src/usb_disp_prot_ms91xx.cpp
            src/usb_disp_hal_esp32.cpp
        INCLUDE_DIRS src
        REQUIRES ${_pud_reqs})
    return()
endif()


# ---- Pico-SDK / PC ----
project(pico_usb_disp C CXX)

set(_pud_src ${CMAKE_CURRENT_LIST_DIR}/src)

if(PICO_SDK_VERSION_STRING)
    # ---- Pico-SDK: INTERFACE ライブラリ ----
    add_library(pico_usb_disp INTERFACE)
    target_sources(pico_usb_disp INTERFACE
        ${_pud_src}/usb_disp.cpp
        ${_pud_src}/usb_disp_prot_dl-1xx.cpp
        ${_pud_src}/usb_disp_prot_t6.cpp      # HS ガードで空
        ${_pud_src}/usb_disp_prot_ms91xx.cpp  # HS ガードで空
        ${_pud_src}/usb_disp_hal_pico.cpp
        ${_pud_src}/usb_disp_udh_bus.cpp
        ${_pud_src}/usb_disp_udh_encode.cpp
        ${_pud_src}/usb_disp_udh_host.cpp
    )
    target_include_directories(pico_usb_disp INTERFACE ${_pud_src})
    target_link_libraries(pico_usb_disp INTERFACE
        pico_stdlib
        pico_multicore
        hardware_dma
        hardware_pio
    )
    # PIO ヘッダはソースツリー内の生成済み (src/pio/*.pio.h) を使う。
    # 再生成したい場合は利用側で:
    #   pico_generate_pio_header(<target> <lib>/src/pio/usb_tx.pio OUTPUT_DIR <lib>/src/pio)
else()
    # ---- PC (libusb) ----
    add_library(pico_usb_disp STATIC
        ${_pud_src}/usb_disp.cpp
        ${_pud_src}/usb_disp_prot_dl-1xx.cpp
        ${_pud_src}/usb_disp_prot_t6.cpp
        ${_pud_src}/usb_disp_prot_ms91xx.cpp
        ${_pud_src}/usb_disp_hal_libusb.cpp
    )
    target_include_directories(pico_usb_disp PUBLIC ${_pud_src})
    if(PICO_USB_DISP_LIBUSB_INCLUDE)
        target_include_directories(pico_usb_disp PUBLIC ${PICO_USB_DISP_LIBUSB_INCLUDE})
    endif()
    if(PICO_USB_DISP_LIBUSB_LIB)
        target_link_libraries(pico_usb_disp PUBLIC ${PICO_USB_DISP_LIBUSB_LIB})
    endif()
endif()


