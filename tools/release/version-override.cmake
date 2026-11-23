# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-1] Build a package with another version number than project() says, without touching the
# version line (end-to-end update tests: package B = a higher version). Included right after the
# top-level project() through
#     -DCMAKE_PROJECT_OctoMX_INCLUDE=<this file> -DMX_VERSION_OVERRIDE=5.2.0
# (tools/release.sh --version 5.2.0 passes both). Everything that reads PROJECT_VERSION afterwards
# (MX_VERSION_FULL / MX_VERSION_STRING, the Windows version resource) sees the override.
if(MX_VERSION_OVERRIDE)
    if(NOT MX_VERSION_OVERRIDE MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
        message(FATAL_ERROR "MX_VERSION_OVERRIDE must be x.y.z, not '${MX_VERSION_OVERRIDE}'")
    endif()
    message(STATUS "OctoMX: version ${PROJECT_VERSION} overridden to ${MX_VERSION_OVERRIDE} (MX_VERSION_OVERRIDE)")
    foreach(prefix PROJECT OctoMX CMAKE_PROJECT)
        set(${prefix}_VERSION ${MX_VERSION_OVERRIDE})
        set(${prefix}_VERSION_MAJOR ${CMAKE_MATCH_1})
        set(${prefix}_VERSION_MINOR ${CMAKE_MATCH_2})
        set(${prefix}_VERSION_PATCH ${CMAKE_MATCH_3})
    endforeach()
endif()
