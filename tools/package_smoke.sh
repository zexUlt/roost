#!/usr/bin/env bash
# The package on a machine that is not the one that built it (docs/design/11-build.md):
# a .dmg on macOS, an AppImage on Linux, one script for both. It runs the
# packaged window and the daemon it starts, and checks what the user relies
# on once the window is closed:
#   - the window, with an isolated socket and config, a fake `claude` and an
#     opened session, passes `--smoke-sessions` (it lists the sessions and
#     attaches to the opened one);
#   - the Qt of every window (the first, the second, the one of `open -n`)
#     comes from the package only, and the platform plugin the window ran with
#     is the package's own;
#   - after the window exited, roostd is alive, runs from the package (the
#     .app copied out of the image, the mount of the AppImage: its own
#     mount, which the window's exit does not take away) and loaded its Qt from
#     the package only; the fake attach is alive, and still is at the end of
#     the run, after everything below;
#   - `roostd --dump` of the package answers;
#   - `claude-adapter`, killed by pid with no window around, comes back;
#   - a second window `--smoke-sessions` connects to the same roostd pid;
#   - the licence texts are in the package.
# With --launch-services (macOS) the window is also started once through
# LaunchServices, `open -n` with the environment passed by `open --env`: roostd
# must start from the app and outlive the window as it does in a direct run.
# A failure of `open --env` itself is a FAIL: say so in the job, not a skip.
#
# The environment is isolated as in tools/check_bundle.sh: a temporary HOME
# (CFFIXED_USER_HOME on macOS too, so that no preference lands in the real
# ~/Library/Preferences), socket, config and TMPDIR, a $SHELL that does not
# exist (never the login shell of whoever runs this), a fake `claude` (never
# the real one). The AppImage runs through FUSE, as a user runs it, not
# through --appimage-extract-and-run. What the script starts it kills by pid,
# and it detaches only the image it mounted, at its own mount point.
#
# The windows run with no display: QT_QPA_PLATFORM=offscreen. A real platform
# plugin, the one a user's desktop loads, is a window that opens: it is for CI,
# which sets ROOST_SMOKE_QPA=cocoa on macOS, and xcb under xvfb-run on Linux
# (DISPLAY and XAUTHORITY of the caller are passed on). A local run opens no
# window.
#
# Every failure is reported before the script ends; the exit code is 1 if
# there was one.
#
# Usage: tools/package_smoke.sh <roost.dmg | roost-<arch>.AppImage> [--launch-services]
set -uo pipefail

usage="usage: tools/package_smoke.sh <roost.dmg | roost-<arch>.AppImage> [--launch-services]
the platform of the windows: ROOST_SMOKE_QPA=offscreen (default) | cocoa | xcb"
package=${1:?$usage}
launch_services=0
case ${2:-} in
    "") ;;
    --launch-services) launch_services=1 ;;
    *)
        echo "$usage" >&2
        exit 2
        ;;
esac
qpa=${ROOST_SMOKE_QPA:-offscreen}
case $qpa in
    offscreen | cocoa | xcb) ;;
    *)
        echo "FAIL: ROOST_SMOKE_QPA=$qpa is none of offscreen, cocoa, xcb"
        exit 2
        ;;
esac
if [ ! -f "$package" ]; then
    echo "FAIL: $package is not a file"
    exit 1
fi
package=$(cd "$(dirname "$package")" && pwd -P)/$(basename "$package")
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)

case $package in
    *.dmg) kind=macos ;;
    *.AppImage) kind=linux ;;
    *)
        echo "FAIL: $package is neither a .dmg nor an AppImage"
        exit 1
        ;;
esac
if [ "$kind" = macos ] && [ "$(uname -s)" != Darwin ]; then
    echo "FAIL: a .dmg is smoked on macOS"
    exit 1
fi
if [ "$kind" = linux ] && [ "$(uname -s)" != Linux ]; then
    echo "FAIL: an AppImage is smoked on Linux"
    exit 1
fi
if [ "$launch_services" = 1 ] && [ "$kind" != macos ]; then
    echo "FAIL: --launch-services is for a .dmg"
    exit 1
fi
if [ "$qpa" = cocoa ] && [ "$kind" != macos ]; then
    echo "FAIL: cocoa is the platform of macOS"
    exit 1
