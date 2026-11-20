# The commands of the window itself (design.md §5, §16): a manifest with
# `commands` and `keybindings` only. No QML, no backend, so no targets; the
# window runs the commands (Roost.commands.registerCommand), roostd only gives
# them keys. build/<preset>/roost-plugins/shell holds plugin.json.
set(ROOST_SHELL_PLUGIN_DIR ${ROOST_PLUGINS_OUTPUT_DIR}/shell)

configure_file(plugin.json ${ROOST_SHELL_PLUGIN_DIR}/plugin.json COPYONLY)

install(FILES plugin.json DESTINATION ${ROOST_INSTALL_PLUGINS_DIR}/shell
    COMPONENT roost)
