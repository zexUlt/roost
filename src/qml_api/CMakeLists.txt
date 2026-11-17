# The C++ that QML sees (ADR 011). A thin layer: the logic lives in
# roost_terminal and below.
#
# NO_CACHEGEN: see src/roost/CMakeLists.txt.
qt_add_library(roost_qml_api STATIC)

qt_add_qml_module(roost_qml_api
    URI Roost.Api
    VERSION 1.0
    SOURCES
        bus_adapter.cpp
        bus_adapter.hpp
        commands_adapter.cpp
        commands_adapter.hpp
        js_callback.cpp
        js_callback.hpp
        roost_singleton.cpp
        roost_singleton.hpp
        store_adapter.cpp
        store_adapter.hpp
        terminal_item.cpp
        terminal_item.hpp
    NO_CACHEGEN
)

target_link_libraries(roost_qml_api PUBLIC roost_api roost_terminal Qt6::Quick)
