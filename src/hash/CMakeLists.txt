# SPDX-License-Identifier: GPL-3.0-or-later
# mxhash: MD4/eD2k/AICH/SHA/Tiger-tree/BT piece hashing + magnet/ed2k links. Qt Core only.
add_library(mxhash STATIC
    mxhash/hasher.h
    mxhash/ed2k.h
    mxhash/aich.h
    mxhash/tiger_tree.h
    mxhash/bt_pieces.h
    mxhash/multi_hasher.h
    mxhash/encoding.h
    mxhash/links.h
    md4.cpp
    tiger.cpp
    ed2k.cpp
    aich.cpp
    tiger_tree.cpp
    bt_pieces.cpp
    multi_hasher.cpp
    encoding.cpp
    links.cpp
)
target_include_directories(mxhash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mxhash PUBLIC mxapi Qt6::Core)
