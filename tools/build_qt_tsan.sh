#!/usr/bin/env bash
# Builds the Qt modules roost needs with ThreadSanitizer and installs them.
# Qt from a binary distribution is not instrumented: TSan does not see the
# synchronisation inside QtCore and QtQuick, so a hand-over through Qt looks
# like a race. Tests labelled `threads` need this build (.claude/rules/threads.md).
#
# Core, Gui, Qml, Quick, Quick Controls, Network and Test live in three
# submodules, built in dependency order: qtdeclarative needs qtshadertools.
#
# Usage: tools/build_qt_tsan.sh <qt-version> <prefix>
set -euo pipefail

version=${1:?usage: tools/build_qt_tsan.sh <qt-version> <prefix>}
prefix=${2:?usage: tools/build_qt_tsan.sh <qt-version> <prefix>}

series=${version%.*}
base_url=https://download.qt.io/official_releases/qt/$series/$version/submodules
modules=(qtbase qtshadertools qtdeclarative)
jobs=$(sysctl -n hw.ncpu 2>/dev/null || nproc)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fetch() {
    local module=$1
    local name=$module-everywhere-src-$version.tar.xz
    curl --fail --location --retry 3 --silent --show-error \
        --output "$work/$name" "$base_url/$name"
    curl --fail --location --retry 3 --silent --show-error \
        --output "$work/$name.sha256" "$base_url/$name.sha256"

    # The file holds "<hash>  <name>"; only the hash is compared, so the
    # check does not depend on how the name is written.
    local expected actual
    expected=$(awk '{print tolower($1); exit}' "$work/$name.sha256")
    actual=$(shasum -a 256 "$work/$name" | awk '{print $1}')
    if [ -z "$expected" ] || [ "$expected" != "$actual" ]; then
        echo "FAIL: sha256 of $name: expected '$expected', got '$actual'" >&2
        exit 1
    fi

    tar -xf "$work/$name" -C "$work"
    rm "$work/$name" "$work/$name.sha256"
}

for module in "${modules[@]}"; do
    fetch "$module"
done

# -release -force-debug-info: optimised, but with symbols for TSan stacks.
# Without -nomake, examples and tests would be built too.
mkdir "$work/build-qtbase"
(
    cd "$work/build-qtbase"
    "$work/qtbase-everywhere-src-$version/configure" \
        -prefix "$prefix" -release -force-debug-info -sanitize thread \
        -nomake examples -nomake tests
    cmake --build . --parallel "$jobs"
    cmake --install .
)

for module in qtshadertools qtdeclarative; do
    mkdir "$work/build-$module"
    (
        cd "$work/build-$module"
        "$prefix/bin/qt-configure-module" \
            "$work/$module-everywhere-src-$version" \
            -nomake examples -nomake tests
        cmake --build . --parallel "$jobs"
        cmake --install .
    )
done