fi
if [ "$qpa" = xcb ] && [ "$kind" != linux ]; then
    echo "FAIL: xcb is the platform of Linux"
    exit 1
fi
if [ "$qpa" = xcb ] && [ -z "${DISPLAY:-}" ]; then
    echo "FAIL: ROOST_SMOKE_QPA=xcb needs a display: run under xvfb-run"
    exit 1
fi

failed=0
fail() {
    echo "FAIL: $*"
    failed=1
}
ok() { echo "ok:   $*"; }

work=
mnt=
app=
roostd_pid=
window_pid=
helper_pid=
extra_roostd_pid=
extra_work=

detach_dmg() {
    if [ -n "$mnt" ]; then
        # Only the mount this script made, at its own mount point.
        if hdiutil detach -quiet "$mnt" 2>/dev/null ||
            { sleep 2 && hdiutil detach -quiet -force "$mnt" 2>/dev/null; }; then
            rmdir "$mnt" 2>/dev/null
            mnt=
        else
            echo "FAIL: could not detach $mnt" >&2
            failed=1
        fi
    fi
}

# kill_tree <pid>: the pid and the children it had, by pid (the plugin
# backends; inside an AppImage the real roost under the runtime). The parent
# goes first, so it cannot restart a child that was killed before it.
kill_tree() {
    local pid children
    [ -n "$1" ] || return 0
    children=$(pgrep -P "$1" 2>/dev/null)
    kill "$1" 2>/dev/null
    for pid in $children; do
        kill "$pid" 2>/dev/null
    done
}

# shellcheck disable=SC2329  # called from cleanup, which runs from the trap
# appimage_mounted: the FUSE mount of this AppImage is in /proc/mounts (its
# source is the file).
appimage_mounted() {
    awk -v p="$package" '$1 == p && $3 ~ /^fuse/ {found = 1} END {exit !found}' /proc/mounts
}

# shellcheck disable=SC2329  # runs from the trap
cleanup() {
    local status=$? pid dir _
    kill_tree "$window_pid"
    [ -n "$helper_pid" ] && kill "$helper_pid" 2>/dev/null
    kill_tree "$roostd_pid"
    kill_tree "$extra_roostd_pid"
    # Every fake attach the runs started: the fake claude records its pid in
    # bin/attach.all, one line per attach.
    for dir in "$work" "$extra_work"; do
        [ -n "$dir" ] && [ -s "$dir/bin/attach.all" ] || continue
        while read -r pid; do
            case $pid in
                '' | *[!0-9]*) ;;
                *) kill "$pid" 2>/dev/null ;;
            esac
        done <"$dir/bin/attach.all"
    done
    detach_dmg
    # The FUSE mount of the AppImage goes away with the last process that
    # runs from it: wait for it to go, a failure to go is a failure.
    if [ "$kind" = linux ] && [ -n "$work" ]; then
        for _ in $(seq 1 100); do
            appimage_mounted || break
            sleep 0.1
        done
        if appimage_mounted; then
            echo "FAIL: the FUSE mount of $package did not go away in 10 s" >&2
            failed=1
        fi
    fi
    for dir in "$work" "$extra_work"; do
        [ -n "$dir" ] && rm -rf "$dir"
    done
    # A failure found here counts as much as one found before: the exit status
    # of the script was settled before this trap ran.
    if [ "$status" -eq 0 ] && [ "$failed" -ne 0 ]; then
        exit 1
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# make_env <dir>: the isolated tree of one run in <dir>: the private directory
# of the socket, the home, the config with a session opened, the fake claude.
make_env() {
    local dir=$1
    mkdir -p "$dir/run" "$dir/home" "$dir/bin" "$dir/config"
    chmod 700 "$dir/run"
    cp "$root/tests/fixtures/claude_agents.json" "$dir/bin/agents.json"
    cat >"$dir/bin/claude" <<'FAKE'
#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo $$ >> "$dir/attach.all"; echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 600 ;;
*) exit 2 ;;
esac
FAKE
    chmod 755 "$dir/bin/claude"
    # The store document of the sessions plugin with one session opened, so
    # that the window attaches to it.
    printf '%s' '{"version":1,"view":{"opened":"claude/bg-0a1b2c3d"}}' \
        >"$dir/config/sessions.json"
}

