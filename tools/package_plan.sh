#!/usr/bin/env bash
# The plan of a package.yml run: `key=value` lines for $GITHUB_OUTPUT, so that
# the job `changes` calls it and a person runs it locally against any commit.
#
# Outputs
#   needed   the packaging jobs run (true | false)
#   release  this run tags a release and makes its draft (true | false)
#   version  the version of the app of this run, empty when none is planned
#   patch    its patch, passed to the build as -DROOST_VERSION_PATCH
#   last     the newest v* tag reachable from HEAD (release runs; may be empty)
#
# Input, from the environment
#   EVENT    schedule | push | workflow_dispatch (github.event_name)
#   REF      github.ref: refs/tags/vX.Y.Z on a tag, refs/heads/trunk otherwise
#   RELEASE  the dispatch input `release` (true | false)
#   DRAFT    the dispatch input `draft_release` (true | false)
#   BASE     the head of the last other run of the workflow on trunk, or empty
#   HEAD_REV the commit to plan for, default HEAD
#
# The rules
#   - A tag vX.Y.Z is the version: X.Y must be the MAJOR.MINOR of CMakeLists.txt,
#     the patch goes to the build. Always packaged.
#   - The night (schedule) and a dispatch with release=true plan a release: it
#     is made when some product change lies between the newest v* tag reachable
#     from HEAD and HEAD, and HEAD has no v* tag yet (no tag at all: made). A
#     product change is any file outside docs/, tests/, .claude/, .github/ and
#     *.md. Its version is MAJOR.MINOR of CMakeLists.txt and the patch is one
#     more than the highest patch among all the tags vMAJOR.MINOR.* (0 if none).
#   - A dispatch with draft_release=true (the dry run) plans that version too,
#     makes no release. release and draft_release together fail.
#   - Only the night is ever skipped: without a release it is skipped when no
#     file but documentation and its tooling (docs/, .claude/, *.md, issue
#     templates) changed since BASE, whatever the run at BASE ended with. A
#     release makes it needed anyway: a red night followed by a docs-only day
#     must not leave the product change without a release.
#   - Any other event is packaged, with no version planned (the patch is 0).
#
# Usage: EVENT=schedule BASE=<sha> tools/package_plan.sh
set -euo pipefail

event=${EVENT:?EVENT is required (schedule, push or workflow_dispatch)}
ref=${REF:-refs/heads/trunk}
release_input=${RELEASE:-false}
draft_input=${DRAFT:-false}
base=${BASE:-}
head_rev=${HEAD_REV:-HEAD}

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
git_() { git -C "$root" "$@"; }

fail() {
    echo "::error::$*" >&2
    exit 1
}

# What the project is called: the MAJOR.MINOR of CMakeLists.txt.
cmake_version=$(sed -n -E 's/^[[:space:]]*VERSION[[:space:]]+([0-9]+\.[0-9]+)\..*/\1/p' "$root/CMakeLists.txt" | head -n 1)
[ -n "$cmake_version" ] || fail "no VERSION MAJOR.MINOR.<patch> in CMakeLists.txt"
major=${cmake_version%.*}
minor=${cmake_version#*.}

head=$(git_ rev-parse "$head_rev^{commit}")

needed=true
release=false
version=
patch=
last=

is_product_change() {
    # Succeeds when a file changed between $1 and $2 is not documentation, tests
    # or tooling of the repository. The list is read whole first: `grep -q`
    # stops at the first match, and under pipefail git, cut off by SIGPIPE on a
    # long list, would turn the match into a failure.
    local files
    files=$(git_ diff --no-renames --name-only "$1" "$2")
    [ -n "$files" ] && grep -q -v -E '^(docs/|tests/|\.claude/|\.github/)|\.md$' <<<"$files"
}

plan_release_version() {
    # The highest patch of all the tags vMAJOR.MINOR.*, plus one; 0 without a tag.
    local highest=-1 tag n
    while IFS= read -r tag; do
        n=${tag##*.}
        if [ "$n" -gt "$highest" ]; then highest=$n; fi
    done < <(git_ tag --list "v$major.$minor.*" | grep -E "^v$major\.$minor\.(0|[1-9][0-9]*)$" || true)
    patch=$((highest + 1))
    version="$major.$minor.$patch"
}

if [[ "$ref" == refs/tags/v* ]]; then
    tag=${ref#refs/tags/}
    [[ "$tag" =~ ^v([0-9]+)\.([0-9]+)\.(0|[1-9][0-9]*)$ ]] ||
        fail "the tag $tag is not vMAJOR.MINOR.PATCH"
    [ "${BASH_REMATCH[1]}.${BASH_REMATCH[2]}" = "$cmake_version" ] ||
        fail "the tag $tag is not of the version $cmake_version.x of CMakeLists.txt: raise the minor in a pull request first"
    patch=${BASH_REMATCH[3]}
    version="$major.$minor.$patch"
    echo "tag $tag: packaged, the version of the app is $version" >&2
else
    if [ "$release_input" = true ] && [ "$draft_input" = true ]; then
        fail "release and draft_release together: a release is real, a dry run is not"
    fi
    wants_release=false
    if [ "$event" = schedule ] || { [ "$event" = workflow_dispatch ] && [ "$release_input" = true ]; }; then
        wants_release=true
        [ "$ref" = refs/heads/trunk ] || fail "a release is made on trunk only, this run is on $ref"
    fi

    if [ "$wants_release" = true ]; then
        last=$(git_ describe --tags --abbrev=0 --match 'v[0-9]*' "$head" 2>/dev/null || true)
        if [ -z "$last" ]; then
            release=true
            why="no v* tag before $head"
        elif [ -n "$(git_ tag --points-at "$head" --list 'v[0-9]*')" ]; then
            why="$head has the tag $last already"
        elif is_product_change "$last" "$head"; then
            release=true
            why="a product change since $last"
        else
            why="no product change since $last"
        fi
        echo "release $release: $why" >&2
    fi

    # The version of a release and of a dry run (the version the night would make).
    if [ "$release" = true ] || [ "$draft_input" = true ]; then
        plan_release_version
    fi

    if [ "$event" = schedule ] && [ "$release" != true ]; then
        # The base of the night is the head of the last other run on trunk.
        if [ -z "$base" ] || ! git_ cat-file -e "$base^{commit}" 2>/dev/null; then
            echo "base ${base:-none} (not found or not in history) head $head: needed" >&2
        else
            files=$(git_ diff --no-renames --name-only "$base" "$head" |
                grep -v -E '^(docs/|\.claude/|\.github/ISSUE_TEMPLATE/)|\.md$' || true)
            if [ -n "$files" ]; then
                echo "base $base head $head: needed, first of the files: $(echo "$files" | head -n 5 | paste -sd' ' -)" >&2
            else
                echo "base $base head $head: not needed, no change reaches a package" >&2
                needed=false
            fi
        fi
    else
        echo "event $event: packaged" >&2
    fi
fi

printf 'needed=%s\nrelease=%s\nversion=%s\npatch=%s\nlast=%s\n' \
    "$needed" "$release" "$version" "$patch" "$last"
