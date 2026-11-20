#!/usr/bin/env bash
# The Linux package, an AppImage (docs/design/11-build.md): `cmake --install`
# into an AppDir, linuxdeploy with its Qt plugin copies Qt and the libraries
# in, appimagetool packs the AppDir. Prints the path of the AppImage on the
# last line.
#
# The AppImage of the arch the script runs on: x86_64 or aarch64. The tools
# are AppImages themselves, pinned by tag and sha256, and are run extracted:
# Ubuntu 24.04 has no libfuse2. The AppImage made has the static runtime of
# appimagetool, which needs no libfuse2 either.
#
# Usage: tools/package_linux.sh <build dir> <out dir>
#   <build dir>  a built tree, e.g. build/release; Qt (qmake) is on PATH
#   <out dir>    AppDir, tools and the AppImage go here
set -euo pipefail

build=${1:?usage: tools/package_linux.sh <build dir> <out dir>}
out=${2:?usage: tools/package_linux.sh <build dir> <out dir>}
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

LINUXDEPLOY_TAG=1-alpha-20251107-1
LINUXDEPLOY_QT_TAG=1-alpha-20250213-1
APPIMAGETOOL_TAG=1.9.1
# The runtime appimagetool puts in front of the squashfs: it fetches the
# floating `continuous` one by itself, so it is given this one.
RUNTIME_TAG=20251108

case $(uname -m) in
    x86_64)
        arch=x86_64
        linuxdeploy_sha256=c20cd71e3a4e3b80c3483cef793cda3f4e990aca14014d23c544ca3ce1270b4d
        linuxdeploy_qt_sha256=15106be885c1c48a021198e7e1e9a48ce9d02a86dd0a1848f00bdbf3c1c92724
        appimagetool_sha256=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0
        runtime_sha256=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d
        ;;
    aarch64 | arm64)
        arch=aarch64
        linuxdeploy_sha256=620095110d693282b8ebeb244a95b5e911cf8f65f76c88b4b47d16ae6346fcff
        linuxdeploy_qt_sha256=bf1c24aff6d749b5cf423afad6f15abd4440f81dec1aab95706b25f6667cdcf1
        appimagetool_sha256=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158
        runtime_sha256=00cbdfcf917cc6c0ff6d3347d59e0ca1f7f45a6df1a428a0d6d8a78664d87444
        ;;
    *)
        echo "package_linux.sh: no AppImage tools for $(uname -m)" >&2
        exit 1
        ;;
esac

[ -f "$build/CMakeCache.txt" ] || {
    echo "package_linux.sh: $build is not a CMake build tree" >&2
    exit 1
}
mkdir -p "$out"
out=$(cd "$out" && pwd)
build=$(cd "$build" && pwd)
appdir=$out/AppDir
tools=$out/tools
mkdir -p "$tools"

# fetch <url> <sha256> <file>: the file, checked; a cached one is reused only
# if it matches.
fetch() {
    local url=$1 sha256=$2 file=$3
    if [ -f "$file" ] && echo "$sha256  $file" | sha256sum -c --quiet - 2>/dev/null; then
        return
    fi
    curl -fsSL --retry 3 -o "$file" "$url"
    echo "$sha256  $file" | sha256sum -c --quiet - || {
        echo "package_linux.sh: $url does not match its sha256 $sha256" >&2
        exit 1
    }
}

# extract <AppImage> <dir>: runs `AppRun` of the unpacked AppImage, with no
# FUSE.
extract() {
    local file=$1 dir=$2
    rm -rf "$dir"
    mkdir -p "$dir"
    chmod +x "$file"
    (cd "$dir" && "$file" --appimage-extract >/dev/null)
}

github=https://github.com
fetch "$github/linuxdeploy/linuxdeploy/releases/download/$LINUXDEPLOY_TAG/linuxdeploy-$arch.AppImage" \
    "$linuxdeploy_sha256" "$tools/linuxdeploy.AppImage"
fetch "$github/linuxdeploy/linuxdeploy-plugin-qt/releases/download/$LINUXDEPLOY_QT_TAG/linuxdeploy-plugin-qt-$arch.AppImage" \
    "$linuxdeploy_qt_sha256" "$tools/linuxdeploy-plugin-qt.AppImage"
fetch "$github/AppImage/appimagetool/releases/download/$APPIMAGETOOL_TAG/appimagetool-$arch.AppImage" \
    "$appimagetool_sha256" "$tools/appimagetool.AppImage"
