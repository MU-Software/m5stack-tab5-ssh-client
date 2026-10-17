# SPDX-License-Identifier: GPL-3.0-or-later
idf_component_register(SRCS c6.c c6_nvs_server.c INCLUDE_DIRS include PRIV_REQUIRES esp_lcd m5stack_tab5_noglib nvs_flash)
# c6.c restarts the C6's SDIO counters after each card init
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=eh_host_port_sdio_card_init")