# window_env <dir>: the environment of a run, one VAR=value per line.
window_env() {
    printf '%s\n' \
        "HOME=$1/home" \
        "CFFIXED_USER_HOME=$1/home" \
        "TMPDIR=$1/" \
        "PATH=$1/bin:/usr/bin:/bin" \
        "SHELL=$1/no-such-shell" \
        "ROOST_SOCKET=$1/run/roostd.sock" \
        "ROOST_CONFIG=$1/config" \
        "QT_QPA_PLATFORM=$qpa" \
        "QML_DISABLE_DISK_CACHE=1"
    if [ "$qpa" = xcb ]; then
        printf '%s\n' "DISPLAY=${DISPLAY:-}"
        [ -n "${XAUTHORITY:-}" ] && printf '%s\n' "XAUTHORITY=$XAUTHORITY"
    fi
}

# window_extra: what only a window gets, VAR=value one per line, so that the
# check of its Qt and its platform plugin has something to read. macOS: dyld
# names every library it loads. Linux: ld.so does, to a file per process
# (the roostd the first window starts inherits it, and is read as well).
# window_extra <dir> <name>
window_extra() {
    if [ "$kind" = macos ]; then
        printf '%s\n' "DYLD_PRINT_LIBRARIES=1"
    else
        printf '%s\n' "LD_DEBUG=libs" "LD_DEBUG_OUTPUT=$1/$2.ld"
    fi
}

# The same as `env -i` with window_env, for a command run directly. On Linux
# no TMPDIR: the runtime of the AppImage mounts under it, and a mount in the
# directory this script removes at the end would be in the way of the unmount.
# run_window <dir> <output file> <name of this window>
run_window() {
    local dir=$1 out=$2 name=$3 line
    local vars=()
    while IFS= read -r line; do
        case $kind:$line in
            linux:TMPDIR=*) ;;
            *) vars+=("$line") ;;
        esac
    done < <(window_env "$dir"; window_extra "$dir" "$name")
    env -i "${vars[@]}" "${window_cmd[@]}" --smoke-sessions \
        >"$out" 2>&1 </dev/null &
    window_pid=$!
}

# wait_window <what>: waits for the window started by run_window, at most two
# minutes; sets window_status (empty: it hung, and was killed).
window_status=
wait_window() {
    window_status=
    local _
    for _ in $(seq 1 600); do
        if ! kill -0 "$window_pid" 2>/dev/null; then
            wait "$window_pid"
            window_status=$?
            window_pid=
            return
        fi
        sleep 0.2
    done
    kill_tree "$window_pid"
    window_pid=
}

# note_crash <status>: a window that died of a signal (status above 128):
# which one, and on what CPU, so that a repeat of a crash seen once (SIGILL of
# the window under xcb on x86_64) can be told apart. The FAIL stays.
note_crash() {
    local sig cpu
    [ "$1" -gt 128 ] || return 0
    sig=$(kill -l $(($1 - 128)) 2>/dev/null)
    if [ "$kind" = linux ]; then
        cpu=$(awk -F': ' '/^model name/ {print $2; exit}' /proc/cpuinfo)
        cpu="$cpu, $(awk '/^flags/ {print NF - 2; exit}' /proc/cpuinfo) flags"
    else
        cpu=$(sysctl -n machdep.cpu.brand_string 2>/dev/null)
    fi
    echo "note: the window died of SIG$sig ($1); cpu: ${cpu:-unknown}"
}

# lock_pid <dir>: who holds the socket of the run in <dir>: the first line of
# the lock file is the pid.
lock_pid() {
    if [ -s "$1/run/roostd.sock.lock" ]; then
        head -n 1 "$1/run/roostd.sock.lock" | tr -dc '0-9'
    fi
}

# exe_of <pid>: the executable of a process.
exe_of() {
    if [ "$kind" = linux ]; then
        readlink "/proc/$1/exe"
    else
        ps -o command= -p "$1"
    fi
}

# backend_pid <roostd pid>: the pid of its child claude-adapter, if it has one.
backend_pid() {
    local pid
    for pid in $(pgrep -P "$1" 2>/dev/null); do
        case $(ps -o command= -p "$pid" 2>/dev/null) in
            *claude-adapter*)
                echo "$pid"
                return 0
                ;;
        esac
    done
    return 1
}

