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

# [net-3] the network settings that had only a reader proof, each changed and observed:
# BitTorrent (+ bandwidth/limitLan), eD2k / Kad / Gnutella, HTTP (a local HTTPS server with a
# certificate made by the test) and the web indexes
if(TARGET mxnet_bt AND TARGET mxcore)
    add_executable(tst_netsettings_bt tst_netsettings_bt.cpp net_test_servers.h)
    target_include_directories(tst_netsettings_bt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/bt ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(tst_netsettings_bt PRIVATE mxnet_bt mxcore mxapi Qt6::Test Qt6::Network LibtorrentRasterbar::torrent-rasterbar)
    add_test(NAME netpolicy_settings_bt COMMAND tst_netsettings_bt)
    set_tests_properties(netpolicy_settings_bt PROPERTIES TIMEOUT 600)
endif()
if(TARGET mxnet_ed2k AND TARGET mxnet_gnutella AND TARGET mxcore)
    add_executable(tst_netsettings_p2p tst_netsettings_p2p.cpp net_test_servers.h)
    target_include_directories(tst_netsettings_p2p PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
                               ${CMAKE_SOURCE_DIR}/src/net/ed2k ${CMAKE_SOURCE_DIR}/src/net/ed2k/kad ${CMAKE_SOURCE_DIR}/src/net/gnutella)
    target_link_libraries(tst_netsettings_p2p PRIVATE mxnet_ed2k mxnet_gnutella mxcore mxapi ed2k_test_support gnutella_test_support
                          Qt6::Test Qt6::Network)
    add_test(NAME netpolicy_settings_p2p COMMAND tst_netsettings_p2p)
    set_tests_properties(netpolicy_settings_p2p PROPERTIES TIMEOUT 900)
endif()
if(TARGET mxnet_http AND TARGET mxnet_web AND TARGET mxcore)
    find_package(OpenSSL REQUIRED)
    add_executable(tst_netsettings_web tst_netsettings_web.cpp net_test_servers.h
                   ${CMAKE_SOURCE_DIR}/tests/http/test_server.h ${CMAKE_SOURCE_DIR}/tests/http/test_server.cpp)
    target_include_directories(tst_netsettings_web PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src
                               ${CMAKE_SOURCE_DIR}/tests/http ${CMAKE_SOURCE_DIR}/src/net/http ${CMAKE_SOURCE_DIR}/src/net/web)
    target_link_libraries(tst_netsettings_web PRIVATE mxnet_http mxnet_web mxcore mxapi OpenSSL::SSL OpenSSL::Crypto Qt6::Test Qt6::Network)
    add_test(NAME netpolicy_settings_web COMMAND tst_netsettings_web)
    set_tests_properties(netpolicy_settings_web PROPERTIES TIMEOUT 600)
endif()

# [net-3] a file that stops being shared stops being offered on every network at once (a second
# instance asks by search, browse and hash; a running upload stops with the reason), and
# transfers/maxUploads as one total over the OctoMX network, eD2k and Gnutella
set(MX_NET3_TMP ${CMAKE_BINARY_DIR}/overlay-test-tmp)
if(WIN32 AND CMAKE_HOST_UNIX)
    set(MX_NET3_TMP "Z:${MX_NET3_TMP}")   # cross build, run under wine: a Windows path
endif()
if(TARGET mxcore AND TARGET mxnet_ed2k AND TARGET mxnet_gnutella AND TARGET mxnet_overlay AND TARGET mxnet_bt)
    add_executable(tst_unshare tst_unshare.cpp net_test_servers.h)
    target_include_directories(tst_unshare PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/bt
                               ${CMAKE_SOURCE_DIR}/tests/overlay ${CMAKE_SOURCE_DIR}/src/net/ed2k ${CMAKE_SOURCE_DIR}/src/net/ed2k/kad
                               ${CMAKE_SOURCE_DIR}/src/net/gnutella ${CMAKE_SOURCE_DIR}/src/net/overlay)
    target_link_libraries(tst_unshare PRIVATE mxcore mxnet_ed2k mxnet_gnutella mxnet_overlay mxnet_bt mxapi ed2k_test_support
                          gnutella_test_support Qt6::Test Qt6::Network LibtorrentRasterbar::torrent-rasterbar)
    target_compile_definitions(tst_unshare PRIVATE MX_TEST_TMP="${MX_NET3_TMP}")
    add_test(NAME netpolicy_unshare COMMAND tst_unshare)
    set_tests_properties(netpolicy_unshare PROPERTIES TIMEOUT 900 ENVIRONMENT "MX_OVERLAY_DISCOVERY=0")

    add_executable(tst_upload_pool tst_upload_pool.cpp)
    target_include_directories(tst_upload_pool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/src ${CMAKE_SOURCE_DIR}/tests/overlay
                               ${CMAKE_SOURCE_DIR}/src/net/ed2k ${CMAKE_SOURCE_DIR}/src/net/ed2k/kad ${CMAKE_SOURCE_DIR}/src/net/gnutella
                               ${CMAKE_SOURCE_DIR}/src/net/overlay)
    target_link_libraries(tst_upload_pool PRIVATE mxcore mxnet_ed2k mxnet_gnutella mxnet_overlay mxapi ed2k_test_support
                          gnutella_test_support Qt6::Test Qt6::Network)
    target_compile_definitions(tst_upload_pool PRIVATE MX_TEST_TMP="${MX_NET3_TMP}")
    add_test(NAME netpolicy_upload_pool COMMAND tst_upload_pool)
    set_tests_properties(netpolicy_upload_pool PROPERTIES TIMEOUT 600 ENVIRONMENT "MX_OVERLAY_DISCOVERY=0")
endif()
