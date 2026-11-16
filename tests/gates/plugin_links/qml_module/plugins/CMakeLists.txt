# The plugins of fixture qml_module; the gate runs after all of them.
# Literal shapes of what qt_add_qml_module(... STATIC) generates (read from a
# real Qt 6.8 project): TARGET_OBJECTS of its own generated targets behind a
# condition, and property reads of Qt and of its own targets.
set(_order_cond "$<AND:$<NOT:$<STREQUAL:$<TARGET_PROPERTY:TYPE>,STATIC_LIBRARY>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:Qt6::Platform,_qt_link_order_matters>>>,$<NOT:$<BOOL:$<TARGET_PROPERTY:_qt_object_libraries_finalizer_mode>>>,$<BOOL:TRUE>>")

add_library(mod INTERFACE)
add_library(mod_resources_1 INTERFACE)
add_library(modplugin INTERFACE)
add_library(modplugin_init INTERFACE)

target_link_libraries(mod INTERFACE Qt6::Core
    "$<${_order_cond}:$<TARGET_OBJECTS:$<TARGET_NAME:mod_resources_1>>>")
target_include_directories(mod INTERFACE
    "$<TARGET_PROPERTY:Qt6::Qml,INTERFACE_INCLUDE_DIRECTORIES>")
target_include_directories(mod_resources_1 INTERFACE
    "$<TARGET_PROPERTY:Qt6::Core,INTERFACE_INCLUDE_DIRECTORIES>"
    "$<TARGET_PROPERTY:mod,INCLUDE_DIRECTORIES>")
target_link_libraries(modplugin INTERFACE mod
    "$<${_order_cond}:$<TARGET_OBJECTS:$<TARGET_NAME:modplugin_init>>>")
target_include_directories(modplugin_init INTERFACE
    "$<TARGET_PROPERTY:modplugin,INCLUDE_DIRECTORIES>")
# Other reads the gate accepts: file of a Qt target, link-only of a plugin one.
target_link_libraries(modplugin_init INTERFACE
    "$<$<CONFIG:Debug>:$<TARGET_FILE:Qt6::Core>>" "$<LINK_ONLY:mod>"
    "$<$<TARGET_EXISTS:Qt6::Qml>:Qt6::Qml>")

roost_check_plugin_links(${CMAKE_CURRENT_SOURCE_DIR})
