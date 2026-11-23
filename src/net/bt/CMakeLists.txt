# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_bt: BitTorrent network plugin on libtorrent-rasterbar 2.0. Owner: bt task.
# Public header: bt_plugin.h (mx::createBitTorrentPlugin). Everything else is private.
find_package(Qt6 REQUIRED COMPONENTS Core Network)
find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)

add_library(mxnet_bt STATIC
    bt_plugin.h
    bt_plugin_impl.h
    bt_plugin.cpp
    bt_common.h
    bt_common.cpp
    bt_worker.h
    bt_worker.cpp
    bt_pacing.h
    bt_pacing.cpp
)
target_include_directories(mxnet_bt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxnet_bt
    PUBLIC mxapi Qt6::Core Qt6::Network
    PRIVATE LibtorrentRasterbar::torrent-rasterbar)
