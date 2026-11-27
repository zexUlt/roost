# roost

[![CI](https://github.com/zexUlt/roost/actions/workflows/ci.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/roost/actions/workflows/ci.yml)
[![Package](https://github.com/zexUlt/roost/actions/workflows/package.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/roost/actions/workflows/package.yml)
[![License: Apache-2.0](https://img.shields.io/github/license/zexUlt/roost)](LICENSE)
[![Release](https://img.shields.io/github/v/release/zexUlt/roost)](https://github.com/zexUlt/roost/releases)

A desktop app for macOS and Linux that finds your background coding-agent
sessions (Claude Code, Codex), groups them and shows each agent's own TUI in a
terminal panel, driven from the keyboard. Qt Quick and C++, libghostty-vt as the
terminal emulator. Every feature is a plugin, and panels can be linked.

Status: the window lists Claude Code background sessions and saved Codex
threads and opens them in a terminal panel; panels split, tab and link; keys
come from `keymap.json`; there are a command palette and a settings window.
A session that needs you or fails shows a desktop notification and can run
your own commands; "finished" is opt-in (Settings → Notifications). Design
and plan: [docs/design.md](docs/design.md).

## Install

Download a package from the [releases page](https://github.com/zexUlt/roost/releases).

macOS: `roost-<version>-macos-arm64.dmg`, Apple silicon only. Drag `roost.app`
to Applications. The app is signed ad hoc, so Gatekeeper refuses the first
start; remove the quarantine attribute once:

```sh
xattr -dr com.apple.quarantine /Applications/roost.app
```

Linux: `roost-x86_64.AppImage` or `roost-aarch64.AppImage`. Needs glibc 2.39+
and the libstdc++ of GCC 14, X11 or XWayland, and `fuse3` (`sudo apt install fuse3`).

```sh
chmod +x roost-x86_64.AppImage && ./roost-x86_64.AppImage
```

## At run time

- `claude` (and `codex`, or its full path in `ROOST_CODEX_EXECUTABLE`) must be on
  the `PATH` that `roostd` starts with: a window started from Finder or a desktop
  menu passes the environment of your login shell, a window started from a
  terminal passes that terminal's. A running `roostd` keeps its `PATH`.
- The window starts `roostd` when nobody listens on its socket. `roostd` outlives
  the window: Cmd+Q leaves the daemon and the open sessions running. `pkill roostd`
  stops it.

## First steps

- Session list: `↑`/`↓` or `j`/`k` move, Enter opens the session in the terminal,
  `Ctrl+\` (Control, also on macOS) goes back to the list.
- Command palette: `Ctrl+Shift+P` (`Cmd+Shift+P` on macOS).
- Right-click a panel: split, tabs, close, "Edit links…". The layout is saved.
- Settings: `Cmd+,` on macOS, the "Settings…" button on Linux.
- Report a bug: Help menu on macOS, toolbar button on Linux. It writes a zip to
  `~/.roost/bugreports`; nothing is sent.

## Configuration

State and settings live in `~/.config/roost`; `ROOST_CONFIG` or `XDG_CONFIG_HOME` change it.

| File | What | Details |
|---|---|---|
| `settings.json` | theme, fonts, plugin settings (edited in the settings window) | `docs/design/05-plugins.md` |
| `keymap.json` | your keys, win over the plugins' | `docs/design/16-keymap.md` |
| `agent-icons/` | `<agent>.svg` or `.png` replaces an agent's icon, `default.*` the fallback | `docs/design/08-state.md` |
| `plugins/` | your plugins; template: `templates/plugin-qml/` | `docs/design/05-plugins.md` |

The window checks GitHub for a newer release at start and then every 24 hours, and shows a line with a link. On macOS (an installed `roost.app`) and Linux (the AppImage) the line has "Install and restart": it downloads the release, verifies its signature, replaces the app and starts the new version, only when you click. Whether the folder of the app is writable is checked at the install, after the download; if it is not, nothing is changed and the message names the path and what to do by hand. `"updates": {"check": false}` in `settings.json` (or the "Check for updates" switch in the settings window) turns the check off.

`keymap.json`: `mac` replaces `key` on macOS, `"command": null` removes a key.
A saved file is picked up without a restart.

```json
[
  { "key": "Ctrl+Shift+N", "mac": "Cmd+N", "command": "claude.newSession" },
  { "mac": "Cmd+W", "command": null }
]
```

To replace a built-in plugin, list its id in `{"plugins": {"disabled": ["sessions"]}}`
of `settings.json`, put yours under another id in `plugins/` and restart `roostd`.
To undo it, remove the id from that list and restart `roostd`.

## Logs

In the directory of the socket: `roostd.log` (previous run `roostd.log.1`),
`roostd.start.log` and `roost.log` (`roost.log.1`). The directory is, first that applies:

| Where | Directory |
|---|---|
| `ROOST_SOCKET` is set | the directory of that path |
| `XDG_RUNTIME_DIR` is absolute | `$XDG_RUNTIME_DIR/roost/` |
| otherwise | `$TMPDIR/roost-<uid>/` (`/tmp/roost-<uid>/` without `TMPDIR`) |

## Build from source

Needs Clang 17+ (Apple clang on macOS), CMake 3.25+, Ninja, Qt 6.8+ and Zig 0.17+.
On macOS: `brew install cmake ninja qt zig`.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Binaries land in `build/dev/bin/`; run `build/dev/bin/roost`, with `--dev` it
reloads a plugin when its files change. Other presets: `asan-ubsan`, `tsan`, `release`,
`tsan-qt`; see `docs/design/11-build.md`.

## License

Apache-2.0, see `LICENSE`. Third-party components are listed in `THIRD_PARTY_LICENSES`.
