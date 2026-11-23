# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_ed2k: eDonkey2000 / eMule compatible network plugin (servers + client-to-client).
# Owner: ed2k task. Public header: ed2k_plugin.h (mx::createEd2kPlugins). Everything else is private.
find_package(ZLIB REQUIRED)

add_library(mxnet_ed2k STATIC
    ed2k_plugin.h
    ed2k_plugin_impl.h
    ed2k_plugin.cpp
    ed2k_opcodes.h
    ed2k_io.h
    ed2k_io.cpp
    ed2k_tags.h
    ed2k_tags.cpp
    ed2k_packet.h
    ed2k_packet.cpp
    ed2k_crypto.h
    ed2k_crypto.cpp
    ed2k_servermet.h
    ed2k_servermet.cpp
    ed2k_search.h
    ed2k_search.cpp
    ed2k_connection.h
    ed2k_connection.cpp
    ed2k_server.h
    ed2k_server.cpp
    ed2k_peer.h
    ed2k_peer.cpp
    ed2k_source.h
    ed2k_source.cpp
    ed2k_upload.h
    ed2k_upload.cpp
    ed2k_core.h
    ed2k_core.cpp
    ed2k_util.h
    ed2k_util.cpp
    kad/kad_id.h
    kad/kad_id.cpp
    kad/kad_wire.h
    kad/kad_wire.cpp
    kad/kad_routing.h
    kad/kad_routing.cpp
    kad/kad_search.h
    kad/kad_search.cpp
    kad/kad_index.h
    kad/kad_index.cpp
    kad/kad_node.h
    kad/kad_node.cpp
    kad/kad_plugin.h
    kad/kad_plugin.cpp
)
target_include_directories(mxnet_ed2k PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxnet_ed2k PUBLIC mxapi Qt6::Core Qt6::Network PRIVATE ZLIB::ZLIB)
