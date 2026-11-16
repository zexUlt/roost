#!/usr/bin/env bash
# The plugin link gate (design.md §5): built-in plugins use the public API
# only, so a target under plugins/ may link Qt and its own targets and may not
# include from or above src/ nor compile its files. cmake/PluginGate.cmake
# checks that at configure time.
#
# tests/gates/plugin_links/ holds one tiny CMake project per case. They need
# no Qt (Qt faked by an imported target); most need no compiler (LANGUAGES
# NONE), the two with STATIC libraries need a C compiler for plain
# LINK_LIBRARIES. Each runs the real cmake/PluginGate.cmake on its own plugins/ directory.
# "clean" must configure; every other case breaks the rule one way and must
# fail with the target and the culprit named, not with some other error. A
# gate that only checked rejection would also pass a module that rejects
# everything; the clean case rules that out.
#
# Usage: tools/check_plugin_links.sh
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
fixtures=$root/tests/gates/plugin_links

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

failed=0

# check <case> <expected exit: pass|fail> [expected text]...
check() {
    local name=$1 expect=$2
    shift 2
    local output status=0
    output=$(cmake -S "$fixtures/$name" -B "$work/$name" \
        "-DROOST_GATE_MODULE=$root/cmake/PluginGate.cmake" 2>&1) || status=$?

    if [ "$expect" = pass ]; then
        if [ "$status" -ne 0 ]; then
            echo "FAIL $name: configure failed, it should pass:"
            printf '%s\n' "$output" | sed 's/^/    /'
            failed=1
            return
        fi
    elif [ "$status" -eq 0 ]; then
        echo "FAIL $name: configure passed, it should fail"
        failed=1
        return
    fi

    local text
    for text in "$@"; do
        if ! grep -qF -- "$text" <<<"$output"; then
            echo "FAIL $name: output lacks \"$text\":"
            printf '%s\n' "$output" | sed 's/^/    /'
            failed=1
            return
        fi
    done
    echo "ok $name"
}

check clean pass
check links_core fail "plugin_a: links roost_core" "design.md §5"
check links_core_static fail "plugin_a: links roost_core"
check alias_core fail "plugin_a: links roost::core"
check includes_src fail "plugin_a: includes" "/src/core" "design.md §5"
check genex fail "plugin_a: links" "roost_core is not Qt"
check interface_link fail "plugin_a: links roost_core"
check qml_module pass
check genex_internal fail "plugin_a: links" "TARGET_OBJECTS:roost_core" "roost_core is not Qt" \
    "plugin_b: includes"
check includes_root fail "plugin_a: includes" "plugin_b: includes" "/src/.."
check includes_symlink fail "plugin_a: includes" "/shortcut"
check sources_src fail "plugin_a: lists source" "plugin_b: lists source" "../src/core/x.cpp"

exit "$failed"
