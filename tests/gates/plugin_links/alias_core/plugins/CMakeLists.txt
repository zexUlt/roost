# The plugins of fixture alias_core; the gate runs after all of them.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core roost::core)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
