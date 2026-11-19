#!/usr/bin/env bash
# The macOS bundle stands on its own (docs/design/11-build.md): every Mach-O in
# it names only libraries of the bundle (@rpath, @executable_path,
# @loader_path) or of the system, the QML modules the window and the plugins
# import are deployed, the licences are in it, the signature verifies, and the
# bundled window starts the bundled roostd and shows the sessions.
#
# The smoke runs in a temporary directory with its own HOME, socket and
# config, a $SHELL that does not exist (never the login shell of whoever runs
# this) and a fake `claude` (never the real one). It leaves the roostd it
# started running, as the window does, and kills that roostd and the fake
# attach by pid at the end.
#
# Usage: tools/check_bundle.sh <roost.app>
set -uo pipefail

app=${1:?usage: tools/check_bundle.sh <roost.app>}
if [ ! -d "$app/Contents" ]; then
    echo "FAIL: $app is not an application bundle"
    exit 1
fi
app=$(cd "$app" && pwd -P)
root=$(cd "$(dirname "$0")/.." && pwd -P)
failed=0

fail() {
    echo "FAIL: $*"
    failed=1
}

# ---------------------------------------------------------------------------
# Every Mach-O names only the bundle and the system.

macho=$(find "$app" -type f -print0 | xargs -0 file |
    sed -n 's/: *Mach-O .*//p')
if [ -z "$macho" ]; then
    fail "no Mach-O file in $app"
fi

