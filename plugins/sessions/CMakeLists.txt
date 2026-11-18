# The sessions list (design.md §6): build/<preset>/roost-plugins/sessions holds
# plugin.json and qml/SessionsPanel.qml. No backend, so no targets.
set(ROOST_SESSIONS_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/sessions)

configure_file(plugin.json ${ROOST_SESSIONS_PLUGIN_DIR}/plugin.json COPYONLY)
configure_file(qml/SessionsPanel.qml
    ${ROOST_SESSIONS_PLUGIN_DIR}/qml/SessionsPanel.qml COPYONLY)
