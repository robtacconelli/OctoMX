# SPDX-License-Identifier: GPL-3.0-or-later
# mxcore: the real mx::Core (settings, database, library + hasher, search/network/chat/user/bandwidth
# services, upload tracker, download-engine seam). Owner: core task.
# Sources are globbed so that the download-engine worker can replace engine/engine_stub.cpp with its
# own files under engine/ without editing this file (re-run cmake after adding/removing files).
file(GLOB MXCORE_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/engine/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/diag/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/diag/*.h)   # [diag-1] Connection Test
add_library(mxcore STATIC ${MXCORE_SOURCES})
# Includes are written as "core/xxx.h" / "core/engine/engine.h".
target_include_directories(mxcore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mxcore PUBLIC mxapi mxhash Qt6::Core Qt6::Network Qt6::Sql Qt6::Concurrent)
# [diag-1] default gateway lookup (GetBestRoute) and the firewall check (COM) on Windows
if(WIN32)
    target_link_libraries(mxcore PUBLIC iphlpapi ws2_32 ole32 oleaut32)
endif()