show_tail() {
    tail -n 30 "$1" 2>/dev/null | sed 's/^/    /'
}

# ---------------------------------------------------------------------------
# Unpack: the .app out of the image into a directory of its own, or the
# AppImage as it is.

work=$(mktemp -d "${TMPDIR:-/tmp}/roost-smoke.XXXXXX") || exit 1
work=$(cd "$work" && pwd -P)
make_env "$work"

if [ "$kind" = macos ]; then
    mnt=$(mktemp -d "${TMPDIR:-/tmp}/roost-dmg.XXXXXX") || exit 1
    if ! hdiutil attach -readonly -nobrowse -noautoopen -quiet \
        -mountpoint "$mnt" "$package" >/dev/null 2>&1; then
        fail "hdiutil attach of $package failed"
        rmdir "$mnt" 2>/dev/null
        mnt=
        exit 1
    fi
    if [ ! -d "$mnt/roost.app/Contents" ]; then
        fail "$package has no roost.app"
        exit 1
    fi
    # Where a user drags it: a directory of apps of its own, not the build
    # tree. ditto keeps the signature and the symlinks of the bundle.
    mkdir "$work/Applications"
    if ! ditto "$mnt/roost.app" "$work/Applications/roost.app"; then
        fail "could not copy roost.app out of $package"
        exit 1
    fi
    detach_dmg
    app=$work/Applications/roost.app
    window_cmd=("$app/Contents/MacOS/roost")
    ok "roost.app copied out of the image to $app"
    for qt in /opt/homebrew/opt/qtbase /opt/homebrew/opt/qt; do
        if [ -e "$qt" ]; then
            fail "$qt exists: this machine has a Qt, the smoke proves nothing"
        fi
    done
else
    chmod +x "$package"
    window_cmd=("$package")
    if [ ! -e /dev/fuse ]; then
        fail "no /dev/fuse: the AppImage cannot run through FUSE here"
        exit 1
    fi
fi

# ---------------------------------------------------------------------------
# The window.

# check_window_qt <label> <name of the window> <output file>...: the Qt that
# the window (and what it started) loaded is the package's, and the platform
# plugin it ran with, the one asked for, is the package's. macOS: dyld names
# every library it loads, in the output of the window. Linux: ld.so names
# them, to the files <work>/<name>.ld.<pid>; a library under a `.mount_`
# directory is one of the AppImage mount. A check that saw no QtCore of the
# package saw nothing: a failure.
check_window_qt() {
    local label=$1 name=$2 outside inside platform where
    shift 2
    if [ "$kind" = macos ]; then
        outside=$(cat "$@" | grep -E '/Qt[A-Za-z0-9]*(\.framework/| |$)|libQt' |
            grep -vF "$app/" || true)
        inside=$(cat "$@" | grep -F "$app/" | grep -cE '/QtCore(\.framework/| |$)' || true)
        where="$app/"
        platform=$(cat "$@" | grep -E "^dyld\[[0-9]+\]: .*/platforms/libq$qpa\.dylib\$" | grep -F "$app/" | head -n 1 || true)
    else
        local loaded
        loaded=$(cat "$work/$name".ld.* 2>/dev/null | sed -n 's/.*calling init: //p' || true)
        outside=$(printf '%s\n' "$loaded" | grep -E '/libQt6[A-Za-z0-9]*\.so' |
            grep -v '/\.mount_' || true)
        inside=$(printf '%s\n' "$loaded" | grep -E '/\.mount_.*/libQt6Core\.so' | grep -c . || true)
        where="the AppImage mount"
        platform=$(cat "$work/$name".ld.* 2>/dev/null | grep -E "calling init: .*/\.mount_.*/platforms/libq$qpa\.so\$" | head -n 1 || true)
    fi
    if [ -n "$outside" ]; then
        fail "$label loaded Qt from outside the package:"
        printf '%s\n' "$outside" | head -n 10 | sed 's/^/    /'
    elif [ "$inside" -eq 0 ]; then
        fail "$label: no QtCore of $where in the list of loaded libraries: the check saw nothing"
    else
        ok "$label loaded QtCore from the package and no Qt from outside it"
    fi
    if [ -n "$platform" ]; then
        ok "$label ran on the platform plugin $qpa of the package"
        echo "      $platform"
    else
        fail "$label: no platform plugin $qpa of the package loaded"
        grep -hiE 'qpa|xcb|cocoa|display|could not' "$@" 2>/dev/null | head -n 10 | sed 's/^/    /'
    fi
}

