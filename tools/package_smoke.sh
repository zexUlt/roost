#!/usr/bin/env bash
# The package on a machine that is not the one that built it (docs/design/11-build.md):
# a .dmg on macOS, an AppImage on Linux, one script for both. It runs the
# packaged window and the daemon it starts, and checks what the user relies
# on once the window is closed:
#   - the window, with an isolated socket and config, a fake `claude` and an
#     opened session, passes `--smoke-sessions` (it lists the sessions and
#     attaches to the opened one);
#   - after the window exited, roostd is alive, runs from the package (the
#     .app copied out of the image, the mount of the AppImage: its own
#     mount, which the window's exit does not take away) and loaded its Qt from
#     the package only; the fake attach is alive too;
#   - `roostd --dump` of the package answers;
#   - `claude-adapter`, killed by pid with no window around, comes back;
#   - a second window `--smoke-sessions` connects to the same roostd pid;
#   - the licence texts are in the package.
# With --launch-services (macOS) the window is also started once through
# LaunchServices, `open -n` with the environment passed by `open --env`: roostd
# must start from the app and outlive the window as it does in a direct run.
# A failure of `open --env` itself is a FAIL: say so in the job, not a skip.
#
# The environment is isolated as in tools/check_bundle.sh: a temporary HOME,
# socket, config and TMPDIR, a $SHELL that does not exist (never the login
# shell of whoever runs this), a fake `claude` (never the real one), no
# display. The AppImage runs through FUSE, as a user runs it, not through
# --appimage-extract-and-run. What the script starts it kills by pid, and it
# detaches only the image it mounted, at its own mount point.
#
# Every failure is reported before the script ends; the exit code is 1 if
# there was one.
#
# Usage: tools/package_smoke.sh <roost.dmg | roost-<arch>.AppImage> [--launch-services]
set -uo pipefail

usage="usage: tools/package_smoke.sh <roost.dmg | roost-<arch>.AppImage> [--launch-services]"
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
attach_pids=()

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

# kill_tree <pid>: the pid and its children, by pid (the plugin backends).
# shellcheck disable=SC2329  # called from cleanup, which runs from the trap
kill_tree() {
    local pid
    [ -n "$1" ] || return 0
    for pid in $(pgrep -P "$1" 2>/dev/null) "$1"; do
        kill "$pid" 2>/dev/null
    done
}

# shellcheck disable=SC2329  # runs from the trap
cleanup() {
    local pid dir
    for pid in "$window_pid" "$helper_pid"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null
    done
    kill_tree "$roostd_pid"
    kill_tree "$extra_roostd_pid"
    for dir in "$work" "$extra_work"; do
        [ -n "$dir" ] && [ -s "$dir/bin/attach.pid" ] && kill "$(cat "$dir/bin/attach.pid")" 2>/dev/null
    done
    # (the expansion of an empty array fails under `set -u` in bash 3.2)
    for pid in ${attach_pids[@]+"${attach_pids[@]}"}; do
        kill "$pid" 2>/dev/null
    done
    detach_dmg
    # The FUSE mount of the AppImage goes away with the last process that
    # runs from it.
    [ "$kind" = linux ] && sleep 1
    for dir in "$work" "$extra_work"; do
        [ -n "$dir" ] && rm -rf "$dir"
    done
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
attach) echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 600 ;;
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
        "TMPDIR=$1/" \
        "PATH=$1/bin:/usr/bin:/bin" \
        "SHELL=$1/no-such-shell" \
        "ROOST_SOCKET=$1/run/roostd.sock" \
        "ROOST_CONFIG=$1/config" \
        "QT_QPA_PLATFORM=offscreen" \
        "QML_DISABLE_DISK_CACHE=1"
}

# The same as `env -i` with window_env, for a command run directly. On Linux
# no TMPDIR: the runtime of the AppImage mounts under it, and a mount in the
# directory this script removes at the end would be in the way of the unmount.
run_window() {
    local dir=$1 out=$2 extra=${3:-}
    local vars=()
    while IFS= read -r line; do
        case $kind:$line in
            linux:TMPDIR=*) ;;
            *) vars+=("$line") ;;
        esac
    done < <(window_env "$dir")
    # shellcheck disable=SC2086  # $extra is at most one VAR=value
    env -i "${vars[@]}" $extra "${window_cmd[@]}" --smoke-sessions \
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
    kill "$window_pid" 2>/dev/null
    window_pid=
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
    if [ -e /opt/homebrew/opt/qtbase ]; then
        fail "/opt/homebrew/opt/qtbase exists: this machine has a Qt, the smoke proves nothing"
    fi
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

# macOS: dyld names every library it loads, so the Qt that is not the
# bundle's shows up for the window as well as for roostd.
dyld_env=
[ "$kind" = macos ] && dyld_env=DYLD_PRINT_LIBRARIES=1
run_window "$work" "$work/window.out" "$dyld_env"
wait_window
if [ -z "$window_status" ]; then
    fail "roost --smoke-sessions did not end in 2 minutes"
    show_tail "$work/window.out"
