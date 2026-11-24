# SPDX-License-Identifier: GPL-3.0-or-later
# [str-1] the local streaming API: a real core (search, engine, API) with local fake networks or a
# local libtorrent swarm on 127.0.0.1; nothing live. Test data under <build>/tests/stream/....
set(MX_STREAM_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${MX_STREAM_TEST_TMP})
set(MX_STREAM_TEST_HOME ${CMAKE_BINARY_DIR}/test-home)
file(MAKE_DIRECTORY ${MX_STREAM_TEST_HOME})
set(_stream_env "MX_PORT_MAPPING=0;MX_OVERLAY_DISCOVERY=0;TMPDIR=${MX_STREAM_TEST_TMP};QT_QPA_PLATFORM=offscreen;HOME=${MX_STREAM_TEST_HOME};XDG_DOWNLOAD_DIR=${MX_STREAM_TEST_HOME}/Downloads;XDG_CONFIG_HOME=${MX_STREAM_TEST_HOME}/.config;XDG_DATA_HOME=${MX_STREAM_TEST_HOME}/.local/share;MX_OS_ROOT=${CMAKE_BINARY_DIR}/os-root")

function(mx_stream_test name)
    add_executable(${name} ${name}.cpp stream_support.h ${CMAKE_SOURCE_DIR}/tests/engine/engine_fakes.h
                   ${CMAKE_SOURCE_DIR}/tests/core/fake_plugin.h ${ARGN})
    target_link_libraries(${name} PRIVATE mxcore Qt6::Test Qt6::Network)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/tests/engine ${CMAKE_SOURCE_DIR}/tests/core)
    string(REPLACE "tst_" "stream_" test_name ${name})
    add_test(NAME ${test_name} COMMAND ${name})
    set_tests_properties(${test_name} PROPERTIES ENVIRONMENT "${_stream_env}" TIMEOUT 900)
endfunction()

mx_stream_test(tst_stream_api)      # the API: calls, errors, token, Host, methods, limits, on / off / port / LAN
mx_stream_test(tst_stream_play)     # streams over every store and network mix, read like a player
if(TARGET mxnet_bt)
    mx_stream_test(tst_stream_bt ${CMAKE_SOURCE_DIR}/tests/engine/bt_node.h)   # a local libtorrent swarm
    find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)
    target_link_libraries(tst_stream_bt PRIVATE mxnet_bt LibtorrentRasterbar::torrent-rasterbar)
    target_include_directories(tst_stream_bt PRIVATE ${CMAKE_SOURCE_DIR}/src/net/bt)
endif()
