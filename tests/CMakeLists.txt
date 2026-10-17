# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(tab5_ssh_tests C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(SANITIZE "address,undefined" CACHE STRING "-fsanitize= value")

include(../cmake/wolf.cmake)
add_compile_options(-g -O1 -fno-omit-frame-pointer -fsanitize=${SANITIZE} -fno-sanitize-recover=all -Wall -Wextra -Werror)
include_directories(${TOP}/main)
add_link_options(-fsanitize=${SANITIZE})

add_library(wolf STATIC ${WOLFSSL_SOURCES} ${WOLFSSH_SOURCES})
target_include_directories(wolf PUBLIC ${TOP}/components/wolfssl/include ${WOLFSSL_DIR} ${WOLFSSH_DIR})
target_compile_definitions(wolf PUBLIC WOLFSSL_USER_SETTINGS)
target_compile_options(wolf PRIVATE -w)

add_library(cjson STATIC ${TOP}/third_party/cjson/cJSON.c)
target_include_directories(cjson PUBLIC ${TOP}/third_party/cjson)
target_compile_options(cjson PRIVATE -w)

add_executable(config_test config_test.c ${TOP}/main/config.c)
target_link_libraries(config_test cjson wolf)

add_executable(hangul_test hangul_test.c ${TOP}/main/hangul.c)

add_executable(layout_test layout_test.c ${TOP}/main/layout.c)
target_link_libraries(layout_test cjson)

add_executable(modem_test modem_test.c ${TOP}/main/modem.c)

add_executable(settings_test settings_test.c ${TOP}/main/layout.c ${TOP}/main/settings.c)
target_link_libraries(settings_test cjson wolf)

add_executable(ssh_test ssh_test.c ${TOP}/main/config.c ${TOP}/main/ssh.c ${TOP}/main/tmux.c)
target_link_libraries(ssh_test cjson wolf)
