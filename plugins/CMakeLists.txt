# Built-in plugins (design.md §5), each laid out under build/<preset>/roost-plugins
# as roostd finds it: point ROOST_PLUGINS_PATH there.
set(ROOST_PLUGINS_OUTPUT_DIR ${PROJECT_BINARY_DIR}/roost-plugins)

add_subdirectory(claude)
