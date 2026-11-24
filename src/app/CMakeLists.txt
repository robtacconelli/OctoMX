# SPDX-License-Identifier: GPL-3.0-or-later
# octomx: the executable: command line, single instance, logging, composition (real or demo core).
# Owner: ui-a. Builds with whatever modules exist: the real core and each network plugin are used
# when their target exists (MX_HAVE_* defines); -DMX_UI_ONLY=ON gives a demo-core-only build.
find_package(Qt6 REQUIRED COMPONENTS Widgets Network)

add_executable(octomx
    main.cpp
    single_instance.h
    single_instance.cpp
    app_log.h
    app_log.cpp
)
target_link_libraries(octomx PRIVATE mxui mxmock mxapi Qt6::Widgets Qt6::Network)

if(NOT MX_UI_ONLY AND TARGET mxcore)
    target_link_libraries(octomx PRIVATE mxcore)
    target_compile_definitions(octomx PRIVATE MX_HAVE_CORE)
    foreach(p bt ed2k web gnutella http overlay)
        string(TOUPPER ${p} P)
        if(TARGET mxnet_${p} AND NOT "net/${p}" IN_LIST MX_SKIP_MODULES)
            target_link_libraries(octomx PRIVATE mxnet_${p})
            target_include_directories(octomx PRIVATE ${CMAKE_SOURCE_DIR}/src/net/${p})
            target_compile_definitions(octomx PRIVATE MX_HAVE_${P})
            message(STATUS "octomx: using plugin mxnet_${p}")
        endif()
    endforeach()
else()
    message(STATUS "octomx: demo core only (MX_UI_ONLY or no mxcore target)")
endif()

if(WIN32)
    set_target_properties(octomx PROPERTIES WIN32_EXECUTABLE TRUE)
    # [post-1] version info from the project version
    set(MX_RC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/res)
    configure_file(res/octomx.rc ${CMAKE_CURRENT_BINARY_DIR}/octomx.rc @ONLY)
    target_sources(octomx PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/octomx.rc)
    # the resource compiler does not report the icon as a dependency: rebuild the resources when it changes
    set_source_files_properties(${CMAKE_CURRENT_BINARY_DIR}/octomx.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/octomx.ico)
endif()

if(APPLE)
    # [mac-1] bin/octomx.app (OctoMX.app in the package, tools/mac/package.sh): Info.plist from the
    # project version (URL schemes, .torrent documents, local network text), icon from tools/make-icns.py
    set_target_properties(octomx PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/res/Info.plist.in)
    target_sources(octomx PRIVATE res/octomx.icns)
    set_source_files_properties(res/octomx.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    # App Nap opt-out through the Objective-C runtime (main.cpp keepRunningInBackground)
    target_link_libraries(octomx PRIVATE "-framework Foundation" objc)
endif()
