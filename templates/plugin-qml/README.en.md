[Русская версия](README.md)

# QML plugin template

A plugin with one QML panel and no backend. The window loads the panel from disk, no build step is needed.

## Making a plugin

1. Copy the `templates/plugin-qml` directory under any name: the directory name does not matter.
2. Change `id` in `plugin.json`. It is set only there: QML gets it from the window through the `pluginId` property of the panel's root. Allowed are lowercase letters, digits, `.`, `_` and `-`, starting with a letter or a digit; `keymap` is reserved.
3. Put the plugin directory into the directory where `roostd` looks for plugins: `<state directory>/plugins`. The state directory is `$ROOST_CONFIG`, otherwise `$XDG_CONFIG_HOME/roost`, otherwise `~/.config/roost`. To try a plugin without touching the state, set `ROOST_PLUGINS_PATH` to a `:`-separated list of absolute paths. Each path is a directory that holds plugin directories (each with its own `plugin.json`), not a plugin directory itself.
4. Restart `roostd`, then the window. Load problems show up in the problems bar at the bottom of the window and on the stderr of `roostd` (the terminal it runs in). The bar shows the manifest problems from `plugins.list` and the problems of the window's panel loader.
5. The window layout is fixed for now: the sessions list and the terminal. The new plugin's panel is loaded and checked but does not appear in the window: free layout comes with M6. To open the panel, do as `tests/roost/plugin_template_test.cpp` does: a window with its own `PanelSlot` whose `panelType` is the full panel id `<plugin id>.hello`.
6. Edit `qml/HelloPanel.qml`. With `roost --dev` the window reloads the plugin when a file changes; a change to `plugin.json` needs a restart of `roostd`.

The panel's root is an `Item`; the optional `pluginId` property receives the plugin id. The panel's inputs and outputs are declared in `inputs` and `outputs` of the manifest and must be properties of the root. The API is the `Roost` singleton from `import Roost.Api`.

## Adding a backend

1. Put a program into the plugin's `backend/` and make it executable: `chmod +x backend/run`. Any language; the protocol is JSON-RPC 2.0 over stdio, one message per line.
2. In `plugin.json`, next to `contributes`, add:
   `"backend": { "command": ["backend/run"] }`
   The path is inside the plugin directory; the backend runs in it.
3. Declare the commands in `contributes`:
   `"commands": [{ "id": "<id>.doThing", "title": "Do the thing" }]`
4. The backend answers the `commands.run` request with params `{command, args}`: it puts the answer into `result` and rejects an unknown command with error `-32602`. A closed stdin means "exit". While the backend is not running, or if it exited without answering, `commands.run` returns error `-32003`, and the QML callback receives it.
5. From QML, call the command with `Roost.commands.run("<id>.doThing", args, function (error, result) { ... })`.

Details: `docs/design/05-plugins.md` and `docs/design/04-protocol.md` (in Russian).
