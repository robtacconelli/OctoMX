# SPDX-License-Identifier: GPL-3.0-or-later
# tests/bt: offline, deterministic tests of mxnet_bt (two sessions on 127.0.0.1, DHT/LSD off).
# Owner: bt task. Also builds bt_probe (live network proof; not a ctest).
find_package(Qt6 REQUIRED COMPONENTS Core Network Test)
find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)

foreach(t transfer inject restart)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tst_bt_${t}.cpp)
        add_executable(tst_bt_${t} tst_bt_${t}.cpp bt_test_util.h)
        target_link_libraries(tst_bt_${t} PRIVATE mxnet_bt mxapi Qt6::Core Qt6::Network Qt6::Test
            LibtorrentRasterbar::torrent-rasterbar)
        target_include_directories(tst_bt_${t} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        add_test(NAME bt_${t} COMMAND tst_bt_${t})
        set_tests_properties(bt_${t} PROPERTIES TIMEOUT 300
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endforeach()

# [bw-1] libtorrent inside the core's shared budget (CoreRateLimiter: needs mxcore)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tst_bt_limits.cpp AND TARGET mxcore)
    add_executable(tst_bt_limits tst_bt_limits.cpp bt_test_util.h)
    target_link_libraries(tst_bt_limits PRIVATE mxnet_bt mxcore mxapi Qt6::Core Qt6::Network Qt6::Test
        LibtorrentRasterbar::torrent-rasterbar)
    target_include_directories(tst_bt_limits PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME bt_limits COMMAND tst_bt_limits)
    set_tests_properties(bt_limits PROPERTIES TIMEOUT 300 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bt_probe.cpp)
    add_executable(bt_probe bt_probe.cpp)
    target_link_libraries(bt_probe PRIVATE mxnet_bt mxapi Qt6::Core Qt6::Network)
endif()
