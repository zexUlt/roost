#!/usr/bin/env bash
# Holds tools/ci_changes.sh to a table. A wrong answer here is a pull request
# that merges untested, so the table checks both ways.
#
# Usage: tools/ci_changes_test.sh
set -uo pipefail

classify="$(cd "$(dirname "$0")" && pwd)/ci_changes.sh"
failures=0

fail() {
    printf 'FAIL %s\n  want: %s\n  got:  %s\n' "$1" "$2" "$3"
    failures=$((failures + 1))
}

# expect "<paths, comma-separated>" "<code true|false>" [<tests_files>]
# Without tests_files the change is not tests_only.
expect() {
    local paths=$1 want got only=false
    [ -z "${3:-}" ] || only=true
    want=$(printf 'code=%s\ntests_only=%s\ntests_files=%s' "$2" "$only" "${3:-}")
    got=$(printf '%s\n' "${paths//,/$'\n'}" | "$classify")
    [ "$got" = "$want" ] || fail "$paths" "$want" "$got"
}

expect "" false
expect "docs/design.md" false
expect "docs/diagram.png" false
expect ".claude/settings.json" false
expect "README.md" false
expect "plugins/claude/README.md" false
expect "README.md,CLAUDE.md,.claude/rules/threads.md,docs/prior-art.md" false

expect "src/core/rpc.cpp" true
expect "src/qml_api/Main.qml" true
expect "src/docs/a.cpp" true
expect "docs.cpp" true
expect "src/x.md.in" true
expect "README.MD" true
expect "src/a b.cpp" true
expect "tests/CMakeLists.txt" true
expect "CMakeLists.txt" true
expect "CMakePresets.json" true
expect ".github/workflows/ci.yml" true
expect ".github/actions/changes/action.yml" true
expect "tools/ci_changes.sh" true
expect ".clang-tidy" true
expect "plugins/claude/plugin.json" true
expect "docs/design.md,src/core/rpc.cpp" true

# Test sources only: the build stays whole, the tests narrow.
expect "tests/roost/terminal_font_keys_test.cpp" true "tests/roost/terminal_font_keys_test.cpp"
expect "tests/core/a_test.cpp,tests/qml_api/b_test.cpp" true "tests/core/a_test.cpp tests/qml_api/b_test.cpp"
expect "tests/core/a_test.cpp,docs/design.md,README.md" true "tests/core/a_test.cpp"
expect "tests/bench/terminal_memory.cpp" true "tests/bench/terminal_memory.cpp"
# Anything else with them is a full run.
expect "tests/core/a_test.cpp,tests/helpers/scoped_env.cpp" true
expect "tests/core/a_test.cpp,tests/CMakeLists.txt" true
expect "tests/core/a_test.cpp,tests/probes/probes.cmake" true
expect "tests/core/a_test.cpp,src/core/rpc.cpp" true
expect "tests/core/a_test.cpp,.github/workflows/ci.yml" true
expect "tests/core/a_test.cpp,tools/ci_select_tests.sh" true
expect "tests/helpers/scoped_env.cpp" true
expect "tests/helpers/window_teardown.hpp" true
expect "tests/core/a_test.hpp" true
expect "tests/api/api_test_support.hpp" true
expect "tests/CMakeLists.txt" true
expect "tests/probes/probes.cmake" true
expect "tests/probes/p_test.cpp" true
expect "tests/gates/naming/bad.cpp" true
expect "tests/fixtures/claude_agents.json" true
expect "tests/fixtures/x.cpp" true
expect "tests/qml_api/tst_panel.qml" true
expect "tests/qml_api/data.json" true
expect "tests/core/a_test.cmake" true
expect "tests/core/a b_test.cpp" true
expect "tests/core/a_test.cpp.in" true
expect "tests/core/a_test.CPP" true
expect "mytests/core/a_test.cpp" true
expect "src/tests/a_test.cpp" true

# The BASE form on a real history. A git hook running this test exports
# these, and they would point every git call below at the hook's repository.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE
repo=$(mktemp -d)
trap 'rm -rf "$repo"' EXIT
git_() { git -C "$repo" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }

# expect_base <name> <base> <true|false> [<tests_files>]: stdout, which goes
# to $GITHUB_OUTPUT, is those three lines and nothing else.
expect_base() {
    local got want only=false
    [ -z "${4:-}" ] || only=true
    want=$(printf 'code=%s\ntests_only=%s\ntests_files=%s' "$3" "$only" "${4:-}")
    got=$(cd "$repo" && "$classify" "$2" 2>/dev/null)
    [ "$got" = "$want" ] || fail "$1" "$want" "$got"
}

git_ init -q -b trunk
mkdir -p "$repo/src" "$repo/docs"
echo 'int main() {}' > "$repo/src/a.cpp"
echo 'cmake_minimum_required(VERSION 3.28)' > "$repo/CMakeLists.txt"
echo 'notes' > "$repo/docs/notes.md"
git_ add -A && git_ commit -q -m base
base=$(git_ rev-parse HEAD)

# Moving code into documentation still changes code.
git_ checkout -q -b move
git_ mv src/a.cpp docs/a.md
git_ mv CMakeLists.txt NOTES.md
git_ commit -q -m move
expect_base "rename src/a.cpp -> docs/a.md" "$base" true

# BASE...HEAD: what trunk did after the branch point is not this change.
git_ checkout -q trunk
echo 'int main() { return 0; }' > "$repo/src/a.cpp"
git_ commit -q -am trunk-moves-on
git_ checkout -q -b docs-only "$base"
echo 'more notes' >> "$repo/docs/notes.md"
git_ commit -q -am docs
expect_base "docs on a branch behind trunk" trunk false

# Test sources on a branch: the BASE form lists them too.
git_ checkout -q -b tests-only "$base"
mkdir -p "$repo/tests/core"
echo 'int main() {}' > "$repo/tests/core/a_test.cpp"
echo 'more notes' >> "$repo/docs/notes.md"
git_ add -A && git_ commit -q -m tests
expect_base "a test source and docs" "$base" true tests/core/a_test.cpp
echo 'int main() { return 1; }' > "$repo/src/a.cpp"
git_ commit -q -am src
expect_base "a test source and a source" "$base" true

for bad in "" no-such-ref; do
    if (cd "$repo" && "$classify" "$bad" >/dev/null 2>&1); then
        fail "BASE '$bad'" "exit status 1" "exit status 0"
    fi
done

if [ "$failures" -ne 0 ]; then
    echo "$failures case(s) failed" >&2
    exit 1
fi
echo "ok"
