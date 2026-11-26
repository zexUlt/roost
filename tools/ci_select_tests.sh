#!/usr/bin/env bash
# The ctest tests that cover the given test sources: a regex for `ctest -R` on
# stdout, `^(a|b)$`. Fails (status 1, the reason on stderr) when it cannot say,
# and then the caller runs the whole suite.
#
# From the build system, not from the text of CMakeLists.txt: the CMake File
# API (codemodel-v2) gives target -> sources, `ctest --show-only=json-v1` gives
# test -> command. A test is selected when its command runs the executable of
# a target that has one of the files among its sources. When that executable
# is used by anything else — another target depends on it, a define or a test
# property names its path (a test that runs another test's binary as a fake,
# e.g. FAKE_CODEX_PATH) — the answer is unknown: the whole suite runs.
#
# The build directory must have been configured with the query file in place:
#   mkdir -p BUILD/.cmake/api/v1/query && touch BUILD/.cmake/api/v1/query/codemodel-v2
# and built, whole: ctest leaves out the command of a test whose executable is
# not there yet, and then the answer is unknown.
#
# Usage: tools/ci_select_tests.sh BUILD_DIR FILE...   (FILE relative to the repository root)
set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: ci_select_tests.sh BUILD_DIR FILE..." >&2
    exit 1
fi
build=$(cd "$1" && pwd)
shift

die() {
    echo "ci_select_tests: $*" >&2
    exit 1
}

reply="$build/.cmake/api/v1/reply"
# Index files are named by time, the last one is the current reply.
index=$(find "$reply" -maxdepth 1 -name 'index-*.json' 2>/dev/null | sort | tail -n 1 || true)
[ -n "$index" ] || die "no File API reply in $reply (the query file was not there before configure?)"
codemodel="$reply/$(jq -er '.reply["codemodel-v2"].jsonFile' "$index")" ||
    die "the File API reply has no codemodel-v2"

tests=$(ctest --test-dir "$build" --show-only=json-v1 | jq -c '.tests') ||
    die "ctest --show-only failed"
missing=$(jq -r '[.[] | select(.command == null) | .name] | join(" ")' <<<"$tests")
[ -z "$missing" ] || die "no command for: $missing (not built yet?)"

# One line per target: its type, the executable it makes (absolute) and its
# sources (relative to the source directory), as JSON.
targets=$(
    jq -r '.configurations[0].targets[].jsonFile' "$codemodel" | while IFS= read -r file; do
        jq -c --arg build "$build" '{
            id, name, type,
            deps: [(.dependencies // [])[].id],
            defines: [(.compileGroups // [])[] | (.defines // [])[] | .define],
            exe: ((.artifacts // [])[0].path // "" | if . == "" then "" else $build + "/" + . end),
            sources: [(.sources // [])[] | select(.isGenerated | not) | .path]
        }' "$reply/$file"
    done | jq -s '.'
)

selected=""
for file in "$@"; do
    # Every target that compiles the file must be a test executable: a
    # library may link into other tests than the ones found.
    owners=$(jq -c --arg f "$file" '[.[] | select(.sources | index($f))]' <<<"$targets")
    [ "$(jq 'length' <<<"$owners")" -gt 0 ] || die "$file is a source of no target"
    if [ "$(jq '[.[] | select(.type != "EXECUTABLE")] | length' <<<"$owners")" -gt 0 ]; then
        die "$file is compiled into a library, not only into executables"
    fi
    # The executable used elsewhere: a dependency of another target, or its
    # path in a define or in a test property (ENVIRONMENT and the like).
    users=$(jq -r --argjson owners "$owners" '
        [ .[] | . as $t | select(any($owners[]; . as $o | $o.id != $t.id and
            (any($t.deps[]; . == $o.id) or ($o.exe != "" and any($t.defines[]; contains($o.exe)))))) | .name ]
        | join(" ")' <<<"$targets")
    [ -z "$users" ] || die "$file builds an executable that other targets use: $users"
    prop_users=$(jq -r --argjson owners "$owners" '
        [ .[] | select(any(.properties[]?.value | tostring; . as $v | any($owners[]; . as $o | $o.exe != "" and ($v | contains($o.exe))))) | .name ]
        | join(" ")' <<<"$tests")
    [ -z "$prop_users" ] || die "$file builds an executable that the properties of tests name: $prop_users"
    names=$(jq -r --argjson owners "$owners" '
        [ .[] | select(.command | any(. as $word | $owners | any(.exe == $word))) | .name ]
        | .[]' <<<"$tests")
    [ -n "$names" ] || die "$file is a source of no ctest test"
    echo "ci_select_tests: $file -> $(tr '\n' ' ' <<<"$names")" >&2
    selected+="$names"$'\n'
done

list=$(printf '%s' "$selected" | sort -u | paste -sd '|' -)
# The names go into a regex: only plain ones.
if [[ ! "$list" =~ ^[A-Za-z0-9_.|-]+$ ]]; then
    die "a test name needs escaping: $list"
fi
printf '^(%s)$\n' "$list"
