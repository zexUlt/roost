# The plugins of fixture includes_root: the source root is an ancestor of src/,
# so #include "src/core/x.hpp" works.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core)
target_include_directories(plugin_a INTERFACE ${PROJECT_SOURCE_DIR})

add_library(plugin_b INTERFACE)
target_link_libraries(plugin_b INTERFACE Qt6::Core)
target_include_directories(plugin_b INTERFACE ${PROJECT_SOURCE_DIR}/src/..)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