run_window "$work" "$work/window.out" win1
wait_window
if [ -z "$window_status" ]; then
    fail "roost --smoke-sessions did not end in 2 minutes"
    show_tail "$work/window.out"
elif [ "$window_status" -ne 0 ]; then
    fail "roost --smoke-sessions exited with $window_status"
    note_crash "$window_status"
    show_tail "$work/window.out"
else
    ok "roost --smoke-sessions"
fi
check_window_qt "the window" win1 "$work/window.out"

# ---------------------------------------------------------------------------
# After the window: roostd and the attach.

roostd_pid=$(lock_pid "$work")
if [ -z "$roostd_pid" ] || ! kill -0 "$roostd_pid" 2>/dev/null; then
    fail "the window left no roostd running (lock: $work/run/roostd.sock.lock)"
    show_tail "$work/run/roostd.log"
    exit 1
fi

exe=$(exe_of "$roostd_pid")
package_root=
if [ "$kind" = macos ]; then
    case $exe in
        "$app/Contents/MacOS/roostd"*)
            ok "roostd $roostd_pid is $app/Contents/MacOS/roostd"
            package_root=$app
            ;;
        *) fail "the roostd that runs is not the one of the copied app: $exe" ;;
    esac
    loaded=$(lsof -Fn -p "$roostd_pid" 2>/dev/null | sed -n 's/^n//p')
    qt=$(printf '%s\n' "$loaded" | grep -iE '/Qt[A-Za-z]*(\.framework/|$)|libQt' || true)
    outside=$(printf '%s\n' "$qt" | grep -vF "$app/" | grep -v '^$' || true)
    if [ -n "$outside" ]; then
        fail "roostd loaded Qt from outside the bundle:"
        printf '%s\n' "$outside" | sed 's/^/    /'
    elif printf '%s\n' "$qt" | grep -F "$app/" | grep -qE '/QtCore(\.framework/|$)'; then
        ok "roostd loaded QtCore from the bundle and no Qt from outside it"
    else
        fail "roostd has no QtCore from $app/ loaded (lsof saw $(printf '%s\n' "$loaded" | grep -c .) files)"
    fi
else
    # The AppImage's own mount: the runtime mounted the image with FUSE (a
    # mount of type fuse* holds the directory of roostd) and the roostd was
    # started as `$APPIMAGE --roostd`, so it has an environment with APPIMAGE.
    mount_point=
    if [[ $exe == */usr/bin/roostd ]]; then
        mount_point=${exe%/usr/bin/roostd}
    fi
    if [ -n "$mount_point" ] && awk -v m="$mount_point" '$3 ~ /^fuse/ && $2 == m {found = 1} END {exit !found}' /proc/mounts; then
        ok "roostd $roostd_pid runs from the FUSE mount $mount_point"
        package_root=$mount_point/usr
    else
        fail "roostd $roostd_pid does not run from a FUSE mount of the AppImage (exe: $exe)"
    fi
    if ! tr '\0' '\n' <"/proc/$roostd_pid/environ" | grep -qxF "APPIMAGE=$package"; then
        fail "roostd $roostd_pid has no APPIMAGE=$package in its environment: it was not started as \$APPIMAGE --roostd"
    fi
    qt=$(grep -oE '/[^ ]*libQt6[A-Za-z0-9]*\.so[^ ]*' "/proc/$roostd_pid/maps" | sort -u || true)
    outside=$(printf '%s\n' "$qt" | grep -v '^$' | grep -vF "$mount_point/" || true)
    if [ -n "$outside" ]; then
        fail "roostd mapped Qt from outside the AppImage:"
        printf '%s\n' "$outside" | sed 's/^/    /'
    elif printf '%s\n' "$qt" | grep -qF "$mount_point/usr/lib/libQt6Core.so"; then
        ok "roostd mapped QtCore from the AppImage and no Qt from outside it"
    else
        fail "roostd has no QtCore of the AppImage mapped"
    fi