elif [ "$window_status" -ne 0 ]; then
    fail "roost --smoke-sessions exited with $window_status"
    show_tail "$work/window.out"
else
    ok "roost --smoke-sessions"
fi

if [ "$kind" = macos ]; then
    # Qt that the window loaded: from the bundle only.
    outside=$(grep -E '/Qt[A-Za-z0-9]*(\.framework/| |$)|libQt' "$work/window.out" |
        grep -vF "$app/" || true)
    inside=$(grep -F "$app/" "$work/window.out" | grep -cE '/QtCore(\.framework/| |$)' || true)
    if [ -n "$outside" ]; then
        fail "the window loaded Qt from outside the bundle:"
        printf '%s\n' "$outside" | head -n 10 | sed 's/^/    /'
    elif [ "$inside" -eq 0 ]; then
        fail "DYLD_PRINT_LIBRARIES showed no QtCore of the bundle: the check saw nothing"
    else
        ok "the window loaded QtCore from the bundle and no Qt from outside it"
    fi
fi

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

# The window is ready without waiting for the attach of the opened session:
# one that exits first leaves no attach to find. A moment for it, then another
# window against the same roostd, at most twice (a note says so): what is
# proved is that the attach of a packaged backend outlives the window that
# asked for it.
attach_wait() {
    local _
    for _ in $(seq 1 50); do
        [ -s "$1/bin/attach.pid" ] && return 0
        sleep 0.1
    done
    return 1
}
retries=0
while ! attach_wait "$work" && [ "$retries" -lt 2 ]; do
    retries=$((retries + 1))
    echo "note: the window exited before the attach started; another window ($retries)"
    run_window "$work" "$work/window-retry.out" "$dyld_env"
    wait_window
done
attach=$(head -n 1 "$work/bin/attach.pid" 2>/dev/null)
if [ -z "$attach" ]; then
    fail "the fake claude was never attached: the packaged backend did not run it"
else
    attach_pids+=("$attach")
    if kill -0 "$attach" 2>/dev/null; then
        ok "the attach $attach is alive after the window exited"
    else
        fail "the attach $attach is gone after the window exited"
    fi
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

run_window "$work" "$work/window2.out" "$dyld_env"
wait_window
if [ -z "$window_status" ]; then
    fail "the second roost --smoke-sessions did not end in 2 minutes"
    show_tail "$work/window2.out"
elif [ "$window_status" -ne 0 ]; then
    fail "the second roost --smoke-sessions exited with $window_status"
    show_tail "$work/window2.out"
else
    second=$(lock_pid "$work")
    if [ "$second" = "$roostd_pid" ] && kill -0 "$roostd_pid" 2>/dev/null; then
        ok "the second window connected to the same roostd $roostd_pid"
    else
        fail "the second window ran against roostd '$second', not $roostd_pid"
    fi
fi
if [ "$kind" = macos ]; then
    outside=$(grep -E '/Qt[A-Za-z0-9]*(\.framework/| |$)|libQt' "$work/window2.out" |
        grep -vF "$app/" || true)
    if [ -n "$outside" ]; then
        fail "the second window loaded Qt from outside the bundle:"
        printf '%s\n' "$outside" | head -n 10 | sed 's/^/    /'
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
    done < <(window_env "$extra_work")
    # One `open -n` runs one window; as above, a window that exits before the
    # attach started is followed by one more, at most twice.
    opened=
    for attempt in 1 2 3; do
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
            break
        elif [ "$opened" -ne 0 ]; then
            fail "open -n --env failed with $opened (does this macOS pass the environment?): $(tr '\n' ' ' <"$extra_work/open.log")"
            break
        fi
        attach_wait "$extra_work" && break
        echo "note: open -n: the window exited before the attach started (attempt $attempt)"
    done
    extra_roostd_pid=$(lock_pid "$extra_work")
    if [ -z "$extra_roostd_pid" ]; then
        fail "open -n: no roostd under $extra_work/run: the environment did not reach the app"
    else
        exe=$(exe_of "$extra_roostd_pid")
        case $exe in
            "$app/Contents/MacOS/roostd"*) ok "open -n: roostd $extra_roostd_pid is $app/Contents/MacOS/roostd" ;;
            *) fail "open -n: the roostd that runs is not the one of the app: $exe" ;;
        esac
        if grep -q 'smoke-sessions' "$extra_work/open.err" "$extra_work/open.out"; then
            fail "open -n: the window reported a failure:"
            show_tail "$extra_work/open.err"
            show_tail "$extra_work/open.out"
        elif [ -s "$extra_work/bin/attach.pid" ]; then
            ok "open -n: the window started from LaunchServices attached the fake claude"
        else
            fail "open -n: the fake claude was never attached"
        fi
        # launchd may end what an app leaves when it quits: look a while after.
        sleep 3
        if kill -0 "$extra_roostd_pid" 2>/dev/null; then
            ok "open -n: roostd outlived the window"
        else
            fail "open -n: roostd $extra_roostd_pid did not outlive the window"
        fi
    fi
fi

exit "$failed"
