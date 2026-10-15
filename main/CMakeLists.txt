# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS main.c catm.c clock.c config.c hangul.c keyboard_tab5.c keyboard_usb.c keys.c logs.c net.c power.c screen.c ssh.c storage.c terminal.c usb.c wifi.c
                       PRIV_REQUIRES c6 cjson ghostty shiroko wolfssh app_update esp_driver_gpio esp_driver_i2c esp_driver_ppa esp_lcd esp_mm m5stack_tab5_noglib usb_host_hid esp_driver_sdmmc esp_driver_uart esp_event esp_hal_usb esp_timer esp_netif esp_wifi fatfs nvs_flash sdmmc)
