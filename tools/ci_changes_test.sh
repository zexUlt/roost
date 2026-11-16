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

# expect "<paths, comma-separated>" "<true|false>"
expect() {
    local paths=$1 want="code=$2" got
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

# The BASE form on a real history. A git hook running this test exports
# these, and they would point every git call below at the hook's repository.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE
repo=$(mktemp -d)
trap 'rm -rf "$repo"' EXIT
git_() { git -C "$repo" -c user.name=t -c user.email=t@t -c commit.gpgsign=false "$@"; }

# expect_base <name> <base> <true|false>: stdout, which goes to
# $GITHUB_OUTPUT, is that one line and nothing else.
expect_base() {
    local got
    got=$(cd "$repo" && "$classify" "$2" 2>/dev/null)
    [ "$got" = "code=$3" ] || fail "$1" "code=$3" "$got"
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
