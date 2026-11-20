#!/usr/bin/env bash
# What the AppImage of tools/package_linux.sh has to be (docs/design/11-build.md):
#   - every ELF in it finds all its libraries (ldd, with no LD_LIBRARY_PATH),
#     so it does not lean on the Qt of the machine that built it;
#   - every RPATH/RUNPATH entry of every ELF is relative to $ORIGIN: one that
#     is not (a path of the build machine's Qt) is resolved by ldd on that
#     machine and by nobody else;
#   - no library of every desktop (packaging/linux/system-libraries.txt) is in
#     it, and every shared library in it is in the licence inventory below, and each
#     component of the inventory that is in the AppImage has its heading in the
#     THIRD_PARTY_LICENSES the AppImage carries: a library nobody has put
#     through the licence review fails here;
#   - none of roost's own ELF files (usr/bin, usr/lib/roost: libghostty-vt is
#     linked into them) has an AVX-512 instruction (`%zmm`, `%xmm16`-`%ymm31`,
#     `%k0`-`%k7` in `objdump -d`; EVEX-only instructions on the lower
#     registers are not caught) outside the functions that are picked at run time by the
#     CPU (the icelake implementation of simdutf, the AVX3 targets of
#     highway): an image built for the CPU of the build machine would die of
#     SIGILL on a machine without AVX-512;
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
# pwd -P: the runtime puts the real path into APPIMAGE.
appimage=$(cd "$(dirname "$appimage")" && pwd -P)/$(basename "$appimage")
failed=0

fail() {
    echo "FAIL: $*"
    failed=1
}
ok() { echo "ok:   $*"; }

# system_library <file name>: succeeds if the file is a library of every
# desktop, which the AppImage does not carry (the globs of
# packaging/linux/system-libraries.txt).
system_library() {
    local pattern
    while IFS= read -r pattern; do
        # shellcheck disable=SC2053  # the pattern is a glob on purpose
        [[ $1 == $pattern ]] && return 0
    done < <(grep -v -e '^#' -e '^$' "$root/packaging/linux/system-libraries.txt")
    return 1
}

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
        # What linuxdeploy copies from the build machine (Ubuntu 24.04) and a
        # stock desktop may lack: the xcb-util family, libxcb-shape,
        # libxcb-xkb and libxkbcommon-x11. The libraries of every desktop are not carried
        # (packaging/linux/system-libraries.txt): see `system_library`.
        usr/lib/libxcb-*.so*) echo "X11 and XCB libraries" ;;
        usr/lib/libxkbcommon-x11.so*) echo "libxkbcommon" ;;
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
rpath_count=0
while IFS= read -r -d '' file; do
    [ "$(head -c4 "$file" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
    rel=${file#"$appdir"/}
    elf_count=$((elf_count + 1))
    missing=$(env -u LD_LIBRARY_PATH ldd "$file" 2>&1 | grep 'not found')
    if [ -n "$missing" ]; then
        fail "$rel: ldd does not find"
        printf '%s\n' "$missing" | sed 's/^/        /'
    fi
    # RPATH and RUNPATH entries: each one $ORIGIN-relative.
    while IFS= read -r entry; do
        [ -n "$entry" ] || continue
        case $entry in
            "\$ORIGIN" | "\$ORIGIN/"*) ;;
            *) fail "$rel: RPATH/RUNPATH entry '$entry' is not relative to \$ORIGIN" ;;
        esac
        rpath_count=$((rpath_count + 1))
    done < <(readelf -d "$file" 2>/dev/null |
        sed -n 's/.*(\(RPATH\|RUNPATH\)).*\[\(.*\)\]/\2/p' | tr ':' '\n')
    case $(basename "$file") in
        *.so | *.so.*)
            lib_count=$((lib_count + 1))
            component=$(classify "$rel")
            if system_library "$(basename "$file")"; then
                fail "$rel is a library of every desktop (packaging/linux/system-libraries.txt): the AppImage does not carry it"
            elif [ -z "$component" ]; then
                fail "$rel is not in the licence inventory of tools/check_appimage.sh"
            else
                echo "$component" >>"$tmp/components"
            fi
            ;;
    esac
done < <(find "$appdir" -type f -print0)
ok "$elf_count ELF files, $lib_count of them shared libraries, were checked"
# Our own binaries have run paths: none found means readelf did not work.
if [ "$rpath_count" -gt 0 ]; then
    ok "$rpath_count RPATH/RUNPATH entries were checked, each relative to \$ORIGIN unless FAILed above"
else
    fail "no RPATH/RUNPATH entry found in any ELF: readelf failed?"
fi

