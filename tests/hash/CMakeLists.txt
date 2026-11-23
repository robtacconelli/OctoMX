# SPDX-License-Identifier: GPL-3.0-or-later
# mxhash unit tests (published test vectors). Owner: core task.
set(MX_HASH_TEST_TMP ${CMAKE_BINARY_DIR}/tmp)
file(MAKE_DIRECTORY ${MX_HASH_TEST_TMP})
add_executable(tst_hash tst_hash.cpp)
target_link_libraries(tst_hash PRIVATE mxhash Qt6::Test)
add_test(NAME hash COMMAND tst_hash)
set_tests_properties(hash PROPERTIES ENVIRONMENT "TMPDIR=${MX_HASH_TEST_TMP};QT_QPA_PLATFORM=offscreen" TIMEOUT 120)
