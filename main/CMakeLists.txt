# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS main.c catm.c clock.c config.c hangul.c keyboard_tab5.c keyboard_usb.c keys.c layout.c logs.c memory_log.c net.c power.c screen.c settings.c ssh.c storage.c terminal.c ui.c ui_values.c usb.c wifi.c
                       EMBED_TXTFILES ui.json
                       LDFRAGMENTS linker.lf
                       PRIV_REQUIRES c6 cjson ghostty shiroko wolfssh app_update esp_driver_gpio esp_driver_i2c esp_driver_ppa esp_lcd esp_lcd_touch esp_mm m5stack_tab5_noglib usb_host_hid esp_driver_sdmmc esp_driver_uart esp_event esp_hal_usb esp_timer esp_netif esp_wifi fatfs nvs_flash sdmmc)
