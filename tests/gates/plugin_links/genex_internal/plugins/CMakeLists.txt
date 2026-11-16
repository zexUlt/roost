# The plugins of fixture genex_internal: the qt_add_qml_module shape, but the
# object files are those of roost_core.
add_library(plugin_a INTERFACE)
target_link_libraries(plugin_a INTERFACE Qt6::Core
    "$<$<AND:$<NOT:$<STREQUAL:$<TARGET_PROPERTY:TYPE>,STATIC_LIBRARY>>,$<BOOL:TRUE>>:$<TARGET_OBJECTS:roost_core>>")

# And the property read of roost_core's include directories.
add_library(plugin_b INTERFACE)
target_link_libraries(plugin_b INTERFACE Qt6::Core)
target_include_directories(plugin_b INTERFACE
    "$<TARGET_PROPERTY:roost_core,INTERFACE_INCLUDE_DIRECTORIES>")

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
