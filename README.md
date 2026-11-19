# roost

A desktop app for macOS and Linux that finds every background coding-agent
session, groups them, and shows each agent's own TUI in a terminal panel, all
driven from the keyboard. Claude Code first; Codex and other agents through the
same adapter interface. Built on Qt Quick and C++, with libghostty-vt as the
terminal emulator. Every feature is a plugin, and panels can be linked to each
other.

Status: skeleton. Both binaries build and start, nothing more yet; see
`docs/design.md` for the design and the plan.

## Build

Needs Clang 17+ (Apple clang on macOS), CMake 3.25+, Ninja, Qt 6.8+ and
Zig 0.17+. On macOS: `brew install cmake ninja qt zig`.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Binaries land in `build/dev/bin/`. Other presets: `asan-ubsan`, `tsan`, `release`, `tsan-qt`.
`tsan-qt` builds against a Qt built with TSan (`tools/build_qt_tsan.sh <version> <prefix>`) and needs
`ROOST_TSAN_QT_PREFIX=<prefix>`; it runs nightly in `.github/workflows/nightly-tsan.yml`.

## Run

Start the daemon, then the window:

```sh
build/dev/bin/roostd
build/dev/bin/roost [--dev]
```

roostd finds the built-in plugins next to itself (`build/dev/roost-plugins` for
a build tree) and then looks in `<ROOST_CONFIG or ~/.config/roost>/plugins`. A
plugin there with the id of a built-in one is not loaded; `plugins.list` tells
why. `ROOST_PLUGINS_PATH` (absolute paths, separated by `:`; unset, empty or
without an absolute path it is ignored) overrides the
built-in directory: roostd searches the listed directories instead, so list the
built-in one too to keep its plugins. With `--dev` the
window reloads a plugin when its files change. It watches the copy under
`build/dev/roost-plugins`, so after editing `plugins/<id>/qml/*.qml` run
`cmake --build --preset dev`. Editing a `plugin.json` needs a roostd restart:
roostd reads manifests at start.

The window shows the session list on the left and the terminal on the right.
The list comes from `claude agents`, so `claude` must be on `PATH` of roostd.
In the list, ↑/↓ or `j`/`k` move the cursor. Enter opens the session and moves
the keyboard to the terminal; `Ctrl+\` (the Control key, also on macOS) moves it
back to the list.

## License

Apache-2.0, see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_LICENSES`.
