# SPDX-License-Identifier: GPL-3.0-or-later
# Gnutella module tests (Qt Test) and the live-network probe. Owner: gnutella task.
add_library(gnutella_test_support STATIC test_support.h test_support.cpp)
target_include_directories(gnutella_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(gnutella_test_support PUBLIC mxapi mxhash Qt6::Core)

add_executable(tst_gnutella_unit tst_gnutella_unit.cpp)
target_link_libraries(tst_gnutella_unit PRIVATE mxnet_gnutella Qt6::Test)
add_test(NAME gnutella_unit COMMAND tst_gnutella_unit)
set_tests_properties(gnutella_unit PROPERTIES TIMEOUT 300)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tst_gnutella_loopback.cpp)
    add_executable(tst_gnutella_loopback tst_gnutella_loopback.cpp)
    target_link_libraries(tst_gnutella_loopback PRIVATE mxnet_gnutella gnutella_test_support Qt6::Test Qt6::Network)
    add_test(NAME gnutella_loopback COMMAND tst_gnutella_loopback)
    set_tests_properties(gnutella_loopback PROPERTIES TIMEOUT 600)
endif()

# [glue-3] upload queue control + bandwidth accounting over loopback HTTP (uses the core RateLimiter)
if(TARGET mxcore AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tst_gnutella_glue3.cpp)
    add_executable(tst_gnutella_glue3 tst_gnutella_glue3.cpp)
    target_link_libraries(tst_gnutella_glue3 PRIVATE mxnet_gnutella mxcore gnutella_test_support Qt6::Test Qt6::Network)
    add_test(NAME gnutella_glue3 COMMAND tst_gnutella_glue3)
    set_tests_properties(gnutella_glue3 PROPERTIES TIMEOUT 300)
endif()

# gnutella_probe: console tool exercising the plugin on the live Gnutella network
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/gnutella_probe.cpp)
    add_executable(gnutella_probe gnutella_probe.cpp)
    target_link_libraries(gnutella_probe PRIVATE mxnet_gnutella gnutella_test_support Qt6::Network)
endif()

# [enc-1] TLS layer: TlsSocket policies, anonymous-DH client, certificate, TLS flags on the wire
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tst_gnutella_tls.cpp)
    find_package(OpenSSL REQUIRED)
    add_executable(tst_gnutella_tls tst_gnutella_tls.cpp)
    target_link_libraries(tst_gnutella_tls PRIVATE mxnet_gnutella Qt6::Test Qt6::Network OpenSSL::SSL OpenSSL::Crypto)
    add_test(NAME gnutella_tls COMMAND tst_gnutella_tls)
    set_tests_properties(gnutella_tls PROPERTIES TIMEOUT 300)
endif()
