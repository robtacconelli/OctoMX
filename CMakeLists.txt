# SPDX-License-Identifier: GPL-3.0-or-later
# OctoMX: top-level build. Owner: coordinator.
# Modules are picked up automatically when their directory contains a CMakeLists.txt, so workers
# never need to edit this file: just create src/<module>/CMakeLists.txt and tests/<module>/CMakeLists.txt.
cmake_minimum_required(VERSION 3.21)
project(OctoMX VERSION 5.10.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent Test)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
# [post-1] the one version number: MX_VERSION_FULL "5.10.0" and MX_VERSION_STRING "5.10" (title bar,
# About, user agents, update check) come from project() above
add_compile_definitions(MX_VERSION_FULL="${PROJECT_VERSION}"
                        MX_VERSION_STRING="${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}")
if(WIN32)
    add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
endif()

option(MX_BUILD_TESTS "Build unit tests" ON)
# [release] deliberate fault-injection hooks (overlay/testCorruptUploads, used by
# tests/engine/corrupt_peer.sh). ON for dev builds; tools/release.sh builds with OFF so release
# binaries do not contain them. Modules add the MX_TEST_HOOKS define themselves.
option(MX_TEST_HOOKS "Compile fault-injection test hooks (never in release builds)" ON)
if(MX_BUILD_TESTS)
    enable_testing()
endif()

# Order matters: a module may only link to modules listed before it.
#   api       mxapi           contract headers (mx/types.h, mx/services.h, mx/plugin.h)
#   hash      mxhash          MD4/eD2k/AICH/SHA-1/Tiger-tree hashing (Qt Core only; usable by plugins)
#   core      mxcore          settings, database, hashing, part files, download engine, services
#   mock      mxmock          canned-data Core for --demo and UI work
#   net/*     mxnet_<name>    protocol plugins (bt, ed2k, gnutella, http, overlay, web)
#   ui        mxui            the classic WinMX-style look (Qt Widgets); depends on mxapi only
#   app       octomx          the executable
set(MX_MODULES api hash core mock net/bt net/ed2k net/gnutella net/http net/overlay net/web ui app)
# [ui-a] MX_UI_ONLY=ON: build only api + mock + ui + app (demo core), skipping core and the
# network plugins, so the UI can be built while those modules are in flux.
option(MX_UI_ONLY "Build only the UI with the demo core" OFF)
# Workers build concurrently and a module can be broken mid-edit. Exclude someone else's module
# (and its tests) from YOUR build tree with e.g.  -DMX_SKIP_MODULES="net/overlay;net/ed2k"
set(MX_SKIP_MODULES "" CACHE STRING "Semicolon list of src/ modules to leave out of this build tree")
foreach(mod IN LISTS MX_MODULES)
    if(MX_UI_ONLY AND NOT mod MATCHES "^(api|mock|ui|app)$")
        continue()
    endif()
    if(mod IN_LIST MX_SKIP_MODULES)
        message(STATUS "OctoMX: skipping module ${mod} (MX_SKIP_MODULES)")
    elseif(EXISTS ${CMAKE_SOURCE_DIR}/src/${mod}/CMakeLists.txt)
        add_subdirectory(src/${mod})
    endif()
endforeach()

if(MX_BUILD_TESTS)
    file(GLOB test_dirs RELATIVE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/*)
    foreach(t IN LISTS test_dirs)
        # tests/toolchain-hello and tests/research-* are standalone projects, not part of this build
        if(EXISTS ${CMAKE_SOURCE_DIR}/tests/${t}/CMakeLists.txt
           AND NOT t MATCHES "^(toolchain-hello|research-)"
           AND (NOT MX_UI_ONLY OR t STREQUAL "ui")
           AND NOT t IN_LIST MX_SKIP_MODULES
           AND NOT "net/${t}" IN_LIST MX_SKIP_MODULES)
            add_subdirectory(tests/${t})
        endif()
    endforeach()
endif()
