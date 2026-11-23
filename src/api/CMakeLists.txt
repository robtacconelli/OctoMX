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
    # [ren-1] copies installed before 5.0: legacy profile folder, database, download folder, settings
    mx/legacy_names.h
    legacy_names.cpp
    # [ui-j] the trash / Recycle Bin (test seam under MX_OS_ROOT)
    mx/trash.h
    trash.cpp
)
target_include_directories(mxapi PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxapi PUBLIC Qt6::Core Qt6::Network)
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(mxapi PRIVATE OpenSSL::Crypto ZLIB::ZLIB)
if(WIN32)
    target_link_libraries(mxapi PRIVATE ws2_32 crypt32 bcrypt advapi32 user32)   # static libcrypto needs them
endif()
# [upd-1] tests may trust a throwaway update key (mx::upd::addTestTrustedKey); never in release
# builds (tools/release.sh: MX_TEST_HOOKS=OFF, and the verify scripts check the binary)
if(MX_TEST_HOOKS)
    target_compile_definitions(mxapi PUBLIC MX_UPDATE_TEST_HOOKS=1)
endif()