count=0
while IFS= read -r file; do
    [ -n "$file" ] || continue
    count=$((count + 1))
    rel=${file#"$app"/}

    # The install name of a dylib is the first of the lines of `otool -L`
    # after its name, and is not a dependency; `otool -D` tells which line it
    # is. (A Qt of Homebrew leaves absolute install names on the frameworks it
    # copies; nothing depends on them once the dependents name @rpath.)
    id=$(otool -D "$file" | sed -n '2p')
    deps=$(otool -L "$file" | sed -n '2,$p' | awk '{print $1}')
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        [ "$dep" = "$id" ] && continue
        case $dep in
            @executable_path/* | @loader_path/* | /usr/lib/* | /System/Library/*) ;;
            @rpath/*)
                # Not just the prefix: the library is in the bundle.
                if [ ! -e "$app/Contents/Frameworks/${dep#@rpath/}" ] &&
                    [ -z "$(find "$app" -name "$(basename "$dep")" -print -quit)" ]; then
                    fail "$rel needs $dep, which is not in the bundle"
                fi
                ;;
            *) fail "$rel names $dep" ;;
        esac
    done <<<"$deps"
    # A run path outside the bundle would let a machine with Qt installed
    # load that one when the bundle's own copy is missing.
    runpaths=$(otool -l "$file" | awk '/cmd LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}')
    while IFS= read -r runpath; do
        [ -n "$runpath" ] || continue
        case $runpath in
            @executable_path/* | @loader_path/* | @rpath/*) ;;
            *) fail "$rel has the run path $runpath" ;;
        esac
    done <<<"$runpaths"
done <<<"$macho"
echo "ok:   $count Mach-O files name only the bundle and the system"

# ---------------------------------------------------------------------------
# What the window and the plugins import, and the licences.

for path in Contents/Resources/qml/QtCore/qmldir \
    Contents/Resources/qml/QtQuick/Controls \
    Contents/Resources/qml/QtQuick/Layouts; do
    if [ -e "$app/$path" ]; then
        echo "ok:   $path"
    else
        fail "$path is not deployed"
    fi
done

for name in LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt; do
    if [ -s "$app/Contents/Resources/licenses/$name" ]; then
        echo "ok:   licenses/$name"
    else
        fail "Contents/Resources/licenses/$name is missing or empty"
    fi
done

# ---------------------------------------------------------------------------
# The signature.

if verify=$(codesign --verify --deep --strict --verbose=2 "$app" 2>&1); then
    echo "ok:   codesign --verify --deep --strict"
else
    printf '%s\n' "$verify" | sed 's/^/    /'
    fail "codesign does not verify $app"
fi

# ---------------------------------------------------------------------------
# The window starts the roostd of the bundle.

work=$(mktemp -d "${TMPDIR:-/tmp}/roost-bundle.XXXXXX") || exit 1
roostd_pid=
window_pid=

# shellcheck disable=SC2329  # runs from the trap
cleanup() {
    if [ -n "$window_pid" ]; then
        kill "$window_pid" 2>/dev/null
    fi
    local pid
    # The roostd the window started, its children (the plugin backends) and
    # the fake attach: found by the pid files, not by name.
    if [ -n "$roostd_pid" ]; then
        for pid in $(pgrep -P "$roostd_pid" 2>/dev/null) "$roostd_pid"; do
            kill "$pid" 2>/dev/null
        done
    fi
    if [ -s "$work/bin/attach.pid" ]; then
        kill "$(cat "$work/bin/attach.pid")" 2>/dev/null
    fi
    rm -rf "$work"
}
trap cleanup EXIT

# The directory of the socket must be private: roostd takes over only that.
mkdir -p "$work/run" "$work/home" "$work/bin" "$work/config"
chmod 700 "$work/run"
cat >"$work/bin/claude" <<'EOF'
#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 600 ;;
*) exit 2 ;;
esac
EOF
chmod 755 "$work/bin/claude"
cp "$root/tests/fixtures/claude_agents.json" "$work/bin/agents.json"
# The store document of the sessions plugin with one session opened, so that
# the window attaches to it.
printf '%s' '{"version":1,"view":{"opened":"claude/bg-0a1b2c3d"}}' \
    >"$work/config/sessions.json"

env -i \
    HOME="$work/home" \
    TMPDIR="$work/" \
    PATH="$work/bin:/usr/bin:/bin" \
    SHELL="$work/no-such-shell" \
    ROOST_SOCKET="$work/run/roostd.sock" \
    ROOST_CONFIG="$work/config" \
    QT_QPA_PLATFORM=offscreen \
    QML_DISABLE_DISK_CACHE=1 \
    "$app/Contents/MacOS/roost" --smoke-sessions \
    >"$work/window.out" 2>&1 </dev/null &
window_pid=$!

# The mode gives up by itself after 20 s; the loop is for a window that hangs.
status=
for _ in $(seq 1 600); do
    if ! kill -0 "$window_pid" 2>/dev/null; then
        wait "$window_pid"
        status=$?
        window_pid=
        break
    fi
    sleep 0.2
done

# Who holds the socket: the first line of the lock file is the pid.
if [ -s "$work/run/roostd.sock.lock" ]; then
    roostd_pid=$(head -n 1 "$work/run/roostd.sock.lock" | tr -dc '0-9')
fi

if [ -z "$status" ]; then
    fail "roost --smoke-sessions did not end in 2 minutes"
    sed 's/^/    /' "$work/window.out"
elif [ "$status" -ne 0 ]; then
    fail "roost --smoke-sessions exited with $status"
    sed 's/^/    /' "$work/window.out"
else
    echo "ok:   roost --smoke-sessions"
fi

if [ -z "$roostd_pid" ] || ! kill -0 "$roostd_pid" 2>/dev/null; then
    fail "the window left no roostd running"
else
    command=$(ps -o command= -p "$roostd_pid")
    case $command in
        "$app/Contents/MacOS/roostd"*)
            echo "ok:   roostd $roostd_pid is $app/Contents/MacOS/roostd"
            ;;
        *) fail "the roostd that runs is not the bundled one: $command" ;;
    esac
    # Qt that this roostd has loaded comes from the bundle.
    outside=$(lsof -p "$roostd_pid" 2>/dev/null | awk '{print $NF}' |
        grep -iE '/Qt[A-Za-z]*(\.framework|$)|libQt' | grep -vF "$app/" || true)
    if [ -n "$outside" ]; then
        fail "roostd loaded Qt from outside the bundle:"
        printf '%s\n' "$outside" | sed 's/^/    /'
    else
        echo "ok:   roostd loaded no Qt from outside the bundle"
    fi
fi

if [ ! -s "$work/bin/attach.pid" ]; then
    fail "the fake claude was never attached: the bundled backend did not run it"
else
    echo "ok:   the bundled claude-adapter ran the fake claude"
fi

exit "$failed"
