# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS main.c config.c ssh.c storage.c wifi.c
                       PRIV_REQUIRES c6 cjson wolfssh esp_driver_sdmmc esp_driver_usb_serial_jtag esp_event esp_netif esp_wifi fatfs nvs_flash sdmmc)
