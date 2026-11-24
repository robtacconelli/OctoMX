# SPDX-License-Identifier: GPL-3.0-or-later
# [str-1] the Streaming API page of the Settings (a real core: the API's own state) and a stream in
# the Transfers window
mx_ui_test(tst_str1)
target_link_libraries(tst_str1 PRIVATE mxcore Qt6::Network)
target_include_directories(tst_str1 PRIVATE ${CMAKE_SOURCE_DIR}/tests/engine ${CMAKE_SOURCE_DIR}/tests/core)
target_sources(tst_str1 PRIVATE ${CMAKE_SOURCE_DIR}/tests/engine/engine_fakes.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h)
target_compile_definitions(tst_str1 PRIVATE MX_STR1_SHOTS="${CMAKE_BINARY_DIR}/shots")
