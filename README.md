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

Start the daemon with the built-in plugins, then the window:

```sh
ROOST_PLUGINS_PATH=$PWD/build/dev/roost-plugins build/dev/bin/roostd
build/dev/bin/roost [--dev]
```

roostd looks for plugins in `ROOST_PLUGINS_PATH` (absolute paths, separated by
`:`), then in `<ROOST_CONFIG or ~/.config/roost>/plugins`. With `--dev` the
window reloads a plugin when its files change. It watches the copy under
`build/dev/roost-plugins`, so after editing `plugins/<id>/qml/*.qml` run
`cmake --build --preset dev`. Editing a `plugin.json` needs a roostd restart:
roostd reads manifests at start.

The window shows the session list on the left and the terminal on the right.
The list comes from `claude agents`, so `claude` must be on `PATH` of roostd.
In the list, ↑/↓ or `j`/`k` move the cursor and Enter opens the session in the
terminal.

## License

Apache-2.0, see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_LICENSES`.
