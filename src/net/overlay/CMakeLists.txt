# SPDX-License-Identifier: GPL-3.0-or-later
# mxnet_overlay: librats-based networks: the rats-search P2P search network (Net::Rats) and our own
# OctoMX overlay (Net::Overlay: chat, PMs, browse, whois, overlay search, direct transfer, hash links).
# Owner: overlay task. Public header: overlay_plugin.h. Everything else is private to this module.
#
# librats is vendored at third_party/src/librats and PINNED to the commit below (it interoperates
# with the live rats-search/4 network and fixes same-host mDNS). Re-run tests/overlay/rats_probe
# against the live network before every bump.
get_filename_component(_mx_root ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
set(MX_LIBRATS_DIR ${_mx_root}/third_party/src/librats CACHE PATH "librats source tree")
set(MX_LIBRATS_PIN 9eefd0a9d9b7b0ccc3c692d6e8f80224723aed49)   # 2026-11-18, HEAD on 2026-11-20

find_package(Git QUIET)
if(GIT_FOUND AND EXISTS ${MX_LIBRATS_DIR}/.git)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse HEAD WORKING_DIRECTORY ${MX_LIBRATS_DIR}
                    OUTPUT_VARIABLE _rats_head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(_rats_head AND NOT _rats_head STREQUAL MX_LIBRATS_PIN)
        message(WARNING "librats checkout is at ${_rats_head}, pinned commit is ${MX_LIBRATS_PIN}")
    endif()
endif()

# librats knobs: library only (no client/tests/examples/bindings, no BitTorrent/storage).
set(RATS_BUILD_CLIENT    OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS     OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_EXAMPLES  OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS        OFF CACHE BOOL "" FORCE)
set(RATS_SEARCH_FEATURES OFF CACHE BOOL "" FORCE)
set(RATS_STORAGE         OFF CACHE BOOL "" FORCE)
set(RATS_SHARED_LIBRARY  OFF CACHE BOOL "" FORCE)
set(RATS_STATIC_LIBRARY  ON  CACHE BOOL "" FORCE)
set(RATS_VERSION_OVERRIDE 3.0.6)   # shallow clone has no tags; do not depend on git describe
add_subdirectory(${MX_LIBRATS_DIR} ${CMAKE_BINARY_DIR}/third_party/librats EXCLUDE_FROM_ALL)
set_target_properties(rats PROPERTIES AUTOMOC OFF AUTORCC OFF AUTOUIC OFF)
# librats is third-party code: do not drown our build log in its warnings.
target_compile_options(rats PRIVATE -w)

find_package(OpenSSL REQUIRED)   # Ed25519 / X25519 / ChaCha20-Poly1305 for overlay identity + PMs
find_package(ZLIB REQUIRED)

set(MX_OVERLAY_SOURCES
    overlay_plugin_impl.h
    overlay_plugin.cpp
    overlay_chat.cpp
    overlay_users.cpp
    overlay_search.cpp
    overlay_transfer.h
    overlay_transfer.cpp
    overlay_probe.cpp     # [diag-1] Connection Test: diagnostics + "probe my ports"
)
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/overlay_plugin.cpp)
    set(MX_OVERLAY_SOURCES)   # bootstrap: rats part only
endif()
add_library(mxnet_overlay STATIC
    overlay_plugin.h
    rats_node.h
    rats_node.cpp
    ovl_util.h
    ovl_util.cpp
    ovl_crypto.h
    ovl_crypto.cpp
    rats_search_plugin.h
    rats_search_plugin.cpp
    ${MX_OVERLAY_SOURCES}
)
target_include_directories(mxnet_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
if(MX_TEST_HOOKS)   # [release] overlay/testCorruptUploads exists only in dev builds
    target_compile_definitions(mxnet_overlay PRIVATE MX_TEST_HOOKS=1)
endif()
target_link_libraries(mxnet_overlay
    PUBLIC  mxapi Qt6::Core Qt6::Network
    PRIVATE rats OpenSSL::Crypto ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(mxnet_overlay PRIVATE ws2_32 crypt32 bcrypt advapi32 user32)
endif()
