# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-h] 4.5: no control that does nothing: the settings registry, the settings made to work
mx_ui_test(tst_uih)
mx_ui_test(tst_settings_registry)
if(TARGET tst_settings_registry)
    # the registry check reads the source tree (the files the proofs point at)
    target_compile_definitions(tst_settings_registry PRIVATE MX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
