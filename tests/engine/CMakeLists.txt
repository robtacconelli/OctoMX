# SPDX-License-Identifier: GPL-3.0-or-later
# Download-engine tests (Qt Test) + the mxcli headless tool. Owner: engine task.
# Test data lives under <build>/tests/engine/.../engine-test-data (never /tmp or $HOME).
set(MX_ENGINE_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${MX_ENGINE_TEST_TMP})
# [engine-4] a real core in a test must never write into the real $HOME (default incoming dir =
# <Downloads>/OctoMX): every test runs with HOME / XDG dirs inside the build tree; [ui-j] MX_OS_ROOT
# there too, so a deleted download's data goes to the trash seam of mx/trash.h, never the real trash
set(MX_TEST_HOME ${CMAKE_BINARY_DIR}/test-home)
file(MAKE_DIRECTORY ${MX_TEST_HOME})

function(mx_engine_test name)
    add_executable(${name} ${name}.cpp engine_fakes.h engine_env.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h ${ARGN})
    target_link_libraries(${name} PRIVATE mxcore Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core)
    string(REPLACE "tst_" "engine_" test_name ${name})
    add_test(NAME ${test_name} COMMAND ${name})
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "MX_PORT_MAPPING=0;TMPDIR=${MX_ENGINE_TEST_TMP};QT_QPA_PLATFORM=offscreen;HOME=${MX_TEST_HOME};XDG_DOWNLOAD_DIR=${MX_TEST_HOME}/Downloads;XDG_CONFIG_HOME=${MX_TEST_HOME}/.config;XDG_DATA_HOME=${MX_TEST_HOME}/.local/share;MX_OS_ROOT=${CMAKE_BINARY_DIR}/os-root"
        TIMEOUT 600)
endfunction()

mx_engine_test(tst_engine)
mx_engine_test(tst_engine_torrent)
mx_engine_test(tst_engine_sources)   # [engine-4] source rows, discovery, per-download AFS/AEQ
mx_engine_test(tst_engine_order)     # [dl-1] sequential / first and last pieces first, preview
mx_engine_test(tst_engine_merge)     # [merge-1] copies matched by name and size: judge, confirm, drop, waste cap
mx_engine_test(tst_engine_map)       # [ui-i] the map of the file for the Transfers bar
mx_engine_test(tst_engine_join)      # [eng-1] torrents that hold the same file: ranking, no file list, restart, row
mx_engine_test(tst_engine_remove)    # [ui-j] removing a download: keep the file (and resume it) or delete it
mx_engine_test(tst_engine_serve)     # [up-1] serving files still downloading, hash-set rules, close reasons
mx_engine_test(tst_set1_engine)      # [set-1] the File Transfers settings, each with its effect
mx_engine_test(tst_engine_verify)    # [ver-1] Verify File (damaged blocks only) and Restart Download
# [ver-1] a damaged finished file is not offered while it is repaired: the Library and a second
# OctoMX network node (two overlay nodes on 127.0.0.1, no discovery)
if(TARGET mxnet_overlay)
    mx_engine_test(tst_engine_verify_net ${CMAKE_SOURCE_DIR}/tests/overlay/ovl_test_support.h)
    target_link_libraries(tst_engine_verify_net PRIVATE mxnet_overlay Qt6::Network)
    target_include_directories(tst_engine_verify_net PRIVATE ${CMAKE_SOURCE_DIR}/tests/overlay ${CMAKE_SOURCE_DIR}/src/net/overlay)
    set(MX_VER_NET_TMP ${MX_ENGINE_TEST_TMP}/ver-net)
    file(MAKE_DIRECTORY ${MX_VER_NET_TMP})
    if(WIN32 AND CMAKE_HOST_UNIX)
        set(MX_VER_NET_TMP "Z:${MX_VER_NET_TMP}")   # cross build, run under wine
    endif()
    target_compile_definitions(tst_engine_verify_net PRIVATE MX_TEST_TMP="${MX_VER_NET_TMP}")
    set_property(TEST engine_engine_verify_net APPEND PROPERTY ENVIRONMENT "MX_OVERLAY_DISCOVERY=0")