fi

# The window waits for the attach of the session it opens, and fails when it
# cannot be opened: the first window, once it exited with 0, has left the
# attach of the fake claude behind. None is a FAIL, not a reason for another
# window. What is proved is that the attach of a packaged backend outlives the
# window that asked for it.
attach=
[ -s "$work/bin/attach.pid" ] && attach=$(head -n 1 "$work/bin/attach.pid")
if [ -z "$attach" ]; then
    fail "the window exited ($window_status) and the fake claude was never attached: the packaged backend did not run it"
    show_tail "$work/run/roostd.log"
elif kill -0 "$attach" 2>/dev/null; then
    ok "the attach $attach is alive after the window exited"
else
    fail "the attach $attach is gone after the window exited"
fi

# ---------------------------------------------------------------------------
# The licences.

if [ "$kind" = macos ]; then
    licenses=$app/Contents/Resources/licenses
    names="LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt"
else
    licenses=$package_root/share/licenses/roost
    names="LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt ICU-LICENSE.txt"
fi
for name in $names; do
    if [ -s "$licenses/$name" ]; then
        ok "licenses/$name"
    else
        fail "$licenses/$name is missing or empty"
    fi
done

# ---------------------------------------------------------------------------
# roostd --dump of the package answers: the answer to the greeting is the
# first frame. It never ends by itself, so it is read for a while and killed.

if [ "$kind" = macos ]; then
    dump_cmd=("$app/Contents/MacOS/roostd")
else
    dump_cmd=("$package" --roostd)
fi
vars=()
while IFS= read -r line; do
    case $kind:$line in
        linux:TMPDIR=*) ;;
        *) vars+=("$line") ;;
    esac
done < <(window_env "$work")
env -i "${vars[@]}" "${dump_cmd[@]}" --dump >"$work/dump.out" 2>"$work/dump.err" </dev/null &
helper_pid=$!
for _ in $(seq 1 100); do
    if grep -q '^rpc ' "$work/dump.out" 2>/dev/null || ! kill -0 "$helper_pid" 2>/dev/null; then
        break
    fi
    sleep 0.1
done
if grep -q '^rpc ' "$work/dump.out" 2>/dev/null; then
    ok "roostd --dump answers"
else
    fail "roostd --dump printed no answer"
    show_tail "$work/dump.out"
    show_tail "$work/dump.err"
fi
kill "$helper_pid" 2>/dev/null
wait "$helper_pid" 2>/dev/null
helper_pid=

# ---------------------------------------------------------------------------
# claude-adapter, killed with no window around, comes back. On Linux that the
# mount is still there is what lets roostd start it again.

old_backend=$(backend_pid "$roostd_pid")
if [ -z "$old_backend" ]; then
    fail "roostd $roostd_pid has no claude-adapter child"
else
    kill -KILL "$old_backend"
    new_backend=
    for _ in $(seq 1 200); do
        new_backend=$(backend_pid "$roostd_pid") || new_backend=
        if [ -n "$new_backend" ] && [ "$new_backend" != "$old_backend" ]; then
            break
        fi
        new_backend=
        sleep 0.1
    done
    if [ -n "$new_backend" ]; then
        ok "claude-adapter $old_backend was killed, roostd started $new_backend"
    else
        fail "roostd did not restart the claude-adapter killed by pid $old_backend within 20 s"
        show_tail "$work/run/roostd.log"
    fi
fi
if ! kill -0 "$roostd_pid" 2>/dev/null; then
    fail "roostd $roostd_pid died with the adapter"
fi

# ---------------------------------------------------------------------------
# A second window finds the same roostd.

run_window "$work" "$work/window2.out" win2
wait_window
if [ -z "$window_status" ]; then
    fail "the second roost --smoke-sessions did not end in 2 minutes"
    show_tail "$work/window2.out"
elif [ "$window_status" -ne 0 ]; then
    fail "the second roost --smoke-sessions exited with $window_status"
    note_crash "$window_status"
    show_tail "$work/window2.out"
