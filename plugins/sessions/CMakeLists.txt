# The sessions list (design.md §6): build/<preset>/roost-plugins/sessions holds
# plugin.json, qml/SessionsPanel.qml and the QML module RoostSessions in
# qml/RoostSessions/ (qmldir, the library, the types), which the manifest key
# native.qmlImportPath makes the window put on the import path.
set(ROOST_SESSIONS_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/sessions)

configure_file(plugin.json ${ROOST_SESSIONS_PLUGIN_DIR}/plugin.json COPYONLY)
configure_file(qml/SessionsPanel.qml
    ${ROOST_SESSIONS_PLUGIN_DIR}/qml/SessionsPanel.qml COPYONLY)

# The model of the list over Qt Core, so that tests drive the real thing
# without a QML engine: the rows, the store document and the QML-facing
# SessionsList. Position independent: the QML module links it.
add_library(sessions_list_lib STATIC
    model/session_rows.cpp
    model/list_state.cpp
    model/row_diff.cpp
    qml_types/sessions_list.cpp
)
target_include_directories(sessions_list_lib PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(sessions_list_lib PUBLIC Qt6::Core)
set_target_properties(sessions_list_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
# The module registers SessionsList through QML_FOREIGN, so qmltyperegistrar
# needs the meta types of the library.
qt_extract_metatypes(sessions_list_lib)

# The QML module RoostSessions: a shared plugin the engine loads from the
# plugin's own directory. NO_CACHEGEN, as the modules of roost (src/roost).
qt_add_plugin(sessions_qml CLASS_NAME RoostSessionsPlugin)
qt_add_qml_module(sessions_qml
    PLUGIN_TARGET sessions_qml
    URI RoostSessions
    VERSION 1.0
    OUTPUT_DIRECTORY ${ROOST_SESSIONS_PLUGIN_DIR}/qml/RoostSessions
    SOURCES
        module/sessions_list_foreign.cpp
        module/sessions_list_foreign.hpp
    NO_CACHEGEN
)
target_include_directories(sessions_qml PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/module)
target_link_libraries(sessions_qml PRIVATE sessions_list_lib Qt6::Qml)
