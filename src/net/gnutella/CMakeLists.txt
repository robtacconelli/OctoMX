# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_gnutella: Gnutella 0.6 leaf servent on Qt Network. Owner: gnutella task.
# Public header: gnutella_plugin.h (mx::createGnutellaPlugin). Everything else is private
# (the tests include the private headers directly).
find_package(ZLIB REQUIRED)
find_package(OpenSSL REQUIRED)   # [enc-1] Gnutella TLS (own contexts, same on Windows and Linux)

add_library(mxnet_gnutella STATIC
    gnutella_plugin.h
    gnut_util.h        gnut_util.cpp
    gnut_zlib.h        gnut_zlib.cpp
    gnut_ggep.h        gnut_ggep.cpp
    gnut_huge.h        gnut_huge.cpp
    gnut_message.h     gnut_message.cpp
    gnut_qrp.h         gnut_qrp.cpp
    gnut_headers.h     gnut_headers.cpp
    gnut_http.h        gnut_http.cpp
    gnut_thex.h        gnut_thex.cpp
    gnut_hostcache.h   gnut_hostcache.cpp
    gnut_tls.h         gnut_tls.cpp
    gnut_connection.h  gnut_connection.cpp
    gnut_upload.h      gnut_upload.cpp
    gnut_source.h      gnut_source.cpp
    gnut_browse.h      gnut_browse.cpp
    gnut_plugin_impl.h
    gnut_plugin.cpp
    gnut_net.cpp
    gnut_search.cpp
    gnut_serve.cpp
)
target_include_directories(mxnet_gnutella PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxnet_gnutella PUBLIC mxapi mxhash Qt6::Core Qt6::Network PRIVATE ZLIB::ZLIB OpenSSL::SSL OpenSSL::Crypto)
