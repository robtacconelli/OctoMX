# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(research_librats_poc LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Which librats checkout to build against. Default: the exact commit rats-search
# v2.3.6 pins as its submodule (364169f), i.e. what the live network runs.
set(LIBRATS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/src/librats-pinned"
    CACHE PATH "librats source tree")

# librats knobs: library only, no client/tests/examples, no BitTorrent/storage.
set(RATS_BUILD_CLIENT   OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(RATS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(RATS_BINDINGS       OFF CACHE BOOL "" FORCE)
set(RATS_SEARCH_FEATURES OFF CACHE BOOL "" FORCE)
set(RATS_STORAGE        OFF CACHE BOOL "" FORCE)
add_subdirectory(${LIBRATS_DIR} librats EXCLUDE_FROM_ALL)

add_executable(rats_poc main.cpp)
target_link_libraries(rats_poc PRIVATE rats)
if(WIN32)
    # static runtime so the .exe runs under wine without extra DLLs
    target_link_options(rats_poc PRIVATE -static)
endif()
