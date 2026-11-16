# The C++ that QML sees (ADR 011). A thin layer: the logic lives in
# roost_terminal and below.
#
# NO_CACHEGEN: see src/roost/CMakeLists.txt.
qt_add_library(roost_qml_api STATIC)

qt_add_qml_module(roost_qml_api
    URI Roost.Api
    VERSION 1.0
    SOURCES
        terminal_item.cpp
        terminal_item.hpp
    NO_CACHEGEN
)

target_link_libraries(roost_qml_api PUBLIC roost_terminal Qt6::Quick)
