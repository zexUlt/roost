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
        calling_context.cpp
        calling_context.hpp
        commands_adapter.cpp
        commands_adapter.hpp
        js_callback.cpp
        js_callback.hpp
        roost_singleton.cpp
        roost_singleton.hpp
        sessions_model.cpp
        sessions_model.hpp
        store_adapter.cpp
        store_adapter.hpp
        terminal_item.cpp
        terminal_item.hpp
        theme.cpp
        theme.hpp
        update_download.cpp
        update_download.hpp
        updates.cpp
        updates.hpp
    NO_CACHEGEN
)

target_link_libraries(roost_qml_api PUBLIC roost_api roost_terminal Qt6::Quick)
# calling_context.cpp only: the private API of QtQml (see the file).
target_link_libraries(roost_qml_api PRIVATE Qt6::QmlPrivate)

# The public key that checks the signature of a downloaded release is the file
# packaging/minisign.pub: its key line is compiled in (one source of truth with
# the release workflow). Another key, another build.
set(_roost_minisign_pub ${PROJECT_SOURCE_DIR}/packaging/minisign.pub)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_roost_minisign_pub})
file(STRINGS ${_roost_minisign_pub} _roost_minisign_lines)
set(_roost_update_key "")
foreach(_line IN LISTS _roost_minisign_lines)
    if(NOT _line MATCHES "^untrusted comment:" AND NOT _line STREQUAL "")
        set(_roost_update_key "${_line}")
    endif()
endforeach()
if(NOT _roost_update_key MATCHES "^RW[A-Za-z0-9+/]+=*$")
    message(FATAL_ERROR "${_roost_minisign_pub} has no minisign key line")
endif()
set_source_files_properties(updates.cpp PROPERTIES
    COMPILE_DEFINITIONS "ROOST_UPDATE_PUBLIC_KEY=\"${_roost_update_key}\"")
