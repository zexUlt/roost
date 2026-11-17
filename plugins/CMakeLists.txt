# Built-in plugins (design.md §5), each laid out under build/<preset>/roost-plugins
# as roostd finds it: point ROOST_PLUGINS_PATH there.
set(ROOST_PLUGINS_OUTPUT_DIR ${PROJECT_BINARY_DIR}/roost-plugins)

add_subdirectory(claude)
add_subdirectory(terminal)

# Built-in plugins use the public API only, like third-party ones (design.md
# §5): this fails the configure if one links an internal target or includes
# from src/.
roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
