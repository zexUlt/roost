# Agent icons

Pixel-art birds (a roost is a perch) shown at the left of a session row in the
sessions list: an owl for `claude`, a crow for `codex`, a sparrow
(`default.svg`) for any other agent. They are original, drawn for roost on a
16x16 grid, and fall under the license of this repository.

The file name is `<agent id>.svg`. An id may hold only `A-Z a-z 0-9 . _ -`;
any other id gets `default.svg`.

To change an icon, put `<agent id>.svg` or `<agent id>.png` into
`agent-icons/` of the config directory (`~/.config/roost/agent-icons/`, or
`$ROOST_CONFIG/agent-icons/`). A file there wins over the bundled icon, `.svg`
before `.png`. The folder is read when the sessions panel is created, so reload
the panel or restart the window to see a change.
