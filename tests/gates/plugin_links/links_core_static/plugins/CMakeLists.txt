# The plugins of fixture links_core_static; the gate runs after all of them.
add_library(plugin_a STATIC plugin_a.c)
target_link_libraries(plugin_a PRIVATE roost_core)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
