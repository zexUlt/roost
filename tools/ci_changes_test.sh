#!/usr/bin/env bash
# Holds tools/ci_changes.sh to a table. A wrong answer here is a pull request
# that merges untested, so the table checks both ways.
#
# Usage: tools/ci_changes_test.sh
set -uo pipefail

classify="$(cd "$(dirname "$0")" && pwd)/ci_changes.sh"
failures=0

# expect "<paths, comma-separated>" "<true|false>"
expect() {
    local paths=$1 want="code=$2" got
    got=$(printf '%s\n' "${paths//,/$'\n'}" | "$classify")
    if [ "$got" != "$want" ]; then
        printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$paths" "$want" "$got"
        failures=$((failures + 1))
    fi
}

expect "" false
expect "docs/design.md" false
expect "README.md,CLAUDE.md,.claude/rules/threads.md,docs/prior-art.md" false
expect "plugins/claude/README.md" false

expect "src/core/rpc.cpp" true
expect "src/qml_api/Main.qml" true
expect "tests/CMakeLists.txt" true
expect "CMakeLists.txt" true
expect "CMakePresets.json" true
expect ".github/workflows/ci.yml" true
expect ".github/actions/changes/action.yml" true
expect "tools/ci_changes.sh" true
expect ".clang-tidy" true
expect ".clang-format" true
expect "plugins/claude/plugin.json" true
expect "docs/design.md,src/core/rpc.cpp" true

if [ "$failures" -ne 0 ]; then
    echo "$failures case(s) failed" >&2
    exit 1
fi
echo "ok"
