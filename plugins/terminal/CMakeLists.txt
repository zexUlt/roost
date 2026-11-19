# The terminal panel (design.md §6): build/<preset>/roost-plugins/terminal holds
# plugin.json and qml/TerminalPanel.qml. No backend, so no targets.
set(ROOST_TERMINAL_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/terminal)

configure_file(plugin.json ${ROOST_TERMINAL_PLUGIN_DIR}/plugin.json COPYONLY)
configure_file(qml/TerminalPanel.qml
    ${ROOST_TERMINAL_PLUGIN_DIR}/qml/TerminalPanel.qml COPYONLY)

install(FILES plugin.json DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/terminal
    COMPONENT roost)
install(FILES qml/TerminalPanel.qml
    DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/terminal/qml COMPONENT roost)
