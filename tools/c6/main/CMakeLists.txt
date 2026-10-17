# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS main.c INCLUDE_DIRS ../../../components/c6/include PRIV_REQUIRES app_update esp_driver_sdio esp_event esp_hosted nvs_flash)
# main.c takes over esp_hosted's SDIO events
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdio_slave_initialize")
