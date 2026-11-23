# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_http: plain HTTP/HTTPS byte-range sources (mirrors, Internet Archive, pasted links) + URL probe.
# Owner: http task. Public header: http_plugin.h (mx::createHttpPlugin, mx::HttpProbe). Everything else is private.
add_library(mxnet_http STATIC
    http_plugin.h
    http_plugin_impl.h
    http_plugin.cpp
    http_util.h
    http_util.cpp
    http_jobs.h
    http_jobs.cpp
    http_source.h
    http_source.cpp
)
target_include_directories(mxnet_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxnet_http PUBLIC mxapi Qt6::Core Qt6::Network)
