# SPDX-License-Identifier: GPL-3.0-or-later
# [post-1] the owner requests for 4.1: colour theme, Check for Updates (local fake server only),
# no em / en dashes in user-visible texts
mx_ui_test(tst_theme)
if(MX_TEST_HOOKS)   # [upd-1] the self-update tests trust a throwaway key (MX_UPDATE_TEST_HOOKS)
    mx_ui_test(tst_update)
    # [safe-1] 65 s on an idle machine, 130 s on the shared one under load (also on the code before
    # safe-1): the default 120 s of the UI tests timed it out
    set_tests_properties(ui_tst_update PROPERTIES TIMEOUT 600)
endif()
mx_ui_test(tst_nodashes)
# the published source tree itself (tools/check-no-dashes.sh needs bash: not on a Windows host)
find_program(MX_BASH bash)
if(MX_BASH AND NOT WIN32)
    add_test(NAME tools_check_no_dashes COMMAND ${MX_BASH} ${CMAKE_SOURCE_DIR}/tools/check-no-dashes.sh ${CMAKE_SOURCE_DIR})
    # [ren-1] the program is called OctoMX: every remaining old name is explained by tools/names-allowlist.txt
    add_test(NAME tools_check_names COMMAND ${MX_BASH} ${CMAKE_SOURCE_DIR}/tools/check-names.sh ${CMAKE_SOURCE_DIR})
    # [demo-1] no names of real commercial works (demo data, tests, docs): tools/content-denylist.txt,
    # justified hits in tools/content-allowlist.txt
    add_test(NAME tools_check_content COMMAND ${MX_BASH} ${CMAKE_SOURCE_DIR}/tools/check-content.sh ${CMAKE_SOURCE_DIR})
endif()
