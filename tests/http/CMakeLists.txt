# SPDX-License-Identifier: GPL-3.0-or-later
# Tests of the HTTP plugin (in-process HTTP server) + the live probe tool. Owner: http task.
add_executable(tst_http tst_http.cpp test_server.h test_server.cpp mini_scheduler.h)
target_include_directories(tst_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src/net/http)
target_link_libraries(tst_http PRIVATE mxnet_http Qt6::Test Qt6::Network)
add_test(NAME http COMMAND tst_http)
set_tests_properties(http PROPERTIES TIMEOUT 300)

add_executable(http_probe http_probe.cpp mini_scheduler.h)
target_include_directories(http_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(http_probe PRIVATE mxnet_http Qt6::Network)
