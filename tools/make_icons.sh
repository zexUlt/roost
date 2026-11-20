#!/usr/bin/env bash
# Builds the application icon files from one source image
# (docs/design/11-build.md): packaging/macos/roost.icns and
# packaging/linux/icons/hicolor/<N>x<N>/apps/roost.png for N in 16 32 48 64
# 128 256 512. The files are committed; the build never runs this script.
# Run it by hand on a Mac (it needs `sips` and `iconutil`) when the icon
# changes, and commit the result.
#
# The source is a square PNG of 1024x1024 pixels.
#
# Usage: tools/make_icons.sh <source.png>
set -euo pipefail

src=${1:?usage: tools/make_icons.sh <source.png (1024x1024)>}
root=$(cd "$(dirname "$0")/.." && pwd -P)

if [ "$(uname -s)" != Darwin ]; then
    echo "make_icons.sh runs on macOS (sips, iconutil)" >&2
    exit 1
fi
width=$(sips -g pixelWidth "$src" | awk '/pixelWidth/ {print $2}')
height=$(sips -g pixelHeight "$src" | awk '/pixelHeight/ {print $2}')
if [ "$width" != 1024 ] || [ "$height" != 1024 ]; then
    echo "make_icons.sh: $src is ${width}x${height}, not 1024x1024" >&2
    exit 1
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/roost-icons.XXXXXX")
trap 'rm -rf "$work"' EXIT

# resize <size> <out>: a copy of the source of <size> pixels square.
resize() {
    sips -s format png -z "$1" "$1" "$src" --out "$2" >/dev/null
}

# macOS: the iconset names are <size>x<size>[@2x].png, the @2x being twice the
# pixels of the name.
iconset=$work/roost.iconset
mkdir "$iconset"
for size in 16 32 128 256 512; do
    resize "$size" "$iconset/icon_${size}x${size}.png"
    resize $((size * 2)) "$iconset/icon_${size}x${size}@2x.png"
done
iconutil -c icns -o "$root/packaging/macos/roost.icns" "$iconset"

# Linux: the hicolor theme.
for size in 16 32 48 64 128 256 512; do
    dir=$root/packaging/linux/icons/hicolor/${size}x${size}/apps
    mkdir -p "$dir"
    resize "$size" "$dir/roost.png"
done
