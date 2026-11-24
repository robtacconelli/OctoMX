# SPDX-License-Identifier: GPL-3.0-or-later
# [safe-1] the safe quit: the closing window (a stand-in core); the real program stopped by signals,
# by a quit while the disk is slow and by "Close now" (Linux and macOS: signals)
mx_ui_test(tst_safe_quit)
if(TARGET octomx AND NOT WIN32)
    add_executable(tst_safe_app tst_safe_app.cpp ${CMAKE_SOURCE_DIR}/tests/http/test_server.cpp ${CMAKE_SOURCE_DIR}/tests/http/test_server.h)
    target_link_libraries(tst_safe_app PRIVATE Qt6::Test Qt6::Network)
    target_include_directories(tst_safe_app PRIVATE ${CMAKE_SOURCE_DIR}/tests/http)
    target_compile_definitions(tst_safe_app PRIVATE MX_OCTOMX_EXE="$<TARGET_FILE:octomx>")
    add_dependencies(tst_safe_app octomx)
    add_test(NAME ui_tst_safe_app COMMAND tst_safe_app)
    set_tests_properties(ui_tst_safe_app PROPERTIES
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;LC_ALL=C.UTF-8;MX_PORT_MAPPING=0;MX_OS_ROOT=${CMAKE_BINARY_DIR}/os-root"
        TIMEOUT 900)
endif()
