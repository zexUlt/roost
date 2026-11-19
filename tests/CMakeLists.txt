find_package(Qt6 REQUIRED COMPONENTS Test)

# Tests never touch the real ~/.config/roost or another program's config, and
# leave no QML disk cache behind in the user's cache directory.
set(ROOST_TEST_ENVIRONMENT
    QT_QPA_PLATFORM=offscreen
    QML_DISABLE_DISK_CACHE=1
)

# ctest runs the tests in parallel (execution.jobs in CMakePresets.json). Each
# test uses a temporary directory and a socket path of its own, so none
# needs a lock today. A test that must share a path, a port or a lock file with
# another gets RESOURCE_LOCK with a comment on what it shares; RUN_SERIAL is for
# timing checks only (frame_budget_test).

# Shared by several tests.
add_library(roost_test_helpers STATIC
    helpers/detached_daemons.cpp
    helpers/refusing_server.cpp
    helpers/roostd_process.cpp
    helpers/scoped_env.cpp
    helpers/window_teardown.cpp
)
target_include_directories(roost_test_helpers PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(roost_test_helpers PUBLIC
    roost::compile_options roost_core Qt6::Core Qt6::Gui Qt6::Test)

# Helpers for the tests of terminal channels; apart, so the others do not link
# roost_terminal.
add_library(roost_test_terminal_helpers STATIC
    helpers/channels_left.cpp
    helpers/counting_proxy.cpp
)
target_link_libraries(roost_test_terminal_helpers PUBLIC
    roost_test_helpers roost_terminal)

# The window of the shell with the sessions panel, for the tests of that panel.
add_library(roost_test_sessions_window STATIC helpers/sessions_window.cpp)
target_link_libraries(roost_test_sessions_window PUBLIC
    roost_test_helpers roost_app roost_shell roost_qml_api roost_api
    Qt6::Qml Qt6::Quick)
target_compile_definitions(roost_test_sessions_window PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")

# One executable per test file, so a crash in one does not hide the others.
function(roost_add_qt_test name source)
    # test_environment.cpp is a source, not a library member: the linker may drop
    # an unreferenced object of a static library.
    qt_add_executable(${name} ${source}
        ${CMAKE_CURRENT_SOURCE_DIR}/helpers/test_environment.cpp)
    target_link_libraries(${name} PRIVATE roost_core Qt6::Test ${ARGN})
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES ENVIRONMENT "${ROOST_TEST_ENVIRONMENT}")
endfunction()

roost_add_qt_test(ghostty_formatter_test core/ghostty_formatter_test.cpp ghostty-vt-static)
roost_add_qt_test(bus_test core/bus_test.cpp)
roost_add_qt_test(test_environment_test core/test_environment_test.cpp Qt6::Gui)
# The same binary with the variables removed: the guard alone must set them.
add_test(NAME test_environment_unset
    COMMAND ${CMAKE_COMMAND} -E env --unset=QT_QPA_PLATFORM --unset=QML_DISABLE_DISK_CACHE
        $<TARGET_FILE:test_environment_test>)
roost_add_qt_test(frame_test core/frame_test.cpp)
roost_add_qt_test(rpc_test core/rpc_test.cpp)
roost_add_qt_test(rpc_client_peer_pid_test core/rpc_client_peer_pid_test.cpp
    roost_test_helpers)
roost_add_qt_test(store_test core/store_test.cpp roost_test_helpers)
roost_add_qt_test(vt_terminal_test core/vt_terminal_test.cpp)
roost_add_qt_test(vt_render_state_test core/vt_render_state_test.cpp)
roost_add_qt_test(vt_key_encoder_test core/vt_key_encoder_test.cpp)
roost_add_qt_test(vt_mouse_encoder_test core/vt_mouse_encoder_test.cpp)
roost_add_qt_test(terminal_session_test terminal/terminal_session_test.cpp roost_terminal)
roost_add_qt_test(renderer_runs_test terminal/renderer_runs_test.cpp roost_terminal)
roost_add_qt_test(qt_key_mapping_test terminal/qt_key_mapping_test.cpp roost_terminal)
roost_add_qt_test(physical_keys_test terminal/physical_keys_test.cpp roost_terminal)
roost_add_qt_test(qt_key_layout_test terminal/qt_key_layout_test.cpp roost_terminal)
roost_add_qt_test(default_font_test terminal/default_font_test.cpp roost_terminal)
roost_add_qt_test(terminal_item_test qml_api/terminal_item_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_input_test qml_api/terminal_item_input_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_layout_test qml_api/terminal_item_layout_test.cpp roost_qml_api)
roost_add_qt_test(roost_binding_key_test qml_api/roost_binding_key_test.cpp
    roost_qml_api roost_qml_apiplugin Qt6::Qml)
roost_add_qt_test(terminal_item_mouse_test qml_api/terminal_item_mouse_test.cpp roost_qml_api)
roost_add_qt_test(terminal_item_selection_test qml_api/terminal_item_selection_test.cpp roost_qml_api)

# The frame benchmark (design.md section 9): ten sessions fed 1 MB/s each by one
# writer, the GUI thread's time per frame measured apart from rendering. Offscreen has no
# RHI, so Qt Quick draws in software, whose render thread exists only with
# QSG_RENDER_LOOP=threaded (prior-art section 5.1).
roost_add_qt_test(frame_budget_test bench/frame_budget_test.cpp roost_qml_api
    roost_test_helpers)
set_tests_properties(frame_budget_test PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};QSG_RENDER_LOOP=threaded"
    TIMEOUT 120
    # The budget is a timing check: never beside other tests under ctest -j.
    RUN_SERIAL TRUE
)

