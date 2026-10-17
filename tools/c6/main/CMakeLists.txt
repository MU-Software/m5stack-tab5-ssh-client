# SPDX-License-Identifier: Apache-2.0
idf_component_register(SRCS main.c INCLUDE_DIRS ../../../components/c6/include PRIV_REQUIRES app_update esp_event esp_hosted nvs_flash)
