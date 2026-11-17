# The plugins of fixture includes_symlink: the include directory is a symlink
# to src/core, its path does not mention src/.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core)
target_include_directories(plugin_a INTERFACE ${CMAKE_BINARY_DIR}/shortcut)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
