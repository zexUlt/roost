# The plugins of fixture interface_link; the gate runs after all of them.
add_library(plugin_a INTERFACE)
set_target_properties(plugin_a PROPERTIES INTERFACE_LINK_LIBRARIES roost_core)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
