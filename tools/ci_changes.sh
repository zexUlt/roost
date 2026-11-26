#!/usr/bin/env bash
# Whether CI needs to build and test a change, on stdout:
#   code=true|false         a build is needed
#   tests_only=true|false   the change is test sources only: CI builds all,
#                           but runs only the tests of those files
#   tests_files=<paths>     those files, separated by spaces (empty if not)
#
# A script rather than a paths filter in YAML: the CI jobs are required
# checks, a workflow filtered out by path never reports them and leaves the
# pull request unmergeable, while a skipped job counts as passed. And a
# script can be held to a table (tools/ci_changes_test.sh).
#
# Documentation (anything under docs/ or .claude/, *.md anywhere) needs
# nothing. Any other path builds and tests everything: a path nobody thought
# about is not safe to skip.
#
# A change whose every non-documentation path is a test source (a .cpp under
# tests/, apart from tests/helpers/, tests/fixtures/, tests/gates/ and
# tests/probes/, which are shared code or data) is tests_only. The path must
# be plain (letters, digits, . _ / -): a name that needs quoting, or one nobody
# thought about, is a full run. tools/ci_select_tests.sh maps the files to
# ctest tests after configure.
#
# Usage: tools/ci_changes.sh BASE   the change from BASE to HEAD
#        tools/ci_changes.sh        the changed paths on stdin, one per line
set -uo pipefail

classify() {
    local path code=false tests_only=true files=""
    while IFS= read -r path; do
        [ -n "$path" ] || continue
        case "$path" in
            docs/* | .claude/* | *.md) continue ;;
        esac
        code=true
        case "$path" in
            tests/helpers/* | tests/fixtures/* | tests/gates/* | tests/probes/*)
                tests_only=false ;;
            tests/*.cpp)
                if [[ "$path" =~ ^[A-Za-z0-9_./-]+$ ]]; then
                    files="${files:+$files }$path"
                else
                    tests_only=false
                fi ;;
            *) tests_only=false ;;
        esac
    done
    [ "$code" = true ] || tests_only=false
    [ "$tests_only" = true ] || files=""
    printf 'code=%s\ntests_only=%s\ntests_files=%s\n' "$code" "$tests_only" "$files"
}

if [ $# -eq 0 ]; then
    classify
    exit 0
fi

# An empty BASE would diff HEAD with itself and skip everything.
if ! git rev-parse --verify -q "$1^{commit}" >/dev/null; then
    echo "ci_changes: BASE '$1' is not a commit" >&2
    exit 1
fi
# --no-renames: a rename is listed by its new name only, so moving
# src/a.cpp to docs/a.md would look like a change to documentation.
if ! changed=$(git diff --name-only --no-renames "$1"...HEAD); then
    echo "ci_changes: git diff $1...HEAD failed" >&2
    exit 1
fi
printf '%s\n' "$changed" >&2
printf '%s\n' "$changed" | classify
