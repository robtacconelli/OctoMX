# SPDX-License-Identifier: GPL-3.0-or-later
# Throwaway proof that the toolchain works (Linux native + Windows cross).
cmake_minimum_required(VERSION 3.21)
project(toolchain_hello LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Network Sql Concurrent Test Svg Multimedia)
find_package(LibtorrentRasterbar 2.0 CONFIG REQUIRED)
find_package(OpenSSL 3 REQUIRED)
find_package(ZLIB REQUIRED)
find_package(Boost 1.80 CONFIG REQUIRED)
message(STATUS "hello: libtorrent ${LibtorrentRasterbar_VERSION}, OpenSSL ${OPENSSL_VERSION} (${OPENSSL_CRYPTO_LIBRARY}), zlib ${ZLIB_LIBRARIES}, Boost ${Boost_VERSION}")

qt_standard_project_setup()

qt_add_executable(toolchain_hello main.cpp)
target_link_libraries(toolchain_hello PRIVATE
    Qt6::Widgets Qt6::Network Qt6::Sql Qt6::Concurrent Qt6::Svg Qt6::Multimedia
    LibtorrentRasterbar::torrent-rasterbar OpenSSL::Crypto ZLIB::ZLIB)

# Qt Test smoke test
enable_testing()
qt_add_executable(toolchain_hello_test test.cpp)
target_link_libraries(toolchain_hello_test PRIVATE Qt6::Test Qt6::Core)
add_test(NAME toolchain_hello_test COMMAND toolchain_hello_test)