fetch "$github/AppImage/type2-runtime/releases/download/$RUNTIME_TAG/runtime-$arch" \
    "$runtime_sha256" "$tools/runtime-$arch"
extract "$tools/linuxdeploy.AppImage" "$tools/linuxdeploy"
extract "$tools/linuxdeploy-plugin-qt.AppImage" "$tools/linuxdeploy-plugin-qt"
extract "$tools/appimagetool.AppImage" "$tools/appimagetool"

# linuxdeploy finds `linuxdeploy-plugin-qt` in PATH.
mkdir -p "$tools/bin"
cat >"$tools/bin/linuxdeploy-plugin-qt" <<WRAPPER
#!/bin/sh
exec "$tools/linuxdeploy-plugin-qt/squashfs-root/AppRun" "\$@"
WRAPPER
chmod +x "$tools/bin/linuxdeploy-plugin-qt"

qmake=${QMAKE:-$(command -v qmake6 || command -v qmake || true)}
[ -n "$qmake" ] || {
    echo "package_linux.sh: no qmake in PATH (set QMAKE)" >&2
    exit 1
}
qt_libs=$("$qmake" -query QT_INSTALL_LIBS)

# The install puts the directories of the Qt it was built with into the run
# path of every binary; a package carries its own Qt, so not.
# The option goes back to what the tree had when the script ends, so a local
# release tree is left as it was.
rpath_to_qt=$(sed -n 's/^ROOST_INSTALL_RPATH_TO_QT:[A-Z]*=//p' "$build/CMakeCache.txt")
# shellcheck disable=SC2329  # runs from the EXIT trap
restore_rpath_option() {
    [ -z "$rpath_to_qt" ] || cmake -DROOST_INSTALL_RPATH_TO_QT="$rpath_to_qt" "$build" >/dev/null
}
trap restore_rpath_option EXIT
cmake -DROOST_INSTALL_RPATH_TO_QT=OFF "$build" >/dev/null
rm -rf "$appdir"
cmake --install "$build" --prefix "$appdir/usr" --component roost >/dev/null