endif()
mx_engine_test(tst_engine_safe)      # [safe-1] synced records, interrupted moves, the unclean-stop mark, the database copy
if(TARGET mxnet_bt)
    mx_engine_test(tst_engine_bt)
    target_link_libraries(tst_engine_bt PRIVATE mxnet_bt)
    # [safe-1] closing, kills and power cuts on every store: crash_child is the program that is
    # stopped; one ctest per way of stopping and store (each fits the 900 s timeout)
    add_executable(crash_child crash_child.cpp engine_fakes.h bt_node.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h)
    target_link_libraries(crash_child PRIVATE mxcore mxnet_bt Qt6::Test)
    target_include_directories(crash_child PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core)
    add_executable(tst_engine_crash tst_engine_crash.cpp engine_fakes.h engine_env.h bt_node.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h)
    target_link_libraries(tst_engine_crash PRIVATE mxcore mxnet_bt Qt6::Test)
    target_include_directories(tst_engine_crash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core)
    add_dependencies(tst_engine_crash crash_child)
    set(_crash_tests movekill throughput)
    foreach(m quit slowquit closenow term kill cut lie)
        foreach(st ed2k tth whole torrent merged)
            list(APPEND _crash_tests "${m}:${st}")
        endforeach()
    endforeach()
    foreach(t ${_crash_tests})
        string(REPLACE ":" "_" tn "engine_crash_${t}")
        add_test(NAME ${tn} COMMAND tst_engine_crash ${t})
        set_tests_properties(${tn} PROPERTIES
            ENVIRONMENT "MX_PORT_MAPPING=0;TMPDIR=${MX_ENGINE_TEST_TMP};QT_QPA_PLATFORM=offscreen;HOME=${MX_TEST_HOME};XDG_DOWNLOAD_DIR=${MX_TEST_HOME}/Downloads;XDG_CONFIG_HOME=${MX_TEST_HOME}/.config;XDG_DATA_HOME=${MX_TEST_HOME}/.local/share;MX_OS_ROOT=${CMAKE_BINARY_DIR}/os-root"
            TIMEOUT 900)
    endforeach()
endif()

# ---- mxcli: the real core + every plugin that exists in this build tree, no GUI
# [eng-1] join_probe: live check that BitTorrent joins a merged eD2k / Kad / Gnutella download
add_executable(mxcli mxcli.cpp)
add_executable(join_probe join_probe.cpp)
foreach(tool mxcli join_probe)
    target_link_libraries(${tool} PRIVATE mxcore Qt6::Network)
    foreach(p bt ed2k web http gnutella overlay)
        if(TARGET mxnet_${p})
            target_link_libraries(${tool} PRIVATE mxnet_${p})
            string(TOUPPER ${p} P)
            target_compile_definitions(${tool} PRIVATE MX_HAVE_${P}=1)
        endif()
    endforeach()
endforeach()

# [eng-1] join_shots: pictures of the BitTorrent line of the Transfers row, fake swarm (not a ctest)
if(TARGET mxui)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    add_executable(join_shots join_shots.cpp engine_fakes.h engine_env.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h)
    target_link_libraries(join_shots PRIVATE mxcore mxui Qt6::Test Qt6::Widgets)
    target_include_directories(join_shots PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core ${CMAKE_SOURCE_DIR}/src)
endif()

# [bw-2] bw_slice_probe: the shared speed limit per 50 ms, measured outside the program (not a ctest)
if(TARGET mxnet_bt AND TARGET mxnet_ed2k AND TARGET mxnet_http AND TARGET ed2k_test_support)
    add_executable(bw_slice_probe bw_slice_probe.cpp)
    target_link_libraries(bw_slice_probe PRIVATE mxcore mxnet_bt mxnet_ed2k mxnet_http ed2k_test_support Qt6::Network Qt6::Test)
    target_include_directories(bw_slice_probe PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
