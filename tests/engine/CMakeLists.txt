# SPDX-License-Identifier: GPL-3.0-or-later
# Download-engine tests (Qt Test) + the mxcli headless tool. Owner: engine task.
# Test data lives under <build>/tests/engine/.../engine-test-data (never /tmp or $HOME).
set(MX_ENGINE_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${MX_ENGINE_TEST_TMP})
# [engine-4] a real core in a test must never write into the real $HOME (default incoming dir =
# <Downloads>/OctoMX): every test runs with HOME / XDG dirs inside the build tree
set(MX_TEST_HOME ${CMAKE_BINARY_DIR}/test-home)
file(MAKE_DIRECTORY ${MX_TEST_HOME})

function(mx_engine_test name)
    add_executable(${name} ${name}.cpp engine_fakes.h engine_env.h ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h ${ARGN})
    target_link_libraries(${name} PRIVATE mxcore Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/core)
    string(REPLACE "tst_" "engine_" test_name ${name})
    add_test(NAME ${test_name} COMMAND ${name})
    set_tests_properties(${test_name} PROPERTIES
        ENVIRONMENT "MX_PORT_MAPPING=0;TMPDIR=${MX_ENGINE_TEST_TMP};QT_QPA_PLATFORM=offscreen;HOME=${MX_TEST_HOME};XDG_DOWNLOAD_DIR=${MX_TEST_HOME}/Downloads;XDG_CONFIG_HOME=${MX_TEST_HOME}/.config;XDG_DATA_HOME=${MX_TEST_HOME}/.local/share"
        TIMEOUT 600)
endfunction()

mx_engine_test(tst_engine)
mx_engine_test(tst_engine_torrent)
mx_engine_test(tst_engine_sources)   # [engine-4] source rows, discovery, per-download AFS/AEQ
mx_engine_test(tst_engine_order)     # [dl-1] sequential / first and last pieces first, preview
mx_engine_test(tst_engine_merge)     # [merge-1] copies matched by name and size: judge, confirm, drop, waste cap
mx_engine_test(tst_engine_map)       # [ui-i] the map of the file for the Transfers bar
mx_engine_test(tst_engine_join)      # [eng-1] torrents that hold the same file: ranking, no file list, restart, row
if(TARGET mxnet_bt)
    mx_engine_test(tst_engine_bt)
    target_link_libraries(tst_engine_bt PRIVATE mxnet_bt)
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
