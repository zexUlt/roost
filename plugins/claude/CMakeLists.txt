# The Claude adapter (design.md §7): build/<preset>/roost-plugins/claude holds
# plugin.json and backend/claude-adapter.
set(ROOST_CLAUDE_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/claude)

# Everything of the adapter but main(), so that tests drive the real thing.
add_library(claude_adapter_lib STATIC
    backend/adapter.cpp
    backend/agents.cpp
)
target_include_directories(claude_adapter_lib PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(claude_adapter_lib PUBLIC roost_core)

qt_add_executable(claude-adapter backend/main.cpp)
target_link_libraries(claude-adapter PRIVATE claude_adapter_lib)
set_target_properties(claude-adapter PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${ROOST_CLAUDE_PLUGIN_DIR}/backend)

configure_file(plugin.json ${ROOST_CLAUDE_PLUGIN_DIR}/plugin.json COPYONLY)
