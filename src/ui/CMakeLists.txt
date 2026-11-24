# SPDX-License-Identifier: GPL-3.0-or-later
# mxui: everything visible: the classic WinMX look (kit/), models, pages, dialogs, main window and
# the --script driver. Links ONLY mxapi + Qt Widgets (never mxcore or a plugin). Owner: ui-a.
# Sources are globbed (re-run cmake after adding files) so follow-up UI workers can add pages
# without touching this file.
find_package(Qt6 REQUIRED COMPONENTS Widgets)

file(GLOB MXUI_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/kit/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/kit/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/models/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/models/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/pages/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/pages/*.h
    ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/dialogs/*.h)

add_library(mxui STATIC ${MXUI_SOURCES})
# Includes are written as "ui/kit/xxx.h", "ui/pages/xxx.h" ...
target_include_directories(mxui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mxui PUBLIC mxapi Qt6::Core Qt6::Gui Qt6::Widgets)
# [ui-h] alert sounds (kit/sound.cpp): PlaySound
if(WIN32)
    target_link_libraries(mxui PUBLIC winmm shlwapi)   # [ui-h] + AssocQueryString (File Launching verbs)
endif()
if(APPLE)
    # [mac-1] Launch Services (os_integration_mac.cpp)
    target_link_libraries(mxui PUBLIC "-framework CoreServices" "-framework CoreFoundation")
endif()

# Bundled fonts: Wine's metric-compatible Tahoma (LGPL-2.1+), with the embedded-bitmap line
# metrics fixed to match Windows (see res/fonts/patch_tahoma.py). Sources: third_party/fonts.
set(MX_UI_FONTS ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahoma.ttf ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahomabd.ttf)
if(APPLE)
    # [mac-1] the macOS copies: advances = the 11-ppem bitmap advances (Core Text positions glyphs by
    # the advances, see patch_tahoma.py --mac-advances), under the same resource names
    set(MX_UI_FONTS ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahoma-mac.ttf ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahomabd-mac.ttf)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahoma-mac.ttf PROPERTIES QT_RESOURCE_ALIAS tahoma.ttf)
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/tahomabd-mac.ttf PROPERTIES QT_RESOURCE_ALIAS tahomabd.ttf)
endif()
qt_add_resources(mxui mxui_fonts
    PREFIX "/mx/fonts"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts
    FILES ${MX_UI_FONTS}
          ${CMAKE_CURRENT_SOURCE_DIR}/res/fonts/COPYING.LGPL-2.1)
qt_add_resources(mxui mxui_res
    PREFIX "/mx"
    BASE ${CMAKE_CURRENT_SOURCE_DIR}/res
    FILES ${CMAKE_CURRENT_SOURCE_DIR}/res/licenses.txt)
