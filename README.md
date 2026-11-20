# roost

[![CI](https://github.com/zexUlt/roost/actions/workflows/ci.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/roost/actions/workflows/ci.yml)
[![Package](https://github.com/zexUlt/roost/actions/workflows/package.yml/badge.svg?branch=trunk)](https://github.com/zexUlt/roost/actions/workflows/package.yml)
[![License: Apache-2.0](https://img.shields.io/github/license/zexUlt/roost)](LICENSE)
[![Release](https://img.shields.io/github/v/release/zexUlt/roost)](https://github.com/zexUlt/roost/releases)

A desktop app for macOS and Linux that finds every background coding-agent
session, groups them, and shows each agent's own TUI in a terminal panel, all
driven from the keyboard. Claude Code first; Codex and other agents through the
same adapter interface. Built on Qt Quick and C++, with libghostty-vt as the
terminal emulator. Every feature is a plugin, and panels can be linked to each
other.

Status: the window lists your Claude Code sessions and opens each one in a
terminal panel, and ships as a macOS `.dmg` and a Linux AppImage. The panel
layout, the keymap file and notifications are not there yet; see
`docs/design.md` for the design and the plan.

## Install

Download a package from the
[releases page](https://github.com/zexUlt/roost/releases).

### macOS

`roost-<version>-macos-arm64.dmg`, Apple silicon only. Open it and drag
`roost.app` to Applications.

The app is signed ad hoc, not with a Developer ID, so Gatekeeper refuses it
on the first start: the dialog says Apple could not verify that the app is
free of malware. Remove the quarantine attribute once, then start it from
Finder:

```sh
xattr -dr com.apple.quarantine /Applications/roost.app
```

### Linux

`roost-x86_64.AppImage` or `roost-aarch64.AppImage`, built on Ubuntu 24.04:
it needs glibc 2.39 or newer and the libstdc++ of GCC 14. The window runs on
X11 or XWayland.

```sh
chmod +x roost-x86_64.AppImage
./roost-x86_64.AppImage
```

The AppImage mounts itself through FUSE 3: install the `fuse3` package
(`sudo apt install fuse3`). It does not need `libfuse2`.

### After the start

The window starts `roostd` itself when nobody listens on the socket;
`roost --no-spawn` (on Linux `./roost-x86_64.AppImage --no-spawn`) turns that
off. `roostd` outlives the window: quitting the window (Cmd+Q on macOS) leaves
the daemon and the open sessions running, and the next window finds them
again. To stop the daemon, `pkill roostd`. Sessions come from `claude agents`,
run by `roostd`, so `claude` must be on the `PATH` it starts with: a window
started from Finder or a desktop menu gives it the `PATH` of your login shell,
a window started from a terminal gives it that terminal's `PATH`. A `roostd`
that already runs keeps the `PATH` it was started with.

Logs are in the directory of the socket: `roostd.log` (the previous run is
`roostd.log.1`) and `roostd.start.log` (what `roostd` prints before it holds
the socket). The window writes its own log next to them: `roost.log`, one line
per message (`2026-11-20T14:05:01.123 pid=12345 W roost.terminal: text`: time,
process, level D/I/W/C/F, category, text), starting with a line about the
versions, the platform and the graphics API. A newline, carriage return or
backslash in a message is written as `\n`, `\r` or `\\`, so a record is one
line. Every Qt warning goes into the file as it is, including messages from
plugin QML. A window that starts with the file over 1 MiB moves it to
`roost.log.1`; a window that is already running keeps writing to the file it
opened, which may by then be `roost.log.1`. Messages still go to stderr as
before. The `roost.terminal` lines (session and status changes) carry session
ids, agent names and statuses, never terminal output or input; they are info,
for more, `QT_LOGGING_RULES="roost.terminal.debug=true"`. The directory is the first of
these that applies:

| Where | Directory |
|---|---|
| `ROOST_SOCKET` is set | the directory of that path |
| `XDG_RUNTIME_DIR` is an absolute path (Linux desktops: `/run/user/<uid>`) | `$XDG_RUNTIME_DIR/roost/` |
| otherwise (macOS, or Linux without it) | `$TMPDIR/roost-<uid>/`; on macOS a `/var/folders/…/T/roost-<uid>/` path, `echo $TMPDIR` shows it. No `TMPDIR`: `/tmp/roost-<uid>/` |

State and settings are in `~/.config/roost` (`ROOST_CONFIG` or
`XDG_CONFIG_HOME` change it).

## Build from source

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

### Install the build

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
shows the refusal with a "Restart roostd" button next to Retry. The click sends
SIGTERM to the process that refused the window, if `<socket>.lock` names it as
a roostd, waits up to 5 s for it to leave and starts the new one; warm
terminals close, Claude sessions keep running. Nothing is stopped without the
click. A live roostd with a full accept backlog also refuses connections, so a
second one may be started; it loses the socket lock and exits. `roost
--no-spawn` turns the start off. Inside an AppImage the window starts the
daemon as `$APPIMAGE --roostd` (AppRun runs `usr/bin/roostd` for that
argument), so the daemon keeps a mount of the image of its own while it lives.
A window started from the desktop (no `TERM` in its environment, or stdin is not
a tty) has the bare desktop `PATH`, so before the first start it reads the
environment of your login shell once (`$SHELL -l -i -c 'env -0'` between two
markers, 10 s timeout, in the background; the shell's process group is killed on the timeout and as soon as the environment is read, a shell that exited by itself is not chased) and starts roostd with it, `claude`
on `PATH` included. If that fails, roostd gets the window's own environment and
the window logs a warning. From a terminal the window's environment is used as
it is. `APPDIR`, `APPIMAGE`, `ARGV0` and `OWD` are never passed on, and inside
an AppImage or an extracted AppDir run through AppRun (`APPDIR` set, the window
inside it) neither are `LD_LIBRARY_PATH`, `QT_PLUGIN_PATH` and
`QML2_IMPORT_PATH`: AppRun's values, with anything a user appended, are
dropped. Outside an AppImage a user's own values stay.
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

### Reporting a bug

Click "Report a Bug…" in the toolbar, press `Cmd+Shift+B` (macOS only; on
Linux `Ctrl+Shift+B` goes to the terminal program) or choose Help → "Report a
Bug…" (macOS). Describe the problem and press
Create. roost writes `roost-bug-<YYYYMMDD-HHMMSS>.zip` to your Downloads folder
(your home directory if there is none) and never overwrites an earlier zip.
Nothing is sent: "Show in Finder" (Linux: "Show in Folder") reveals the zip, and
"Open GitHub Issue" opens a new issue in the browser with your text in it, so
attach the zip there.

The zip holds `about.txt` (versions of roost and Qt, OS, kernel, CPU, graphics
API, the socket path), `description.txt` and, if they exist, the logs next to the
socket: `roostd.log`, `roostd.log.1`, `roostd.start.log`, `roost.log`,
`roost.log.1`. Of each log only the last 8 MiB go in, after the line
`[roost: first <N> bytes cut]`. Logs may contain paths and session names: read them before you
send. The settings (the files of `~/.config/roost` or `$XDG_CONFIG_HOME/roost`
that roostd's store writes, `*.json`) go in only if you tick "Include settings";
they are off by default.

## License

Apache-2.0, see `LICENSE`. Third-party components and their licences are listed
in `THIRD_PARTY_LICENSES`.
