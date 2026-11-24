# SPDX-License-Identifier: GPL-3.0-or-later
# mxapi: the shared contract (types + abstract service/plugin interfaces). Owner: coordinator.
add_library(mxapi STATIC
    types.cpp
    mx/types.h
    mx/services.h
    mx/plugin.h
    # settings catalogue + in-memory Settings (added by the core task)
    mx/settings_keys.h
    settings_schema.cpp
    mx/simple_settings.h
    simple_settings.cpp
    # default implementations of added service methods (glue-2)
    services.cpp
    # connection diagnostics report model, text report, address masking (diag-1)
    mx/diagnostics.h
    diagnostics.cpp
    # [post-1] Check for Updates (latest GitHub release, version compare)
    mx/update_check.h
    update_check.cpp
    # [upd-1] signed self-update: Ed25519 + SHA-256 checks, manifest, zip reader, journalled swap,
    # download (OpenSSL libcrypto and zlib, both already linked into octomx by the network plugins)
    mx/update_install.h
    update_install.cpp
    update_keys.cpp
    mx/update_download.h
    update_download.cpp
    # [dl-1] download order bands (sequential / first and last) and preview readiness
    mx/download_order.h
    download_order.cpp
    # [safe-1] sync to the disk, copies never seen half done, the crash / power cut test seams
    mx/durable_file.h
    durable_file.cpp
    mx/quit_signals.h
    quit_signals.cpp
    # [ren-1] copies installed before 5.0: legacy profile folder, database, download folder, settings
    mx/legacy_names.h
    legacy_names.cpp
    # [ui-j] the trash / Recycle Bin (test seam under MX_OS_ROOT)
    mx/trash.h
    trash.cpp
    # [mac-1] the program folder as the user sees it (the app bundle on macOS), portable.txt
    mx/app_paths.h
    app_paths.cpp
    # [net-2] the connection policy every network follows (proxy, chosen adapter, IPv6) and the
    # relay that binds web requests to the chosen adapter
    mx/net_policy.h
    net_policy.cpp
    bind_relay.h
    bind_relay.cpp
    # [net-3] one pool of upload slots for the OctoMX network, eD2k and Gnutella (transfers/maxUploads)
    mx/upload_slots.h
    upload_slots.cpp
)
target_include_directories(mxapi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxapi PUBLIC Qt6::Core Qt6::Network)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(mxapi PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
if(APPLE)
    # [mac-1] the package name of this build (mx::upd::platformTag): one universal app, or one architecture
    list(LENGTH CMAKE_OSX_ARCHITECTURES _mx_narch)
    if(_mx_narch GREATER 1)
        set(MX_MAC_PLATFORM_TAG macos-universal)
    elseif(CMAKE_OSX_ARCHITECTURES)
        set(MX_MAC_PLATFORM_TAG macos-${CMAKE_OSX_ARCHITECTURES})
    else()
        set(MX_MAC_PLATFORM_TAG macos-${CMAKE_SYSTEM_PROCESSOR})
    endif()
    target_compile_definitions(mxapi PRIVATE MX_MAC_PLATFORM_TAG="${MX_MAC_PLATFORM_TAG}")
endif()
if(WIN32)
    target_link_libraries(mxapi PRIVATE ws2_32 crypt32 bcrypt advapi32 user32)   # static libcrypto needs them
endif()
# [upd-1] tests may trust a throwaway update key (mx::upd::addTestTrustedKey); never in release
# builds (tools/release.sh: MX_TEST_HOOKS=OFF, and the verify scripts check the binary)
if(MX_TEST_HOOKS)
    target_compile_definitions(mxapi PUBLIC MX_UPDATE_TEST_HOOKS=1)
endif()
