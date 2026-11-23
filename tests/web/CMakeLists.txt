# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the web search plugin (offline) + the live probe tool. Owner: web task.
add_executable(tst_web tst_web.cpp)
target_link_libraries(tst_web PRIVATE mxnet_web Qt6::Test Qt6::Network)
target_compile_definitions(tst_web PRIVATE MX_WEB_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/data")
add_test(NAME web COMMAND tst_web)

add_executable(web_probe web_probe.cpp)
target_link_libraries(web_probe PRIVATE mxnet_web Qt6::Network)
