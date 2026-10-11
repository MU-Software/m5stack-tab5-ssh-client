# SPDX-License-Identifier: GPL-3.0-or-later
# libghostty-vt from make ghostty
set(out ${CMAKE_CURRENT_LIST_DIR}/../../build/ghostty/out)
if(NOT EXISTS ${out}/lib/libghostty-vt.a)
  message(FATAL_ERROR "No libghostty-vt in ${out}: run make ghostty")
endif()
idf_component_register(INCLUDE_DIRS ${out}/include)
add_prebuilt_library(ghostty_vt ${out}/lib/libghostty-vt.a)
target_link_libraries(${COMPONENT_LIB} INTERFACE ghostty_vt)