# --- AVX-512 ---------------------------------------------------------------
# The CPU of libghostty-vt is pinned in cmake/ghostty.cmake. simdutf and
# highway carry AVX-512 variants on purpose and call them only on a CPU that has
# it; anything else with a `zmm` register was compiled for the build machine.
echo "build machine: $(grep -m1 'model name' /proc/cpuinfo 2>/dev/null || uname -m)"
if command -v objdump >/dev/null 2>&1; then
    avx512_count=0
    avx512_dispatched=0
    avx512_bad=0
    while IFS= read -r -d '' file; do
        [ "$(head -c4 "$file" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
        avx512_count=$((avx512_count + 1))
        # <count of instructions> <function>, one line per function.
        if ! report=$(set -o pipefail; objdump -d --no-show-raw-insn "$file" | awk '
            /^[0-9a-f]+ <.*>:$/ { sym = $2; next }
            /%zmm|%[xy]mm(1[6-9]|2[0-9]|3[01])|%k[0-7]/ { n[sym]++ }
            END { for (s in n) print n[s], s }'); then
            fail "objdump failed on ${file#"$appdir"/}"
            avx512_bad=1
            continue
        fi
        bad=$(printf '%s\n' "$report" | grep -v -e '^$' -e 'simdutf7icelake' -e 'N_AVX3' || true)
        if [ -n "$bad" ]; then
            fail "${file#"$appdir"/} has AVX-512 instructions outside the run-time dispatched functions (built for the CPU of the build machine?):"
            printf '%s\n' "$bad" | sort -rn | head -5 | sed 's/^/        /'
            avx512_bad=1
        fi
        avx512_dispatched=$((avx512_dispatched + $(printf '%s\n' "$report" | grep -c -e 'simdutf7icelake' -e 'N_AVX3')))
    done < <(find "$appdir/usr/bin" "$appdir/usr/lib/roost" -type f -print0)
    if [ "$avx512_count" -eq 0 ]; then
        fail "no ELF file found for the AVX-512 check"
    elif [ "$(uname -m)" = x86_64 ] && [ "$avx512_dispatched" -eq 0 ]; then
        # simdutf and highway always carry them on x86_64: none found means
        # the check sees nothing (objdump output or symbol names changed).
        fail "no run-time dispatched AVX-512 function found on x86_64: the AVX-512 check sees nothing"
    elif [ "$avx512_bad" -eq 0 ]; then
        ok "no AVX-512 instruction in $avx512_count ELF files of roost outside the $avx512_dispatched run-time dispatched functions"
    fi
else
    fail "objdump is missing: the AVX-512 check cannot run (install binutils)"
fi

# --- licences --------------------------------------------------------------
licenses=$appdir/usr/share/licenses/roost
for name in LICENSE NOTICE THIRD_PARTY_LICENSES LGPL-3.0.txt GPL-3.0.txt ICU-LICENSE.txt; do
    [ -s "$licenses/$name" ] || fail "usr/share/licenses/roost/$name is missing"
done
if [ -f "$tmp/components" ]; then
    while IFS= read -r component; do
        [ "$component" = roost ] && continue
        grep -qxF "$component" "$licenses/THIRD_PARTY_LICENSES" 2>/dev/null ||
            fail "'$component' is in the AppImage, but has no heading in its THIRD_PARTY_LICENSES"
    done < <(sort -u "$tmp/components")
fi
if cmp -s "$root/licenses/ICU-LICENSE.txt" "$licenses/ICU-LICENSE.txt"; then
    ok "usr/share/licenses/roost/ICU-LICENSE.txt is the ICU licence text of the repository"
else
    fail "ICU-LICENSE.txt of the AppImage is not licenses/ICU-LICENSE.txt"
fi
# The file of the repository is the one the AppImage carries.
cmp -s "$root/THIRD_PARTY_LICENSES" "$licenses/THIRD_PARTY_LICENSES" ||
    fail "THIRD_PARTY_LICENSES of the AppImage is not the one of the repository"

# Copyright files of the Debian packages that linuxdeploy copied libraries from.
copyrights=$(find "$appdir/usr/share/doc" -name copyright 2>/dev/null | wc -l)
if [ "$copyrights" -gt 0 ]; then
    ok "$copyrights copyright files of the bundled system libraries in usr/share/doc"
    # What the licence names of THIRD_PARTY_LICENSES are checked against.
    for file in "$appdir"/usr/share/doc/*/copyright; do
        pkg=$(basename "$(dirname "$file")")
        names=$(grep -h '^License:' "$file" | sed 's/^License: *//' | sort -u | tr '\n' ';')
        if [ -n "$names" ]; then
            echo "      $pkg: $names"
        else
            echo "      $pkg (no License: fields): $(grep -i -m4 -e 'licen[cs]e' -e 'permission is hereby' "$file" | tr -s ' ' | tr '\n' '|')"
        fi
    done
else
    fail "usr/share/doc has no copyright file of the bundled system libraries"
fi

# --- QML -------------------------------------------------------------------
for module in QtCore QtQuick/Controls QtQuick/Controls/Basic QtQuick/Layouts; do
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
