# The plugins of fixture includes_src; the gate runs after all of them.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core)
target_include_directories(plugin_a INTERFACE ${PROJECT_SOURCE_DIR}/src/core)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