# linuxdeploy-plugin-qt scans these for the QML imports to copy: the window,
# and the QML of the plugins (QtCore, for one, is imported by SessionsPanel.qml
# only). RoostSessions is not found there on purpose: it is the plugin's own,
# already in lib/roost/plugins/sessions/qml.
qml_sources=$root/src/roost
for dir in "$root"/plugins/*/qml; do
    qml_sources=$qml_sources:$dir
done

# The libraries of every desktop (packaging/linux/system-libraries.txt) are
# not carried: --exclude-library for each of them.
excludes=()
while IFS= read -r pattern; do
    excludes+=(--exclude-library "$pattern")
done < <(grep -v -e '^#' -e '^$' "$root/packaging/linux/system-libraries.txt")

# NO_STRIP: the Qt libraries are stripped, and the strip of the runner chokes
# on what newer linkers write. The libraries of the Qt of the build are in
# LD_LIBRARY_PATH for the ldd inside linuxdeploy: with no Qt run path nothing
# else leads to them.
(
    export PATH="$tools/bin:$PATH"
    export QMAKE=$qmake
    export QML_SOURCES_PATHS=$qml_sources
    export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
    export NO_STRIP=1
    export LD_LIBRARY_PATH=$qt_libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
    "$tools/linuxdeploy/squashfs-root/AppRun" \
        --appdir "$appdir" \
        --plugin qt \
        --executable "$appdir/usr/bin/roost" \
        --executable "$appdir/usr/bin/roostd" \
        --executable "$appdir/usr/lib/roost/plugins/claude/backend/claude-adapter" \
        --library "$appdir/usr/lib/roost/plugins/sessions/qml/RoostSessions/libsessions_qml.so" \
        --desktop-file "$appdir/usr/share/applications/roost.desktop" \
        --icon-file "$appdir/usr/share/icons/hicolor/256x256/apps/roost.png" \
        --custom-apprun "$root/packaging/linux/AppRun" \
        "${excludes[@]}"
) || {
    echo "package_linux.sh: linuxdeploy failed; the AppDir:" >&2
    (cd "$appdir" && find . | sort) >&2
    exit 1
}

# --library puts a copy of the module into usr/lib for linuxdeploy to follow
# its dependencies; the window loads the one in lib/roost/plugins/sessions.
rm -f "$appdir/usr/lib/libsessions_qml.so"

# linuxdeploy-plugin-qt copies the libraries of the xcb platform plugin with
# no regard for --exclude-library: what is on the list goes out here, and is
# named, to see that the list is needed.
while IFS= read -r pattern; do
    while IFS= read -r -d '' lib; do
        echo "package_linux.sh: removing ${lib#"$appdir"/}, a library of every desktop" >&2
        rm -f "$lib"
    done < <(find "$appdir/usr/lib" -maxdepth 1 -name "$pattern" -print0)
done < <(grep -v -e '^#' -e '^$' "$root/packaging/linux/system-libraries.txt")

# The QML modules and Qt plugins that roost does not use. The QML scan of
# linuxdeploy-plugin-qt copies the whole of every module it meets, with all
# styles of Qt Quick Controls and what they import, and there is no knob to
# stop it. The window runs Fusion (what Qt Quick Controls picks on Linux; the
# smoke fails without QtQuick.Controls.Fusion) and Basic (its base), with
# Layouts and Templates; the other styles, Dialogs, Effects, Particles,
# Shapes, VectorImage, LocalStorage, XmlListModel and DesignSupport are
# imported by no QML of roost, Fusion or Basic. The image formats but qsvg (the
# agent icons of the sessions list are SVG) and the TLS backends: roost makes no
# TLS connection. The Qt libraries that only these needed go below.
unused=(
    usr/qml/QtQuick/Controls/{Imagine,Material,Universal,FluentWinUI3,Native,designer,iOS,macOS}
    usr/qml/QtQuick/{Dialogs,Effects,Particles,Shapes,VectorImage,LocalStorage,tooling}
    usr/qml/QtQml/{DesignSupport,XmlListModel}
    usr/plugins/tls
)
for path in "${unused[@]}"; do
    rm -rf "${appdir:?}/$path"
done
find "$appdir/usr/plugins/imageformats" -type f ! -name 'libqsvg.so' -delete 2>/dev/null || true

# A Qt library that no ELF left in the AppDir lists as needed is dead weight
# now (the style, Dialogs, VectorImage, Sql libraries): off, until
# nothing is left to go. Nothing loads a Qt library by name.
needed() {
    find "$appdir" -type f \( -name '*.so' -o -name '*.so.*' -o -perm -u+x \) -print0 |
        { xargs -0 -r readelf -d 2>/dev/null || true; } |
        sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort -u
}
while :; do
    needed >"$out/needed.txt"
    # No NEEDED at all: readelf is missing or failing, and every Qt library
    # would look dead.
    [ -s "$out/needed.txt" ] || {
        echo "package_linux.sh: readelf found no NEEDED entry in the AppDir" >&2
        exit 1
    }
    removed=0
    for lib in "$appdir"/usr/lib/libQt6*.so*; do
        [ -e "$lib" ] || continue
        name=$(basename "$lib")
        soname=$({ readelf -d "$lib" 2>/dev/null || true; } | sed -n 's/.*(SONAME).*\[\(.*\)\]/\1/p')
        if ! grep -qxF "${soname:-$name}" "$out/needed.txt"; then
            echo "package_linux.sh: removing $name, nothing needs it" >&2
            rm -f "$lib"
            removed=1
        fi
    done
    [ "$removed" = 1 ] || break
done
rm -f "$out/needed.txt"

# linuxdeploy copied the Debian copyright file of every library it copied,
# those of the libraries that are not carried now too: only the packages that
# have a library left in usr/lib keep theirs.
for doc in "$appdir"/usr/share/doc/*/; do
    pkg=$(basename "$doc")
    keep=0
    while IFS= read -r file; do
        [ -e "$appdir/usr/lib/$(basename "$file")" ] && keep=1
    done < <(dpkg -L "$pkg" 2>/dev/null | grep '\.so')
    [ "$keep" = 1 ] || rm -rf "${doc:?}"
done

# The text of the ICU licence, next to the others (the Qt of the image links
# ICU as separate libraries; the macOS bundle has none). Committed, not
# fetched: a licence text needs no network and is reviewed in the diff.
install -m 644 "$root/licenses/ICU-LICENSE.txt" "$appdir/usr/share/licenses/roost/ICU-LICENSE.txt"

image=$out/roost-$arch.AppImage
rm -f "$image"
# --no-appstream: there is no AppStream metadata yet.
# --runtime-file: the pinned runtime, so appimagetool downloads nothing.
ARCH=$arch "$tools/appimagetool/squashfs-root/AppRun" --no-appstream \
    --runtime-file "$tools/runtime-$arch" "$appdir" "$image" >&2
[ -x "$image" ] || chmod +x "$image"
echo "$image"
