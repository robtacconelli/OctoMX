# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-2] legacy starter files below the UI: asset choice, the "bridged" layout, starter files kept
# across updates, crashes and roll-backs, a journal of an earlier version finished by this one
# (tst_update_bridge; uses the upd-1 test support, which needs MX_TEST_HOOKS).
if(MX_TEST_HOOKS)
    mx_ui_test(tst_update_bridge)
endif()
