#!/usr/bin/env bash
# Whether CI needs to build and test a change: the changed paths on stdin, one
# per line; `code=true|false` on stdout.
#
# A script rather than a paths filter in YAML: the CI jobs are required
# checks, a workflow filtered out by path never reports them and leaves the
# pull request unmergeable, while a skipped job counts as passed. And a
# script can be held to a table (tools/ci_changes_test.sh).
#
# Documentation (docs/, .claude/, *.md) needs nothing. Any other path builds
# and tests everything: a path nobody thought about is not safe to skip.
#
# Usage: git diff --name-only BASE...HEAD | tools/ci_changes.sh
set -uo pipefail

code=false
while IFS= read -r path; do
    [ -n "$path" ] || continue
    case "$path" in
        # A Markdown file anywhere is never code.
        docs/* | .claude/* | *.md) ;;
        *) code=true ;;
    esac
done

printf 'code=%s\n' "$code"
