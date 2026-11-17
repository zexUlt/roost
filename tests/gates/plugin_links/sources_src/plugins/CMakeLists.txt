# The plugins of fixture sources_src: a plugin compiles a file of src/.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core)
target_sources(plugin_a INTERFACE ${PROJECT_SOURCE_DIR}/src/core/x.cpp)

# Relative to the plugin directory, in SOURCES.
add_library(plugin_b INTERFACE)
target_link_libraries(plugin_b INTERFACE Qt6::Core)
target_sources(plugin_b PRIVATE ../src/core/x.cpp)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
