# SPDX-License-Identifier: GPL-3.0-or-later
# One-shot P4 image that pushes the C6 firmware; same partition table as the app, so nvs and the fonts stay
cmake_minimum_required(VERSION 3.16)
get_filename_component(TOP ${CMAKE_CURRENT_LIST_DIR}/../../.. ABSOLUTE)
set(SDKCONFIG_DEFAULTS "${TOP}/sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
set(EXTRA_COMPONENT_DIRS ${TOP}/components/c6)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(tab5_c6_update)
