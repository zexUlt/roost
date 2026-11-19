#!/usr/bin/env bash
# The macOS package: roost.app and a .dmg of it (docs/design/11-build.md).
#
#   1. cmake --install of the build into a staging directory (cmake/Install.cmake)
#   2. macdeployqt copies the Qt frameworks and plugins into roost.app and
#      the QML modules the window and the plugins import into
#      Contents/Resources/qml
#   3. ad-hoc codesign, inside out
#   4. hdiutil: roost.app, a link to /Applications and the licences
#
# `macdeployqt` and `qmake` come from the PATH: the Qt the build was configured
# with must be the first Qt there (install-qt-action puts it there). The last
# line printed is the path of the .dmg. tools/check_bundle.sh checks the app
# and the .dmg.
#
# Usage: tools/package_macos.sh <build dir> <out dir>
set -euo pipefail

build=${1:?usage: tools/package_macos.sh <build dir> <out dir>}
out=${2:?usage: tools/package_macos.sh <build dir> <out dir>}
root=$(cd "$(dirname "$0")/.." && pwd -P)

if [ "$(uname -s)" != Darwin ]; then
    echo "package_macos.sh runs on macOS" >&2
    exit 1
fi
build=$(cd "$build" && pwd -P)
mkdir -p "$out"
out=$(cd "$out" && pwd -P)

stage=$out/stage
dmg_root=$out/dmg-root
rm -rf "$stage" "$dmg_root"
mkdir -p "$stage" "$dmg_root"

echo "== install"
cmake --install "$build" --prefix "$stage" --component roost >/dev/null
app=$stage/roost.app
contents=$app/Contents

is_macho() {
    file -b "$1" | grep -q '^Mach-O'
}

# macdeployqt deploys the platform plugin `cocoa` only. The smoke runs of
# tools/check_bundle.sh (and of the CI jobs that start the window with no
# display) need `offscreen`, which is a small plugin of the same Qt: put it
# where macdeployqt looks, so that it fixes its references like the others.
plugins=$(qmake -query QT_INSTALL_PLUGINS)
mkdir -p "$contents/PlugIns/platforms"
cp "$plugins/platforms/libqoffscreen.dylib" "$contents/PlugIns/platforms/"
chmod u+w "$contents/PlugIns/platforms/libqoffscreen.dylib"

# The code that is not the window: macdeployqt is told to treat each as an
# executable of the bundle (it fixes its Qt references and deploys what it
# needs), as it does the main one.
executable_args=()
while IFS= read -r -d '' file; do
    if is_macho "$file" && [ "$file" != "$contents/MacOS/roost" ]; then
        executable_args+=("-executable=$file")
    fi
done < <(find "$contents/MacOS" "$contents/PlugIns" -type f -print0)

# QML scanned for imports: the window's and each plugin's. RoostSessions,
# which a plugin carries itself, is found through the plugin's own directory,
# so it is not passed as -qmlimport (that would copy it a second time into
# Resources/qml).
qmldir_args=("-qmldir=$root/src/roost")
for dir in "$contents"/Resources/roost-plugins/*/qml; do
    if [ -d "$dir" ]; then
        qmldir_args+=("-qmldir=$dir")
    fi
done

echo "== macdeployqt"
macdeployqt "$app" -verbose=1 "${executable_args[@]}" "${qmldir_args[@]}"

# macdeployqt copies every SQL driver of the Qt (QtSql comes with the QML module
# QtQuick.LocalStorage, which the window does not import). The drivers lean on
# database client libraries of the build machine (PostgreSQL, ODBC, Mimer) that
# are not in the bundle and that roost has no use for.
rm -rf "$contents/PlugIns/sqldrivers"

# The install sets the run path of every binary from the Qt of the build
# (ROOST_INSTALL_RPATH_TO_QT), and macdeployqt finds the frameworks to copy
# through it. The bundle carries its own Qt now: a path to the build machine's
# one (and to the libraries a Qt of Homebrew leans on) would let a missing
# library hide behind it.
while IFS= read -r -d '' file; do
    if is_macho "$file"; then
        for runpath in $(otool -l "$file" |
            awk '/cmd LC_RPATH/ {r = 1} r && $1 == "path" {print $2; r = 0}'); do
            case $runpath in
                @*) ;;
                *) install_name_tool -delete_rpath "$runpath" "$file" ;;
            esac
        done
    fi
done < <(find "$app" -type f -print0)

# Inside out: a signature of a container covers what is in it, so each part is
# signed before what holds it. Loose code first (plugins, helpers), then the
# frameworks, then the app. --force replaces the signatures that macdeployqt or
# install_name_tool left invalid; there is no identity ("-" is ad hoc), so
# this is not a Developer ID signature and Gatekeeper still asks.
echo "== codesign"
sign() {
    codesign --force --sign - --timestamp=none "$@" 2>&1 | { grep -v "replacing existing signature" || true; }
}
while IFS= read -r -d '' file; do
    # Code inside a framework is signed with the framework.
    case $file in
        *.framework/*) continue ;;
    esac
    if [ "$file" != "$contents/MacOS/roost" ] && is_macho "$file"; then
        sign "$file"
    fi
done < <(find "$app" -type f -not -path '*/Contents/Frameworks/*' -print0)
for framework in "$contents"/Frameworks/*.framework; do
    if [ -d "$framework" ]; then
        sign "$framework"
    fi
done
# Frameworks' loose dylibs, if any (not in a .framework).
while IFS= read -r -d '' file; do
    if is_macho "$file"; then
        sign "$file"
    fi
done < <(find "$contents/Frameworks" -maxdepth 1 -type f -print0)
sign "$app"
codesign --verify --deep --strict "$app"

echo "== dmg"
version=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
    "$contents/Info.plist")
arch=$(uname -m)
dmg=$out/roost-$version-macos-$arch.dmg
# ditto keeps the symlinks, the signature and the extended attributes.
ditto "$app" "$dmg_root/roost.app"
ln -s /Applications "$dmg_root/Applications"
ditto "$contents/Resources/licenses" "$dmg_root/Licenses"
# hdiutil create now and then fails with "Resource busy" on a CI runner: try
# again, from no partial image.
created=
for attempt in 1 2 3; do
    rm -f "$dmg"
    if hdiutil create -quiet -volname roost -srcfolder "$dmg_root" -fs HFS+ \
        -format UDZO -ov "$dmg"; then
        created=1
        break
    fi
    echo "hdiutil create failed (attempt $attempt of 3)" >&2
    sleep 5
done
if [ -z "$created" ]; then
    rm -f "$dmg"
    exit 1
fi
rm -rf "$dmg_root"

echo "$dmg"
