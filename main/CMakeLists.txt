# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS main.c c6_nvs.c clock.c config.c control.c crash.c dns_message.c gesture.c hangul.c keyboard_tab5.c keyboard_usb.c keys.c layout.c logs.c lte.c memory_log.c modem.c network.c network_rules.c power.c probe.c remote.c screen.c settings.c sleep.c ssh.c sshd.c storage.c terminal.c tmux.c ui.c ui_values.c update.c usb.c wifi.c
                       EMBED_TXTFILES ui.json
                       LDFRAGMENTS linker.lf
                       PRIV_REQUIRES c6 cjson ghostty shiroko wolfssh app_update esp_driver_gpio esp_driver_i2c esp_driver_ppa esp_lcd esp_lcd_touch esp_mm m5stack_tab5_noglib usb_host_hid esp_driver_sdmmc esp_driver_uart esp_event esp_hal_usb esp_timer esp_netif esp_wifi fatfs nvs_flash sdmmc)
# wifi.c: the Wi-Fi interface keeps its address through a drop; crash.c: what a panic saw, kept for the next boot
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_netif_action_disconnected" "-Wl,--wrap=esp_panic_handler")
