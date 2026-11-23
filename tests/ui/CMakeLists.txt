# SPDX-License-Identifier: GPL-3.0-or-later
# UI tests (Qt Test, offscreen). Owner: ui-a.
find_package(Qt6 REQUIRED COMPONENTS Test Widgets)

# [ui-j] MX_OS_ROOT: OS integration and the trash go to a folder of the build tree, never the real ones
function(mx_ui_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE mxui mxmock mxapi Qt6::Test Qt6::Widgets)
    add_test(NAME ui_${name} COMMAND ${name})
    set_tests_properties(ui_${name} PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=C.UTF-8;MX_DEMO_FROZEN=1;MX_PORT_MAPPING=0;MX_OS_ROOT=${CMAKE_BINARY_DIR}/os-root"
        TIMEOUT 120)
endfunction()

mx_ui_test(tst_format)
mx_ui_test(tst_search_model)
mx_ui_test(tst_transfer_model)
mx_ui_test(tst_piece_bar)   # [ui-i] the download bar as a map of the file
mx_ui_test(tst_colors)
mx_ui_test(tst_smoke)
mx_ui_test(tst_perf)

# [ui-c] Settings dialog, wizard, tray/close, OS integration, restart tests
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_c_tests.cmake OPTIONAL)

# [ui-b] Shared Files / Chat / Bandwidth pages, Browse / Whois / PM / Ignored windows
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_b_tests.cmake OPTIONAL)

# [ui-d] glue-2 / glue-3 wiring, settings clean-up, fonts, persistence
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_d_tests.cmake OPTIONAL)

# [qa-1] QA regression tests
include(${CMAKE_CURRENT_SOURCE_DIR}/qa1_tests.cmake OPTIONAL)

# [ui-e] modern usability in the OctoMX style
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_e_tests.cmake OPTIONAL)

# [qa-2] independent check of the modernised UI
include(${CMAKE_CURRENT_SOURCE_DIR}/qa2_tests.cmake OPTIONAL)

# [ui-f] honest, modern file types in Search
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_f_tests.cmake OPTIONAL)

# [post-1] 4.1: close / minimise, colour theme, Check for Updates, commas for dashes
include(${CMAKE_CURRENT_SOURCE_DIR}/post1_tests.cmake OPTIONAL)

# [ui-g] 4.3: full-width columns, results filter, tool tips, translations
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_g_tests.cmake OPTIONAL)

# [upd-1] 4.3: signed self-update
include(${CMAKE_CURRENT_SOURCE_DIR}/upd1_tests.cmake OPTIONAL)

# [bw-1] 4.3: speed limits one click away (Bandwidth page row, tray menu, title, schedule)
include(${CMAKE_CURRENT_SOURCE_DIR}/bw1_tests.cmake OPTIONAL)

# [dl-1] download order flags and Preview
include(${CMAKE_CURRENT_SOURCE_DIR}/dl1_tests.cmake OPTIONAL)

# [ui-h] settings registry, sounds, the settings made to work
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_h_tests.cmake OPTIONAL)

# [ui-j] removing transfers and shared files, with a choice about the file
include(${CMAKE_CURRENT_SOURCE_DIR}/ui_j_tests.cmake OPTIONAL)

# [logo-1] the OctoMX application icon
include(${CMAKE_CURRENT_SOURCE_DIR}/logo1_tests.cmake OPTIONAL)

# [upd-2] legacy starter files kept across updates, a journal of an earlier version
include(${CMAKE_CURRENT_SOURCE_DIR}/upd2_tests.cmake OPTIONAL)
