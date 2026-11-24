# SPDX-License-Identifier: GPL-3.0-or-later
# [net-2] the connection policy (proxy, chosen adapter, IPv6) observed on every network: a local
# SOCKS5 / HTTP CONNECT proxy, canaries that count connections and datagrams per source address,
# and a second loopback address as the "chosen adapter" (see tst_netpolicy.cpp).
if(TARGET mxcore AND TARGET mxnet_ed2k AND TARGET mxnet_gnutella AND TARGET mxnet_http AND TARGET mxnet_web AND TARGET mxnet_overlay)
    add_executable(tst_netpolicy tst_netpolicy.cpp net_test_servers.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h)
    target_include_directories(tst_netpolicy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tst_netpolicy PRIVATE mxcore mxnet_ed2k mxnet_gnutella mxnet_http mxnet_web mxnet_overlay mxapi
                          ed2k_test_support Qt6::Test Qt6::Network)
    add_test(NAME netpolicy_unit COMMAND tst_netpolicy)
    set_tests_properties(netpolicy_unit PROPERTIES TIMEOUT 300 ENVIRONMENT "MX_OVERLAY_DISCOVERY=0")
endif()

if(TARGET mxnet_bt)
    find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)
    add_executable(tst_netpolicy_bt tst_netpolicy_bt.cpp net_test_servers.h)
    target_include_directories(tst_netpolicy_bt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/bt)
    target_link_libraries(tst_netpolicy_bt PRIVATE mxnet_bt mxapi Qt6::Test Qt6::Network LibtorrentRasterbar::torrent-rasterbar)
    add_test(NAME netpolicy_bt COMMAND tst_netpolicy_bt)
    set_tests_properties(netpolicy_bt PROPERTIES TIMEOUT 300)
endif()

# [net-2] the adapter going away and back with a real (dummy) device, in a user network namespace
if(TARGET mxcore AND TARGET mxnet_ed2k AND TARGET mxnet_overlay AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(tst_netpolicy_netns tst_netpolicy_netns.cpp net_test_servers.h)
    target_include_directories(tst_netpolicy_netns PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tst_netpolicy_netns PRIVATE mxcore mxnet_ed2k mxnet_overlay mxapi Qt6::Test Qt6::Network)
    add_test(NAME netpolicy_netns COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/netns_adapter.sh $<TARGET_FILE:tst_netpolicy_netns>)
    set_tests_properties(netpolicy_netns PROPERTIES TIMEOUT 300 ENVIRONMENT "MX_OVERLAY_DISCOVERY=0")
endif()

# [net-2] the long tail: each network key changed and its effect observed
if(TARGET mxnet_bt AND TARGET mxnet_ed2k AND TARGET mxnet_gnutella)
    add_executable(tst_netsettings tst_netsettings.cpp net_test_servers.h)
    target_include_directories(tst_netsettings PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(tst_netsettings PRIVATE mxnet_bt mxnet_ed2k mxnet_gnutella mxapi Qt6::Test Qt6::Network
                          LibtorrentRasterbar::torrent-rasterbar)
    add_test(NAME netpolicy_settings COMMAND tst_netsettings)
    set_tests_properties(netpolicy_settings PROPERTIES TIMEOUT 300)
endif()
