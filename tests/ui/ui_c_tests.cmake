# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-c] tests (included from tests/ui/CMakeLists.txt). Owner: ui-c.
mx_ui_test(tst_settings_dialog)
mx_ui_test(tst_tray_close)
mx_ui_test(tst_os_integration)
# the restart test drives the real executable twice (demo core, own profile under the build dir)
if(TARGET octomx)
    mx_ui_test(tst_restart)
    target_compile_definitions(tst_restart PRIVATE MX_OCTOMX_EXE="$<TARGET_FILE:octomx>")
    # [ui-j] Windows (wine): the Qt the child can run on: its DLLs and the offscreen platform plugin
    if(WIN32)
        target_compile_definitions(tst_restart PRIVATE MX_QT_BIN_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}"
                                                       MX_QT_PLUGINS_DIR="${QT6_INSTALL_PREFIX}/${QT6_INSTALL_PLUGINS}")
    endif()
    add_dependencies(tst_restart octomx)
endif()
