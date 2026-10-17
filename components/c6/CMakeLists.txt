# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS c6.c c6_lease.c c6_nvs_server.c INCLUDE_DIRS include PRIV_REQUIRES esp_event esp_lcd m5stack_tab5_noglib nvs_flash)
# c6.c restarts the C6's SDIO counters after each card init, and holds transfers off for light sleep
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_port_sdio_card_init" "-Wl,--wrap=eh_host_port_sdio_read_reg" "-Wl,--wrap=eh_host_port_sdio_write_reg"
                      "-Wl,--wrap=eh_host_port_sdio_read_block" "-Wl,--wrap=eh_host_port_sdio_write_block")
