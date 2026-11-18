# The sessions list (design.md §6): build/<preset>/roost-plugins/sessions holds
# plugin.json and qml/SessionsPanel.qml. The only target is the library of the
# list model, which no plugin file loads yet.
set(ROOST_SESSIONS_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/sessions)

configure_file(plugin.json ${ROOST_SESSIONS_PLUGIN_DIR}/plugin.json COPYONLY)
configure_file(qml/SessionsPanel.qml
    ${ROOST_SESSIONS_PLUGIN_DIR}/qml/SessionsPanel.qml COPYONLY)

# The rows of the list in plain C++ over Qt Core, so that tests drive the real
# thing. Position independent: the QML module of a later step links it.
add_library(sessions_list_lib STATIC
    model/session_rows.cpp
)
target_include_directories(sessions_list_lib PUBLIC ${PROJECT_SOURCE_DIR}/plugins)
target_link_libraries(sessions_list_lib PUBLIC Qt6::Core)
set_target_properties(sessions_list_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
