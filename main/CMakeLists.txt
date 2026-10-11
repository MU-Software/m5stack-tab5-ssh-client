# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS main.c config.c ssh.c storage.c usb.c wifi.c
                       PRIV_REQUIRES c6 cjson wolfssh app_update esp_driver_sdmmc esp_event esp_hal_usb esp_timer esp_netif esp_wifi fatfs nvs_flash sdmmc)
