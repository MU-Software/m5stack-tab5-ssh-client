# SPDX-License-Identifier: GPL-3.0-or-later
if(CMAKE_BUILD_EARLY_EXPANSION)
    idf_component_register(REQUIRES wolfssl PRIV_REQUIRES lwip)
    return()
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wolf.cmake)
idf_component_register(SRCS ${WOLFSSH_SOURCES} INCLUDE_DIRS ${WOLFSSH_DIR} REQUIRES wolfssl PRIV_REQUIRES lwip)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function -Wno-cpp)
