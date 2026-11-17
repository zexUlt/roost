# The plugins of fixture genex; the gate runs after all of them.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE $<$<BOOL:1>:roost_core>)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
