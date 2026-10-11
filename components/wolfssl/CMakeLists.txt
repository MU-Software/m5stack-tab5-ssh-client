# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES freertos heap PRIV_REQUIRES esp_timer esp_hw_support)
    return()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wolf.cmake)
# settings.h includes "FreeRTOS.h" without its directory under WOLFSSL_ESPIDF
idf_component_register(SRCS ${WOLFSSL_SOURCES}
                       INCLUDE_DIRS include ${WOLFSSL_DIR} $ENV{IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos
                       REQUIRES freertos heap PRIV_REQUIRES esp_timer esp_hw_support)
target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function -Wno-cpp)
