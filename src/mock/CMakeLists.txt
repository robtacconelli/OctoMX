# SPDX-License-Identifier: GPL-3.0-or-later
# mxmock: canned-data mx::Core for --demo, screenshots and UI tests (mx::createMockCore).
# Links mxapi + Qt Core only. Owner: ui-a.
add_library(mxmock STATIC
    mock_core.h
    mock_core.cpp
    mock_data.h
    mock_data.cpp
)
target_include_directories(mxmock PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(mxmock PUBLIC mxapi Qt6::Core)
