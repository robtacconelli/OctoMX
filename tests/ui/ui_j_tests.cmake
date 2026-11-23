# SPDX-License-Identifier: GPL-3.0-or-later
# [ui-j] removing a download (keep the file or delete it), Shared Files Delete File... / Delete Folder...,
# the trash seam, the results filter of exclusions only, eD2k / Kad source wording
mx_ui_test(tst_uij)
if(TARGET tst_uij)
    # its files live in the build tree, outside the program's folder (bin/), which Shared Files never deletes in
    target_compile_definitions(tst_uij PRIVATE MX_UIJ_WORK_DIR="${CMAKE_BINARY_DIR}/tst_uij_work")
endif()
