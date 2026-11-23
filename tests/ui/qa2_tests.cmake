# SPDX-License-Identifier: GPL-3.0-or-later
# [qa-2] independent check of the modernised UI: regression tests for its findings
mx_ui_test(tst_qa2)
# the wording test reads the user-visible strings from the sources
target_compile_definitions(tst_qa2 PRIVATE MX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
