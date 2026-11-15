find_package(Qt6 REQUIRED COMPONENTS Test)

# Tests never touch the real ~/.config/roost or ~/.config/cc-fleet, and leave
# no QML disk cache behind in the user's cache directory.
set(ROOST_TEST_ENVIRONMENT
    QT_QPA_PLATFORM=offscreen
    QML_DISABLE_DISK_CACHE=1
)

# One executable per test file, so a crash in one does not hide the others.
function(roost_add_qt_test name source)
    qt_add_executable(${name} ${source})
    target_link_libraries(${name} PRIVATE roost_core Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ROOST_TEST_ENVIRONMENT}")
endfunction()

roost_add_qt_test(ghostty_formatter_test core/ghostty_formatter_test.cpp ghostty-vt-static)
roost_add_qt_test(bus_test core/bus_test.cpp)
roost_add_qt_test(frame_test core/frame_test.cpp)
roost_add_qt_test(rpc_test core/rpc_test.cpp)
roost_add_qt_test(store_test core/store_test.cpp)

# The window comes up and renders a frame under the offscreen platform.
add_test(NAME roost_smoke COMMAND roost --smoke)
set_tests_properties(roost_smoke PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT}"
    TIMEOUT 30
)

# --version names the libghostty-vt the binary actually links, read from the
# library rather than its headers.
string(REPLACE "." "\\." _ghostty_vt_version_regex "${ROOST_GHOSTTY_VT_VERSION}")
add_test(NAME roostd_version COMMAND roostd --version)
set_tests_properties(roostd_version PROPERTIES
    PASS_REGULAR_EXPRESSION "^roostd ${PROJECT_VERSION} \\(libghostty-vt ${_ghostty_vt_version_regex}[^)]*\\)"
)
