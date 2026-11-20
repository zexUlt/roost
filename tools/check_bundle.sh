#!/usr/bin/env bash
# The macOS bundle stands on its own (docs/design/11-build.md): every Mach-O in
# it names only libraries of the bundle (@rpath, @executable_path,
# @loader_path) or of the system, the QML modules the window and the plugins
# import are deployed, the licences are in it, the signature verifies, and the
# bundled window starts the bundled roostd and shows the sessions.
#
# With a .dmg as the second argument the image is checked too: it verifies, it
# holds roost.app (whose signature verifies), the link to /Applications and the
# licences. It is mounted read-only, with no browse and no Finder, at a
# temporary mount point, and detached on every way out.
#
# The smoke runs in a temporary directory with its own HOME, socket and
# config, a $SHELL that does not exist (never the login shell of whoever runs
# this) and a fake `claude` (never the real one). It leaves the roostd it
# started running, as the window does, and kills that roostd and the fake
# attach by pid at the end.
#
# Usage: tools/check_bundle.sh <roost.app> [<roost.dmg>]
set -uo pipefail

app=${1:?usage: tools/check_bundle.sh <roost.app> [<roost.dmg>]}
dmg=${2:-}
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

work=
mnt=
roostd_pid=
window_pid=

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
    if [ -n "$work" ] && [ -s "$work/bin/attach.pid" ]; then
        kill "$(cat "$work/bin/attach.pid")" 2>/dev/null
    fi
    detach_dmg
    if [ -n "$work" ]; then
        rm -rf "$work"
    fi
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# The way dyld resolves a dependency of $1 named $2, given the run paths $3
# of $1: @loader_path is the directory of $1, @executable_path is
# Contents/MacOS, @rpath is each run path in turn (a run path may itself start
# with @loader_path or @executable_path). dyld also tries the run paths of the
# executable that started the load (a Homebrew dylib names @rpath/libwebp but
# carries only @loader_path/../lib, and finds it through the run path of the
# executable): $3 is followed by those of the executables of the bundle,
# already resolved. Succeeds when one of the resulting paths exists and,
# symlinks resolved, lies inside the bundle.
expand_path() {
    case $2 in
        @loader_path/*) printf '%s\n' "$(dirname "$1")/${2#@loader_path/}" ;;
        @executable_path/*) printf '%s\n' "$app/Contents/MacOS/${2#@executable_path/}" ;;
        *) printf '%s\n' "$2" ;;
    esac
}

in_bundle() {
    local dir
    [ -e "$1" ] || return 1
    dir=$(cd "$(dirname "$1")" 2>/dev/null && pwd -P) || return 1
    case $dir/ in
        "$app"/*) return 0 ;;
        *) return 1 ;;
    esac
}

resolve_in_bundle() {
    local file=$1 dep=$2 runpaths=$3 candidate runpath
    case $dep in
        @rpath/*)
            while IFS= read -r runpath; do
                [ -n "$runpath" ] || continue
                candidate=$(expand_path "$file" "$runpath")/${dep#@rpath/}
                if in_bundle "$candidate"; then
                    return 0
                fi
            done <<<"$runpaths"
            return 1
            ;;
        *) in_bundle "$(expand_path "$file" "$dep")" ;;
    esac
}

# ---------------------------------------------------------------------------
# Every Mach-O names only the bundle and the system.

exe_runpaths=
while IFS= read -r -d '' exe; do
    if file -b "$exe" | grep -q '^Mach-O'; then
        while IFS= read -r runpath; do
            [ -n "$runpath" ] || continue
            exe_runpaths+=$(expand_path "$exe" "$runpath")$'\n'
        done < <(otool -l "$exe" | awk '/cmd LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}')
    fi
done < <(find "$app/Contents/MacOS" -type f -print0)

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
    runpaths=$(otool -l "$file" | awk '/cmd LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}')
    while IFS= read -r dep; do
        [ -n "$dep" ] || continue
        # A bundle (a Qt plugin) lists its own path where a dylib lists its
        # install name.
        if [ "$dep" = "$id" ] || [ "$dep" = "$file" ]; then
            continue
        fi
        case $dep in
            /usr/lib/* | /System/Library/*) ;;
            @executable_path/* | @loader_path/* | @rpath/*)
                if ! resolve_in_bundle "$file" "$dep" "$runpaths"$'\n'"$exe_runpaths"; then
                    fail "$rel needs $dep, which is not in the bundle"
                fi
                ;;
            *) fail "$rel names $dep" ;;
        esac
    done <<<"$deps"
    # A run path outside the bundle would let a machine with Qt installed
    # load that one when the bundle's own copy is missing.
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
    Contents/Resources/qml/QtQuick/Layouts \
    Contents/PlugIns/imageformats/libqsvg.dylib; do
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
# The .dmg.

if [ -n "$dmg" ]; then
    if [ ! -f "$dmg" ]; then
        fail "$dmg is not a file"
    elif ! hdiutil verify -quiet "$dmg" >/dev/null 2>&1; then
        fail "hdiutil verify does not accept $dmg"
    else
        echo "ok:   hdiutil verify"
        mnt=$(mktemp -d "${TMPDIR:-/tmp}/roost-dmg.XXXXXX") || exit 1
        if ! hdiutil attach -readonly -nobrowse -noautoopen -quiet \
            -mountpoint "$mnt" "$dmg" >/dev/null 2>&1; then
            fail "hdiutil attach of $dmg failed"
            rmdir "$mnt" 2>/dev/null
            mnt=
        else
            if [ -d "$mnt/roost.app/Contents" ]; then
                echo "ok:   dmg: roost.app"
                if verify=$(codesign --verify --deep --strict --verbose=2 "$mnt/roost.app" 2>&1); then
                    echo "ok:   dmg: codesign --verify --deep --strict"
                else
                    printf '%s\n' "$verify" | sed 's/^/    /'
                    fail "codesign does not verify roost.app in $dmg"
                fi
            else
                fail "$dmg has no roost.app"
            fi
            if [ -L "$mnt/Applications" ] && [ "$(readlink "$mnt/Applications")" = /Applications ]; then
                echo "ok:   dmg: Applications -> /Applications"
            else
                fail "$dmg has no Applications link to /Applications"
            fi
            for name in LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt; do
                if [ -s "$mnt/Licenses/$name" ]; then
                    echo "ok:   dmg: Licenses/$name"
                else
                    fail "Licenses/$name is missing or empty in $dmg"
                fi
            done
            detach_dmg
        fi
    fi
fi

# ---------------------------------------------------------------------------
# The window starts the roostd of the bundle.

work=$(mktemp -d "${TMPDIR:-/tmp}/roost-bundle.XXXXXX") || exit 1

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
    # Qt that this roostd has loaded comes from the bundle, and it has loaded
    # QtCore from there (an empty answer of lsof is a failure, not a pass).
    # -Fn: one `n<path>` line per file, so that a path with spaces is whole.
    loaded=$(lsof -Fn -p "$roostd_pid" 2>/dev/null | sed -n 's/^n//p')
    qt=$(printf '%s\n' "$loaded" | grep -iE '/Qt[A-Za-z]*(\.framework/|$)|libQt' || true)
    outside=$(printf '%s\n' "$qt" | grep -vF "$app/" | grep -v '^$' || true)
    if [ -n "$outside" ]; then
        fail "roostd loaded Qt from outside the bundle:"
        printf '%s\n' "$outside" | sed 's/^/    /'
    elif printf '%s\n' "$qt" | grep -F "$app/" | grep -qE '/QtCore(\.framework/|$)'; then
        echo "ok:   roostd loaded QtCore from the bundle and no Qt from outside it"
    else
        fail "roostd has no QtCore from $app/ loaded (lsof saw $(printf '%s\n' "$loaded" | grep -c .) files)"
    fi
fi

if [ ! -s "$work/bin/attach.pid" ]; then
    fail "the fake claude was never attached: the bundled backend did not run it"
else
    echo "ok:   the bundled claude-adapter ran the fake claude"
fi

exit "$failed"
