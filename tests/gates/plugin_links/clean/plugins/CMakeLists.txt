# The plugins of fixture clean; the gate runs after all of them.
add_library(plugin_base INTERFACE)
target_link_libraries(plugin_base INTERFACE Qt6::Core)
add_library(plugin::helper ALIAS plugin_base)

add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core plugin::helper)
# The shape qt_add_executable gives static Qt plugin initialisers.
target_link_libraries(plugin_a INTERFACE $<$<BOOL:1>:Qt6::Core>)

# Plain LINK_LIBRARIES, not only the interface ones.
add_library(plugin_static STATIC plugin_static.c)
target_link_libraries(plugin_static PRIVATE Qt6::Core plugin_base)

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
