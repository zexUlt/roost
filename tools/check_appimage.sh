#!/usr/bin/env bash
# What the AppImage of tools/package_linux.sh has to be (docs/design/11-build.md):
#   - every ELF in it finds all its libraries (ldd, with no LD_LIBRARY_PATH),
#     so it does not lean on the Qt of the machine that built it;
#   - every shared library in it is in the licence inventory below, and each
#     component of the inventory that is in the AppImage has its heading in the
#     THIRD_PARTY_LICENSES the AppImage carries: a library nobody has put
#     through the licence review fails here;
#   - the QtCore QML module (a plugin imports it, and a QML scan that did not
#     see the plugins' QML would leave it out) and the licence texts are there;
#   - the AppImage itself, run with `--smoke-sessions` (offscreen, a temporary
#     HOME, socket and config, a fake `claude`, a login shell that does not
#     exist) shows the sessions and exits 0, and the roostd it started is the
#     AppImage's own, started as `$APPIMAGE --roostd`.
# Every failure is reported before the script ends; the exit code is 1 if there
# was one. What it started is stopped by pid.
#
# Usage: tools/check_appimage.sh <roost-<arch>.AppImage>
set -uo pipefail

appimage=${1:?usage: tools/check_appimage.sh <AppImage>}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
appimage=$(cd "$(dirname "$appimage")" && pwd)/$(basename "$appimage")
failed=0

fail() {
    echo "FAIL: $*"
    failed=1
}
ok() { echo "ok:   $*"; }

