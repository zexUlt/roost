#!/usr/bin/env bash
# Qt is linked dynamically (design.md §10): LGPLv3 lets the user replace the Qt
# libraries, and a static Qt would take that away. Each binary must load every
# Qt module it uses as a shared library; a statically linked module would not
# appear in the list of libraries the binary loads.
#
# Usage: tools/check_qt_dynamic.sh build/dev
set -uo pipefail

build=${1:?usage: tools/check_qt_dynamic.sh <build dir>}
failed=0

check() {
    local binary=$1
    shift
    local path=$build/bin/$binary
    if [ ! -x "$path" ]; then
        echo "FAIL: $path was not built"
        failed=1
        return
    fi

    local libs
    case $(uname -s) in
        Darwin) libs=$(otool -L "$path") ;;
        *) libs=$(ldd "$path") ;;
    esac

    local module
    for module in "$@"; do
        # QtCore.framework on macOS, libQt6Core.so.6 on Linux.
        if grep -qE "Qt${module}\.framework|libQt6${module}\.so" <<<"$libs"; then
            echo "ok:   $binary loads Qt$module dynamically"
        else
            echo "FAIL: $binary does not load Qt$module dynamically:"
            printf '%s\n' "$libs" | sed 's/^/    /'
            failed=1
        fi
    done
}

# The desktop notifications of Linux go over QtDBus (src/roost/desktop_notifier_linux.cpp).
case $(uname -s) in
    Linux) check roost Core Gui Qml Quick DBus ;;
    *) check roost Core Gui Qml Quick ;;
esac
check roostd Core
check ghostty_formatter_test Core Test

exit "$failed"
