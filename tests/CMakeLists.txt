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
roost_add_qt_test(vt_render_state_test core/vt_render_state_test.cpp)
roost_add_qt_test(vt_key_encoder_test core/vt_key_encoder_test.cpp)
roost_add_qt_test(vt_mouse_encoder_test core/vt_mouse_encoder_test.cpp)
roost_add_qt_test(terminal_session_test terminal/terminal_session_test.cpp roost_terminal)
roost_add_qt_test(renderer_runs_test terminal/renderer_runs_test.cpp roost_terminal)
roost_add_qt_test(qt_key_mapping_test terminal/qt_key_mapping_test.cpp roost_terminal)
roost_add_qt_test(default_font_test terminal/default_font_test.cpp roost_terminal)
roost_add_qt_test(terminal_item_test qml_api/terminal_item_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_input_test qml_api/terminal_item_input_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_mouse_test qml_api/terminal_item_mouse_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_selection_test qml_api/terminal_item_selection_test.cpp roost_qml_api)

# The frame benchmark (design.md section 9): ten sessions fed 1 MB/s each by one
# writer, the GUI thread's time per frame measured apart from rendering. Offscreen has no
# RHI, so Qt Quick draws in software, whose render thread exists only with
# QSG_RENDER_LOOP=threaded (prior-art section 5.1).
roost_add_qt_test(frame_budget_test bench/frame_budget_test.cpp roost_qml_api)
set_tests_properties(frame_budget_test PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};QSG_RENDER_LOOP=threaded"
    TIMEOUT 120
    # The budget is a timing check: never beside other tests under ctest -j.
    RUN_SERIAL TRUE
)

# A TUI that draws, asks its terminal questions and counts the answers.
add_executable(fake_tui helpers/fake_tui.cpp)
target_link_libraries(fake_tui PRIVATE roost::compile_options)

roost_add_qt_test(pty_test core/pty_test.cpp roost_test_helpers)
target_compile_definitions(pty_test PRIVATE FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>")
add_dependencies(pty_test fake_tui)
roost_add_qt_test(server_test roostd/server_test.cpp roostd_lib)
roost_add_qt_test(channels_test roostd/channels_test.cpp roostd_lib)
roost_add_qt_test(warm_pool_test roostd/warm_pool_test.cpp roostd_lib)
target_compile_definitions(warm_pool_test PRIVATE FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>")
add_dependencies(warm_pool_test fake_tui)
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
roost_add_qt_test(plugins_list_test roostd/plugins_list_test.cpp roostd_lib)
roost_add_qt_test(store_service_test roostd/store_service_test.cpp roostd_lib)
roost_add_qt_test(agents_test roostd/agents_test.cpp roostd_lib)
target_compile_definitions(agents_test PRIVATE
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(agents_test fixture_backend)
roost_add_qt_test(session_channels_test roostd/session_channels_test.cpp roostd_lib)
target_compile_definitions(session_channels_test PRIVATE
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(session_channels_test fake_tui fixture_backend)

# The real binary, started as a process.
roost_add_qt_test(roostd_test roostd/roostd_test.cpp)
target_compile_definitions(roostd_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(roostd_test roostd fixture_backend)
roost_add_qt_test(roostd_store_test roostd/roostd_store_test.cpp)
target_compile_definitions(roostd_store_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>")
add_dependencies(roostd_store_test roostd)

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

# The real roostd with the real Claude plugin, behind a fake `claude`.
roost_add_qt_test(claude_e2e_test plugins/claude_e2e_test.cpp)
target_compile_definitions(claude_e2e_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(claude_e2e_test roostd fake_tui claude-adapter)

# The window's IO thread against the real roostd, the real Claude plugin and a
# fake `claude`.
roost_add_qt_test(io_thread_test terminal/io_thread_test.cpp roost_terminal)
target_compile_definitions(io_thread_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(io_thread_test roostd fake_tui claude-adapter)

# The developer mode: one TerminalItem wired to a session of the real roostd,
# and the roost executable started with --session against it.
roost_add_qt_test(single_session_test roost/single_session_test.cpp roost_app)
target_compile_definitions(single_session_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    ROOST_PATH="$<TARGET_FILE:roost>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(single_session_test roostd roost fake_tui claude-adapter)

# The tests whose code runs on more than one thread carry the label
# `threads`. Those whose threads synchronise only through std primitives also
# carry `threads_std`: the tsan preset, against a binary Qt, runs only them.
# The rest hand data over through Qt (the event queue, the scene-graph sync),
# which TSan sees only in a Qt built with it: the tsan-qt preset, run nightly.
set_tests_properties(terminal_session_test PROPERTIES LABELS "threads;threads_std")
set_tests_properties(
    io_thread_test
    single_session_test
    terminal_item_test
    terminal_item_input_test
    terminal_item_mouse_test
    terminal_item_selection_test
    PROPERTIES LABELS threads
)

# Memory of one terminal in the window (docs/prior-art.md §2.5). Not a test:
# not registered with ctest, built only where it can read the process memory.
if(APPLE)
    add_executable(terminal_memory bench/terminal_memory.cpp)
    target_link_libraries(terminal_memory PRIVATE roost_terminal)
endif()
