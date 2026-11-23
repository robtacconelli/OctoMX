# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_web: search-only network plugin: public torrent index sites / APIs (data-driven providers).
# Owner: web task. Public header: web_plugin.h (mx::createWebSearchPlugin). Everything else is private.
add_library(mxnet_web STATIC
    web_plugin.h
    web_plugin.cpp
    web_search_plugin.h
    search_backend.h
    transforms.h
    transforms.cpp
    provider_def.h
    provider_def.cpp
    response_parser.h
    response_parser.cpp
    http_index_provider.h
    http_index_provider.cpp
    hit_builder.h
    hit_builder.cpp
    web.qrc
)
target_include_directories(mxnet_web PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxnet_web PUBLIC mxapi Qt6::Core Qt6::Network PRIVATE Qt6::Concurrent)

# Which built-in index sites are enabled by default: a profile name from the "defaults" object at
# the top of providers/providers.json. "standard" (default) = all of them (unchanged behaviour);
# "conservative" = only Internet Archive and linuxtracker, the others present but switched off.
set(MX_WEB_DEFAULT_PROFILE "standard" CACHE STRING "Default-enabled web index set (providers.json \"defaults\"): standard | conservative")
set_property(CACHE MX_WEB_DEFAULT_PROFILE PROPERTY STRINGS standard conservative)
target_compile_definitions(mxnet_web PRIVATE MX_WEB_DEFAULT_PROFILE="${MX_WEB_DEFAULT_PROFILE}")
