# SPDX-License-Identifier: GPL-3.0-or-later
# [upd-1] the signed self-update: signature / hash / manifest / zip / swap journal / download against a
# local fake server (tst_update_install); the UI flows are in tst_update (post1_tests.cmake). Both
# need the test key hook, which only exists with MX_TEST_HOOKS=ON (never in a release build).
if(MX_TEST_HOOKS)
    mx_ui_test(tst_update_install)
endif()