else
    second=$(lock_pid "$work")
    if [ "$second" = "$roostd_pid" ] && kill -0 "$roostd_pid" 2>/dev/null; then
        ok "the second window connected to the same roostd $roostd_pid"
    else
        fail "the second window ran against roostd '$second', not $roostd_pid"
    fi
fi
check_window_qt "the second window" win2 "$work/window2.out"

# The attach of the fake claude, alive at the end of the run, after the
# restart of the adapter and the second window: the check right after it
# started proves little (the fake lives for a moment in any case). A moment
# for what would end it.
if [ -n "$attach" ]; then
    sleep 3
    if kill -0 "$attach" 2>/dev/null; then
        ok "the attach $attach is still alive at the end of the run"
    else
        fail "the attach $attach is gone at the end of the run"
        show_tail "$work/run/roostd.log"
    fi
fi

# ---------------------------------------------------------------------------
# LaunchServices: `open -n` of the app, in a run of its own.

if [ "$launch_services" = 1 ]; then
    extra_work=$(mktemp -d "${TMPDIR:-/tmp}/roost-open.XXXXXX") || exit 1
    extra_work=$(cd "$extra_work" && pwd -P)
    make_env "$extra_work"
    : >"$extra_work/open.out"
    : >"$extra_work/open.err"
    open_args=(-n -W --stdout "$extra_work/open.out" --stderr "$extra_work/open.err")
    while IFS= read -r line; do
        open_args+=(--env "$line")
    done < <(window_env "$extra_work"; window_extra "$extra_work" open)
    # One `open -n` runs one window, which waits for the attach as any window.
    open "${open_args[@]}" "$app" --args --smoke-sessions >"$extra_work/open.log" 2>&1 </dev/null &
    helper_pid=$!
    opened=
    for _ in $(seq 1 600); do
        if ! kill -0 "$helper_pid" 2>/dev/null; then
            wait "$helper_pid"
            opened=$?
            helper_pid=
            break
        fi
        sleep 0.2
    done
    if [ -z "$opened" ]; then
        fail "open -n -W did not return in 2 minutes: the app did not exit"
        kill "$helper_pid" 2>/dev/null
        helper_pid=
    elif [ "$opened" -ne 0 ]; then
        fail "open -n --env failed with $opened (does this macOS pass the environment?): $(tr '\n' ' ' <"$extra_work/open.log")"
    else
        check_window_qt "the window of open -n" open "$extra_work/open.out" "$extra_work/open.err"
    fi
    extra_roostd_pid=$(lock_pid "$extra_work")
    if [ -z "$extra_roostd_pid" ]; then
        fail "open -n: no roostd under $extra_work/run: the environment did not reach the app"
    else
        exe=$(exe_of "$extra_roostd_pid")
        case $exe in
            "$app/Contents/MacOS/roostd"*) ok "open -n: roostd $extra_roostd_pid is $app/Contents/MacOS/roostd" ;;
            *) fail "open -n: the roostd that runs is not the one of the app: $exe" ;;
        esac
        if grep -qF 'roost --smoke-sessions:' "$extra_work/open.err" "$extra_work/open.out"; then
            fail "open -n: the window reported a failure:"
            show_tail "$extra_work/open.err"
            show_tail "$extra_work/open.out"
        elif [ -s "$extra_work/bin/attach.pid" ]; then
            open_attach=$(head -n 1 "$extra_work/bin/attach.pid")
            if kill -0 "$open_attach" 2>/dev/null; then
                ok "open -n: the window started from LaunchServices attached the fake claude ($open_attach)"
            else
                fail "open -n: the attach $open_attach is gone after the window exited"
            fi
        else
            fail "open -n: the fake claude was never attached"
        fi
        # launchd may end what an app leaves when it quits: look a while after,
        # at roostd and at the attach.
        sleep 3
        if kill -0 "$extra_roostd_pid" 2>/dev/null; then
            ok "open -n: roostd outlived the window"
        else
            fail "open -n: roostd $extra_roostd_pid did not outlive the window"
        fi
        if [ -n "${open_attach:-}" ]; then
            if kill -0 "$open_attach" 2>/dev/null; then
                ok "open -n: the attach $open_attach is still alive at the end"
            else
                fail "open -n: the attach $open_attach is gone at the end"
            fi
        fi
    fi
fi

exit "$failed"