# classify <path relative to the AppDir>: the component of the inventory a
# shared library belongs to, as its heading in THIRD_PARTY_LICENSES; "roost"
# for our own; nothing for a library that is not in the inventory. A component
# is added here and in THIRD_PARTY_LICENSES together, after the owner agreed
# to it (docs/design/10-licenses.md).
classify() {
    case $1 in
        usr/lib/roost/plugins/*) echo roost ;;
        usr/plugins/* | usr/qml/*) echo "Qt 6" ;;
        usr/lib/libQt6*) echo "Qt 6" ;;
        usr/lib/libicu*) echo "ICU" ;;
    esac
}

tmp=$(mktemp -d "${TMPDIR:-/tmp}/roost-appimage.XXXXXX")
started=()
# shellcheck disable=SC2329  # runs from the EXIT trap
cleanup() {
    local pid
    for pid in "${started[@]}"; do
        kill "$pid" 2>/dev/null
    done
    # What is left that has this directory in its command line (roostd's log
    # is in it), however it got here.
    for pid in $(pgrep -f "$tmp" 2>/dev/null); do
        [ "$pid" = "$$" ] || kill "$pid" 2>/dev/null
    done
    sleep 1
    rm -rf "$tmp"
}
trap cleanup EXIT

chmod +x "$appimage"
mkdir "$tmp/extract"
(cd "$tmp/extract" && "$appimage" --appimage-extract >/dev/null) || {
    echo "FAIL: $appimage does not extract"
    exit 1
}
appdir=$tmp/extract/squashfs-root

# --- libraries -------------------------------------------------------------
elf_count=0
lib_count=0
while IFS= read -r -d '' file; do
    [ "$(head -c4 "$file" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
    rel=${file#"$appdir"/}
    elf_count=$((elf_count + 1))
    missing=$(env -u LD_LIBRARY_PATH ldd "$file" 2>&1 | grep 'not found')
    if [ -n "$missing" ]; then
        fail "$rel: ldd does not find"
        printf '%s\n' "$missing" | sed 's/^/        /'
    fi
    case $(basename "$file") in
        *.so | *.so.*)
            lib_count=$((lib_count + 1))
            component=$(classify "$rel")
            if [ -z "$component" ]; then
                fail "$rel is not in the licence inventory of tools/check_appimage.sh"
            else
                echo "$component" >>"$tmp/components"
            fi
            ;;
    esac
done < <(find "$appdir" -type f -print0)
ok "$elf_count ELF files, $lib_count of them shared libraries, were checked"

# --- licences --------------------------------------------------------------
licenses=$appdir/usr/share/licenses/roost
for name in LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt; do
    [ -s "$licenses/$name" ] || fail "usr/share/licenses/roost/$name is missing"
done
if [ -f "$tmp/components" ]; then
    while IFS= read -r component; do
        [ "$component" = roost ] && continue
        grep -qxF "$component" "$licenses/THIRD_PARTY_LICENSES" 2>/dev/null ||
            fail "'$component' is in the AppImage, but has no heading in its THIRD_PARTY_LICENSES"
    done < <(sort -u "$tmp/components")
fi
# The file of the repository is the one the AppImage carries.
cmp -s "$root/THIRD_PARTY_LICENSES" "$licenses/THIRD_PARTY_LICENSES" ||
    fail "THIRD_PARTY_LICENSES of the AppImage is not the one of the repository"

# --- QML -------------------------------------------------------------------
for module in QtCore QtQuick/Controls QtQuick/Layouts; do
    [ -f "$appdir/usr/qml/$module/qmldir" ] ||
        fail "usr/qml/$module/qmldir is missing"
done
for file in plugins/claude/backend/claude-adapter plugins/sessions/qml/RoostSessions/qmldir \
    plugins/sessions/qml/RoostSessions/libsessions_qml.so plugins/claude/plugin.json; do
    [ -e "$appdir/usr/lib/roost/$file" ] || fail "usr/lib/roost/$file is missing"
done

# --- the AppImage itself ---------------------------------------------------
# The same isolation as tests/roost/window_spawn_test.cpp: nothing of the real
# home, the real claude or the login shell of whoever runs this.
mkdir -p "$tmp/home" "$tmp/bin" "$tmp/run" "$tmp/config"
chmod 700 "$tmp/run"
cp "$root/tests/fixtures/claude_agents.json" "$tmp/bin/agents.json"
cat >"$tmp/bin/claude" <<'FAKE'
#!/bin/sh
dir=$(dirname "$0")
case "$1" in
agents) cat "$dir/agents.json" ;;
attach) echo $$ > "$dir/attach.pid.new" && mv "$dir/attach.pid.new" "$dir/attach.pid"; exec sleep 600 ;;
*) exit 2 ;;
esac
FAKE
chmod +x "$tmp/bin/claude"
printf '%s' '{"version":1,"view":{"opened":"claude/bg-0a1b2c3d"}}' >"$tmp/config/sessions.json"

window_log=$tmp/window.log
env -i PATH="$tmp/bin:/usr/bin:/bin" HOME="$tmp/home" \
    QT_QPA_PLATFORM=offscreen QML_DISABLE_DISK_CACHE=1 \
    ROOST_SOCKET="$tmp/run/roostd.sock" ROOST_CONFIG="$tmp/config" \
    SHELL="$tmp/no-such-shell" \
    timeout 120 "$appimage" --smoke-sessions >"$window_log" 2>&1
code=$?
if [ "$code" -ne 0 ]; then
    fail "$(basename "$appimage") --smoke-sessions exited $code:"
    sed 's/^/        /' "$window_log"
else
    ok "$(basename "$appimage") --smoke-sessions exits 0"
fi

lock=$tmp/run/roostd.sock.lock
pid=$(head -n1 "$lock" 2>/dev/null)
if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
    started+=("$pid")
    exe=$(readlink "/proc/$pid/exe" 2>/dev/null)
    # Started as `$APPIMAGE --roostd`, AppRun gives it an environment of its
    # own: APPIMAGE is this file, and the mount it runs from is not the
    # extracted copy. The window strips both from what it starts, so a roostd
    # that was started as the sibling file has neither.
    if [[ $exe == */usr/bin/roostd && $exe != "$appdir"/* ]] &&
        tr '\0' '\n' <"/proc/$pid/environ" | grep -qxF "APPIMAGE=$appimage"; then
        ok "roostd $pid was started by the AppImage ($exe)"
    else
        fail "roostd $pid was not started as \$APPIMAGE --roostd (exe: $exe)"
    fi
else
    fail "no roostd is running after the window ended (lock: $lock)"
fi
attach=$(head -n1 "$tmp/bin/attach.pid" 2>/dev/null)
[ -z "$attach" ] || started+=("$attach")
[ ! -s "$window_log" ] || sed 's/^/      window: /' "$window_log"

exit "$failed"
