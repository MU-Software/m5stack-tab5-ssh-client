# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS main.c ../../../components/c6/c6_nvs_server.c INCLUDE_DIRS ../../../components/c6/include PRIV_REQUIRES app_update esp_driver_sdio esp_event esp_hosted esp_timer esp_wifi nvs_flash)
# main.c takes over esp_hosted's SDIO events and keeps the driver's send credits
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=sdio_slave_initialize" "-Wl,--wrap=xQueueCreateCountingSemaphore")
