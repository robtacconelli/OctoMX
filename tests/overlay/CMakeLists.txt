# SPDX-License-Identifier: GPL-3.0-or-later
# tests/overlay: Qt Test suites + console probes for mxnet_overlay. Owner: overlay task.
# All suites are deterministic and offline: several nodes in one process talk over 127.0.0.1 with
# explicit dials (no DHT, no mDNS). Test profiles live under <build>/overlay-test-tmp.
if(NOT TARGET mxnet_overlay)
    return()
endif()

set(MX_OVL_TEST_TMP ${CMAKE_BINARY_DIR}/overlay-test-tmp)
file(MAKE_DIRECTORY ${MX_OVL_TEST_TMP})
set(MX_OVL_TEST_TMP_NATIVE ${MX_OVL_TEST_TMP})
if(WIN32 AND CMAKE_HOST_UNIX)
    set(MX_OVL_TEST_TMP_NATIVE "Z:${MX_OVL_TEST_TMP}")   # cross build, run under wine
endif()

function(mx_overlay_test name)
    add_executable(${name} ${name}.cpp ovl_test_support.h)
    target_link_libraries(${name} PRIVATE mxnet_overlay mxapi Qt6::Core Qt6::Network Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net/overlay)
    target_compile_definitions(${name} PRIVATE MX_TEST_TMP="${MX_OVL_TEST_TMP_NATIVE}")
    add_test(NAME overlay_${name} COMMAND ${name})
    set_tests_properties(overlay_${name} PROPERTIES TIMEOUT 300 LABELS overlay
                         ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

mx_overlay_test(tst_rats_node)
mx_overlay_test(tst_overlay_chat)
mx_overlay_test(tst_overlay_users)
mx_overlay_test(tst_overlay_search)
mx_overlay_test(tst_overlay_transfer)
mx_overlay_test(tst_overlay_misc)
mx_overlay_test(tst_overlay_probe)   # [diag-1] "probe my ports", two nodes
mx_overlay_test(tst_overlay_net)     # [ovl-2] multi-node: receipts, presence, connect-back, relay, Hotlist rules, 4.2 node
mx_overlay_test(tst_overlay_upload)  # [up-1] partial files (have maps), verified hash sets, close reasons, upload row texts

# console tools (not run by ctest)
add_executable(rats_probe rats_probe.cpp)
target_link_libraries(rats_probe PRIVATE mxnet_overlay mxapi Qt6::Core)
add_executable(overlay_probe overlay_probe.cpp)
target_link_libraries(overlay_probe PRIVATE mxnet_overlay mxapi Qt6::Core Qt6::Network)
target_include_directories(overlay_probe PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src/net/overlay)

# [set-1] settings between real nodes: what is left out of sharing stays out of search, browse and
# hash requests (the core's Library), the connection type others see, the upload queue, the log
mx_overlay_test(tst_set1_overlay)
if(TARGET tst_set1_overlay AND TARGET mxcore)
    target_link_libraries(tst_set1_overlay PRIVATE mxcore)
endif()

# [net-3] the OctoMX network and Rats settings observed between real nodes (127.0.0.1); the ignore
# list between two nodes (no browse, whois details or upload for an ignored user)
mx_overlay_test(tst_netsettings_ovl)
target_link_libraries(tst_netsettings_ovl PRIVATE mxcore)
set_tests_properties(overlay_tst_netsettings_ovl PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;MX_OVERLAY_DISCOVERY=0")
mx_overlay_test(tst_overlay_ignore)
target_link_libraries(tst_overlay_ignore PRIVATE mxcore)
