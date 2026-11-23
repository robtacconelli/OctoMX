# SPDX-License-Identifier: GPL-3.0-or-later
# eD2k module tests (Qt Test) and the live-network probe tool. Owner: ed2k task.
find_package(ZLIB REQUIRED)

add_library(ed2k_test_support STATIC test_support.h test_support.cpp)
target_include_directories(ed2k_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ed2k_test_support PUBLIC mxapi Qt6::Core)

add_executable(tst_ed2k tst_ed2k.cpp)
target_link_libraries(tst_ed2k PRIVATE mxnet_ed2k ed2k_test_support Qt6::Test Qt6::Network ZLIB::ZLIB)
add_test(NAME ed2k_unit COMMAND tst_ed2k)
set_tests_properties(ed2k_unit PROPERTIES TIMEOUT 600)

add_executable(tst_kad tst_kad.cpp)
target_link_libraries(tst_kad PRIVATE mxnet_ed2k ed2k_test_support Qt6::Test Qt6::Network ZLIB::ZLIB)
add_test(NAME kad_unit COMMAND tst_kad)
set_tests_properties(kad_unit PROPERTIES TIMEOUT 900)

# [glue-3] upload queue control + bandwidth accounting over loopback (uses the core RateLimiter)
if(TARGET mxcore)
    add_executable(tst_ed2k_glue3 tst_ed2k_glue3.cpp)
    target_link_libraries(tst_ed2k_glue3 PRIVATE mxnet_ed2k mxcore ed2k_test_support Qt6::Test Qt6::Network ZLIB::ZLIB)
    add_test(NAME ed2k_glue3 COMMAND tst_ed2k_glue3)
    set_tests_properties(ed2k_glue3 PROPERTIES TIMEOUT 300)
endif()

# ed2k_probe: console tool exercising the plugin on the live eD2k network (its options are listed at the top of ed2k_probe.cpp)
add_executable(ed2k_probe ed2k_probe.cpp)
target_link_libraries(ed2k_probe PRIVATE mxnet_ed2k ed2k_test_support Qt6::Network)

# [up-1] the upload row counts the bytes of every connection of a client
add_executable(tst_ed2k_upload_rows tst_ed2k_upload_rows.cpp)
target_link_libraries(tst_ed2k_upload_rows PRIVATE mxnet_ed2k ed2k_test_support Qt6::Test Qt6::Network ZLIB::ZLIB)
add_test(NAME ed2k_upload_rows COMMAND tst_ed2k_upload_rows)
set_tests_properties(ed2k_upload_rows PROPERTIES TIMEOUT 300)
