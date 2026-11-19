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

## Install

```sh
cmake --install build/release --prefix <dir> --component roost
```

lays the build out as the OS expects it (the build tree is not changed). On
macOS that is `<dir>/roost.app`; on Linux the `usr/` of an AppDir:
`bin/{roost,roostd}`, `lib/roost/plugins/<id>/`, `share/applications/roost.desktop`,
an icon and `share/licenses/roost/`. The licences (`LICENSE`, `NOTICE`,
`THIRD_PARTY_LICENSES`, `LGPL-3.0.txt`, `GPL-3.0.txt`) go along. Qt is not
copied: the installed binaries load it from where the build found it (run path; `-DROOST_INSTALL_RPATH_TO_QT=OFF` leaves that out, for packaging steps that deploy Qt into the bundle). roostd
finds the installed plugins next to itself, with no `ROOST_PLUGINS_PATH`.

## Run

```sh
build/dev/bin/roost [--dev]
```

When nobody listens on the socket (no socket file, or a stale one), the window
starts `roostd` itself: the `roostd` next to its own binary, detached, so it
outlives the window and keeps the warm sessions. The banner reads "Starting
roostd…" meanwhile, and the daemon's output goes to `roostd.log` in the
directory of the socket (what it prints before it holds the socket lock, to
`roostd.start.log` there). If the start fails, the banner says why. A roostd
that refuses the handshake (another version) is not started over: the window
shows the refusal. A live roostd with a full accept backlog also refuses
connections, so a second one may be started; it loses the socket lock and
exits. `roost --no-spawn` turns the start off. The start from an AppImage
(`$APPIMAGE --roostd`, handled by AppRun) arrives with the AppImage packaging
step and does not work before it.
You can still run `build/dev/bin/roostd` by hand first.

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

`roostd --log <file>` writes the stdout and stderr of roostd and its backends
to `<file>` (a new file, mode 0600) and keeps the previous file as `<file>.1`. It
opens the file only once it holds the socket lock: a second roostd that loses
the lock exits with code 1 and leaves the log alone.

The window shows the session list on the left and the terminal on the right.
The list comes from `claude agents`, so `claude` must be on `PATH` of roostd.
In the list, ↑/↓ or `j`/`k` move the cursor. Enter opens the session and moves
the keyboard to the terminal; `Ctrl+\` (the Control key, also on macOS) moves it
back to the list.

Two check modes exit by themselves, for CI and packages. `roost --smoke` exits
0 after the first frame. `roost --smoke-sessions` needs a running roostd: it
exits 0 once the window is connected, both panels (`sessions.list`,
`terminal.view`) are created with no loader problem, the session list has at
least one row and a frame was drawn after that. It exits 1 with the reason on
stderr at once on a loader problem (for example a missing QML import such as
`QtCore` or `RoostSessions`), or after 20 s if it is still not ready. It opens
no session and changes nothing beyond what the window writes when it shows
the list.

## License

Apache-2.0, see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_LICENSES`.
