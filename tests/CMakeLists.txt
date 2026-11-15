find_package(Qt6 REQUIRED COMPONENTS Test)

# Tests never touch the real ~/.config/roost or ~/.config/cc-fleet, and leave
# no QML disk cache behind in the user's cache directory.
set(ROOST_TEST_ENVIRONMENT
    QT_QPA_PLATFORM=offscreen
    QML_DISABLE_DISK_CACHE=1
)

# Shared by several tests.
add_library(roost_test_helpers STATIC
    helpers/scoped_env.cpp
)
target_include_directories(roost_test_helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(roost_test_helpers PUBLIC roost::compile_options Qt6::Core)

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
roost_add_qt_test(store_test core/store_test.cpp roost_test_helpers)
roost_add_qt_test(vt_terminal_test core/vt_terminal_test.cpp)

# A TUI that draws, asks its terminal questions and counts the answers.
add_executable(fake_tui helpers/fake_tui.cpp)
target_link_libraries(fake_tui PRIVATE roost::compile_options)

roost_add_qt_test(pty_test core/pty_test.cpp roost_test_helpers)
target_compile_definitions(pty_test PRIVATE FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>")
add_dependencies(pty_test fake_tui)
roost_add_qt_test(server_test roostd/server_test.cpp roostd_lib)
roost_add_qt_test(manifest_test roostd/manifest_test.cpp roostd_lib roost_test_helpers)

# A plugin backend that misbehaves on request.
qt_add_executable(fixture_backend helpers/fixture_backend.cpp)
target_link_libraries(fixture_backend PRIVATE roost::compile_options Qt6::Core)

roost_add_qt_test(backend_test roostd/backend_test.cpp roostd_lib)
target_compile_definitions(backend_test PRIVATE
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(backend_test fixture_backend)

roost_add_qt_test(dump_test roostd/dump_test.cpp roostd_lib)
roost_add_qt_test(plugins_test roostd/plugins_test.cpp roostd_lib)
target_compile_definitions(plugins_test PRIVATE
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(plugins_test fixture_backend)
roost_add_qt_test(agents_test roostd/agents_test.cpp roostd_lib)
target_compile_definitions(agents_test PRIVATE
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(agents_test fixture_backend)

# The real binary, started as a process.
roost_add_qt_test(roostd_test roostd/roostd_test.cpp)
target_compile_definitions(roostd_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(roostd_test roostd fixture_backend)

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

# The Claude adapter: its parsing, then the real binary over stdio.
roost_add_qt_test(claude_adapter_test plugins/claude_adapter_test.cpp claude_adapter_lib)
target_compile_definitions(claude_adapter_test PRIVATE
    CLAUDE_ADAPTER_PATH="$<TARGET_FILE:claude-adapter>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(claude_adapter_test claude-adapter)