# A roostd of another version as a process, with the socket lock of roostd, for
# daemon_restart_test.
add_executable(refusing_daemon helpers/refusing_daemon.cpp)
target_link_libraries(refusing_daemon PRIVATE roost_test_helpers)

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
roost_add_qt_test(log_file_test roostd/log_file_test.cpp roostd_lib)
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
add_test(NAME roost_smoke COMMAND roost --no-spawn --smoke)
set_tests_properties(roost_smoke PROPERTIES
    # No roostd to find: the window waits for one (--no-spawn: it does not
    # start one), and the real socket of the user is left alone.
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};ROOST_SOCKET=${CMAKE_CURRENT_BINARY_DIR}/roost_smoke.sock"
    TIMEOUT 30
)

# The developer mode starts too, with no roostd to find.
add_test(NAME roost_smoke_dev COMMAND roost --no-spawn --dev --smoke)
set_tests_properties(roost_smoke_dev PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};ROOST_SOCKET=${CMAKE_CURRENT_BINARY_DIR}/roost_smoke_dev.sock"
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

# ClaudeRun on a run that cannot start, in a child process of its own.
roost_add_qt_test(claude_run_test plugins/claude_run_test.cpp claude_adapter_lib)

# The commands of the Claude plugin: the adapter binary behind a fake `claude`,
# and the manifest of the built plugin through the real roostd.
roost_add_qt_test(claude_commands_test plugins/claude_commands_test.cpp roost_api)
target_compile_definitions(claude_commands_test PRIVATE
    CLAUDE_ADAPTER_PATH="$<TARGET_FILE:claude-adapter>"
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(claude_commands_test roostd claude-adapter)

# The real roostd with the real Claude plugin, behind a fake `claude`.
roost_add_qt_test(claude_e2e_test plugins/claude_e2e_test.cpp roost_api)
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

# The channel logic of a terminal panel against the real roostd with two
# fixture plugins whose agents attach to fake_tui.
roost_add_qt_test(session_pane_test terminal/session_pane_test.cpp
    roost_terminal roost_test_terminal_helpers)
target_compile_definitions(session_pane_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(session_pane_test roostd fake_tui fixture_backend)

# The warm terminals of the window against the real roostd, fixture plugins
# whose agents attach to fake_tui: what stays warm, what is dropped, what
# roostd is told.
roost_add_qt_test(warm_terminals_test terminal/warm_terminals_test.cpp
    roost_terminal roost_test_terminal_helpers)
target_compile_definitions(warm_terminals_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(warm_terminals_test roostd fake_tui fixture_backend)

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

# The window's API against the real roostd with a plugin whose backend is the
# fixture: the services and the dispatcher, the thread check.
foreach(_api_test api_dispatcher_test api_bus_test api_connection_test
        api_sessions_test api_store_test api_reconnect_test
        api_interleaving_test api_unavailable_test)
    roost_add_qt_test(${_api_test} api/${_api_test}.cpp roost_api roost_test_helpers)
    target_compile_definitions(${_api_test} PRIVATE
        ROOSTD_PATH="$<TARGET_FILE:roostd>"
        FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
    add_dependencies(${_api_test} roostd fixture_backend)
endforeach()
roost_add_qt_test(api_thread_check_test api/api_thread_check_test.cpp roost_api)

# The singleton Roost of module Roost.Api, as QML calls it, against the real
# roostd and the fixture backend.
roost_add_qt_test(roost_calls_test qml_api/roost_calls_test.cpp
    roost_qml_api roost_qml_apiplugin roost_test_helpers Qt6::Qml)
target_compile_definitions(roost_calls_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(roost_calls_test roostd fixture_backend)

# The owner of the calls from QML: a panel's calls end with the panel.
roost_add_qt_test(roost_call_owner_test qml_api/roost_call_owner_test.cpp
    roost_qml_api roost_qml_apiplugin roost_test_helpers Qt6::Qml)
target_compile_definitions(roost_call_owner_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(roost_call_owner_test roostd fixture_backend)

# The window's loader of plugin panels, against the real roostd with plugins
# that have QML panels and the fixture backend.
roost_add_qt_test(plugin_loader_test roost/plugin_loader_test.cpp
    roost_app roost_qml_apiplugin roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(plugin_loader_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(plugin_loader_test roostd fixture_backend)

# The window's shell: Main.qml with its slots, the loader and the real roostd,
# and a refusing server for the version check.
roost_add_qt_test(shell_test roost/shell_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(shell_test PRIVATE ROOSTD_PATH="$<TARGET_FILE:roostd>")
add_dependencies(shell_test roostd)

# The plugin terminal: its panel terminal.view in the window of the shell, with
# a fixture list, against the real roostd with the real Claude plugin behind a
# fake claude.
roost_add_qt_test(terminal_panel_test roost/terminal_panel_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(terminal_panel_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins"
    TERMINAL_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins/terminal")
add_dependencies(terminal_panel_test roostd fake_tui claude-adapter)

# The plugin sessions: its panel sessions.list in the window of the shell, with
# the real terminal panel, against the real roostd with the real Claude plugin
# behind a fake claude.
roost_add_qt_test(sessions_panel_test roost/sessions_panel_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_sessions_window roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(sessions_panel_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins"
    SESSIONS_PLUGIN_DIR="${PROJECT_SOURCE_DIR}/plugins/sessions")
add_dependencies(sessions_panel_test roostd fake_tui claude-adapter sessions_qml)

# The view of the sessions panel: the head, the marks and the keys of the view.
roost_add_qt_test(sessions_view_test roost/sessions_view_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_sessions_window roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(sessions_view_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(sessions_view_test roostd fake_tui claude-adapter sessions_qml)

# The commands of the sessions panel: d, x, r, R and n, their confirmations and
# notices, and the reopen of a respawned session.
roost_add_qt_test(sessions_commands_test roost/sessions_commands_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_sessions_window roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(sessions_commands_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(sessions_commands_test roostd fake_tui claude-adapter sessions_qml)

# The prompt of the sessions panel's footer: search, category and name.
roost_add_qt_test(sessions_prompt_test roost/sessions_prompt_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_sessions_window roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(sessions_prompt_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(sessions_prompt_test roostd fake_tui claude-adapter sessions_qml)

# Warm terminals in the window: a switch to a warm session is one frame, and a
# window that restarts finds its session warm in roostd. Same setup as
# terminal_panel_test, with a proxy that counts the opens.
roost_add_qt_test(warm_switch_test roost/warm_switch_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_api roost_qml_apiplugin
    roost_test_terminal_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(warm_switch_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(warm_switch_test roostd fake_tui claude-adapter)
# One binary under both render loops: the test checks that the sync ran in the
# thread the loop promises. Offscreen has a render thread only when threaded.
set_tests_properties(warm_switch_test PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};QSG_RENDER_LOOP=basic")
add_test(NAME warm_switch_test_threaded COMMAND warm_switch_test)
set_tests_properties(warm_switch_test_threaded PROPERTIES
    ENVIRONMENT "${ROOST_TEST_ENVIRONMENT};QSG_RENDER_LOOP=threaded"
    LABELS threads)

# The developer mode: the watcher reloads a plugin whose files changed on disk.
roost_add_qt_test(plugin_reload_test roost/plugin_reload_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(plugin_reload_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>")
add_dependencies(plugin_reload_test roostd)

# The template templates/plugin-qml: a copy of it with another id, loaded by the
# real roostd, its panel opened in the window. The template is read from the
# source tree; the build does not install it.
roost_add_qt_test(plugin_template_test roost/plugin_template_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(plugin_template_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    PLUGIN_TEMPLATE_DIR="${PROJECT_SOURCE_DIR}/templates/plugin-qml")
add_dependencies(plugin_template_test roostd)

# Roost.sessions, the list model over the session cache, as QML reads it.
roost_add_qt_test(roost_sessions_test qml_api/roost_sessions_test.cpp
    roost_qml_api roost_qml_apiplugin roost_test_helpers Qt6::Qml)
target_compile_definitions(roost_sessions_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(roost_sessions_test roostd fixture_backend)

# The `session` property of TerminalItem: the item opens, draws and feeds a
# session of the real roostd by itself.
roost_add_qt_test(terminal_item_session_test qml_api/terminal_item_session_test.cpp
    roost_qml_api roost_qml_apiplugin roost_test_terminal_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(terminal_item_session_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FAKE_TUI_PATH="$<TARGET_FILE:fake_tui>"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>")
add_dependencies(terminal_item_session_test roostd fake_tui fixture_backend)

# The tests whose code runs on more than one thread carry the label
# `threads`. Those whose threads synchronise only through std primitives also
# carry `threads_std`: the tsan preset, against a binary Qt, runs only them.
# The rest hand data over through Qt (the event queue, the scene-graph sync),
# which TSan sees only in a Qt built with it: the tsan-qt preset, run nightly.
set_tests_properties(terminal_session_test PROPERTIES LABELS "threads;threads_std")
set_tests_properties(
    api_dispatcher_test
    api_bus_test
    api_connection_test
    api_interleaving_test
    api_reconnect_test
    api_sessions_test
    api_store_test
    api_thread_check_test
    api_unavailable_test
    roost_calls_test
    roost_call_owner_test
    plugin_loader_test
    plugin_reload_test
    shell_test
    terminal_panel_test
    sessions_panel_test
    warm_switch_test
    roost_sessions_test
    io_thread_test
    single_session_test
    session_pane_test
    warm_terminals_test
    terminal_item_test
    terminal_item_session_test
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

# The rows of the sessions list, in plain C++.
roost_add_qt_test(sessions_rows_test plugins/sessions_rows_test.cpp sessions_list_lib)

# What the sessions list remembers and derives: the store document, colours,
# names, unread.
roost_add_qt_test(sessions_state_test plugins/sessions_state_test.cpp sessions_list_lib)

# SessionsList, the QML-facing model of the sessions list, over a fake source,
# and the module RoostSessions loaded from the plugin directory as the window
# does.
roost_add_qt_test(sessions_list_test plugins/sessions_list_test.cpp
    sessions_list_lib roost_qml_api roost_qml_apiplugin Qt6::Qml)
target_compile_definitions(sessions_list_test PRIVATE
    SESSIONS_QML_IMPORT_PATH="${PROJECT_BINARY_DIR}/roost-plugins/sessions/qml")
add_dependencies(sessions_list_test sessions_qml)

# The install layout: `cmake --install` of this build into a temporary prefix,
# and the installed roostd against a fake `claude`. The test installs by itself,
# so the targets it installs are built first.
# The directory of Qt that the installed binaries must have in their run path
# (ROOST_INSTALL_RPATH_TO_QT): where the Qt of the build is, unless that is a
# directory the linker searches anyway, which CMake leaves out.
set(ROOST_TEST_QT_RUN_PATH "")
if(ROOST_INSTALL_RPATH_TO_QT)
    get_filename_component(_qt_lib_dir "${Qt6Core_DIR}/../.." ABSOLUTE)
    if(NOT _qt_lib_dir IN_LIST CMAKE_CXX_IMPLICIT_LINK_DIRECTORIES)
        set(ROOST_TEST_QT_RUN_PATH "${_qt_lib_dir}")
    endif()
endif()
roost_add_qt_test(install_layout_test install/install_layout_test.cpp)
target_compile_definitions(install_layout_test PRIVATE
    CMAKE_COMMAND="${CMAKE_COMMAND}"
    BUILD_DIR="${PROJECT_BINARY_DIR}"
    SOURCE_DIR="${PROJECT_SOURCE_DIR}"
    PROJECT_VERSION_STRING="${PROJECT_VERSION}"
    QT_RUN_PATH="${ROOST_TEST_QT_RUN_PATH}"
    CLAUDE_ADAPTER_FILE="$<TARGET_FILE_NAME:claude-adapter>"
    SESSIONS_MODULE_FILE="$<TARGET_FILE_NAME:sessions_qml>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(install_layout_test roost roostd claude-adapter sessions_qml)

# `roost --smoke-sessions` as a process: the real roostd with the built-in
# plugins behind a fake `claude`; a sessions plugin without its QML module and
# a claude with no sessions must fail it.
roost_add_qt_test(smoke_sessions_test roost/smoke_sessions_test.cpp roost_test_helpers)
target_compile_definitions(smoke_sessions_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    ROOST_PATH="$<TARGET_FILE:roost>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    FIXTURE_BACKEND_PATH="$<TARGET_FILE:fixture_backend>"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(smoke_sessions_test roostd roost claude-adapter sessions_qml
    fixture_backend)

# The window starts roostd when nobody listens on the socket: the launcher with
# a real Api and the real roostd (detached; the test stops it by its lock).
roost_add_qt_test(daemon_launcher_test roost/daemon_launcher_test.cpp
    roost_app roost_api roost_test_helpers)
target_compile_definitions(daemon_launcher_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>")
add_dependencies(daemon_launcher_test roostd)

# The environment of the roostd that the window starts from the desktop: the
# login shell (a fake $SHELL script, never the user's) read asynchronously, the
# bundle variables stripped, and the real roostd finding a fake `claude` only
# through the login PATH.
roost_add_qt_test(login_environment_test roost/login_environment_test.cpp
    roost_app roost_api roost_test_helpers)
target_compile_definitions(login_environment_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
    ROOST_PLUGINS_DIR="${PROJECT_BINARY_DIR}/roost-plugins")
add_dependencies(login_environment_test roostd claude-adapter)

# The same as a process: `roost --smoke-sessions` with no roostd running starts
# one that outlives the window and that the next window finds.
roost_add_qt_test(window_spawn_test roost/window_spawn_test.cpp roost_test_helpers)
target_compile_definitions(window_spawn_test PRIVATE
    ROOST_PATH="$<TARGET_FILE:roost>"
    FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_dependencies(window_spawn_test roostd roost claude-adapter sessions_qml)

# The banner "Starting roostd…" of the shell while the launcher has a start in
# flight, and none for a refusal.
roost_add_qt_test(shell_starting_test roost/shell_starting_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(shell_starting_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>")
add_dependencies(shell_starting_test roostd)

# The Restart roostd button of the banner on VersionMismatch: the old roostd (a
# helper that holds the lock like roostd) gets SIGTERM only on the click and
# only if the lock is roostd's of this host; then the real roostd is started.
roost_add_qt_test(daemon_restart_test roost/daemon_restart_test.cpp
    roost_app roost_shell roost_shellplugin roost_qml_apiplugin
    roost_test_helpers Qt6::Qml Qt6::Quick)
target_compile_definitions(daemon_restart_test PRIVATE
    ROOSTD_PATH="$<TARGET_FILE:roostd>"
    REFUSING_DAEMON_PATH="$<TARGET_FILE:refusing_daemon>")
add_dependencies(daemon_restart_test roostd refusing_daemon)
