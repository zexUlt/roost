# A built-in adapter with no dependency on roostd internals (design.md §7).
set(ROOST_CODEX_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/codex)

qt_add_executable(codex-adapter backend/adapter.cpp backend/main.cpp)
target_link_libraries(codex-adapter PRIVATE roost_plugin_backend)
set_target_properties(codex-adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ROOST_CODEX_PLUGIN_DIR}/backend)

configure_file(plugin.json ${ROOST_CODEX_PLUGIN_DIR}/plugin.json COPYONLY)

install(FILES plugin.json DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/codex
    COMPONENT roost)
roost_install_plugin_binary(codex codex-adapter backend)
