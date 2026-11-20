#!/usr/bin/env bash
# The QML colours gate (design.md §5, Roost.theme): the QML of the window and of
# the plugins takes its colours from Roost.theme, so no colour is written in it.
#
# The rule. In every .qml file of
#   src/roost/*.qml                 (not below)
#   plugins/*/qml/**/*.qml
#   templates/plugin-qml/**/*.qml
# a line is a violation if it has
#   1. a string that is a colour in hex: "#rgb", "#rrggbb", "#aarrggbb", up to
#      "#rrrrggggbbbb" (3 to 12 hex digits), in double or single quotes;
#   2. a string that is exactly a colour name of Qt (the SVG names,
#      case-insensitive), whatever property it is for; "transparent" is
#      excepted: it is no hue and the theme has no role for it;
#   3. a call that makes a colour: Qt.rgba, Qt.rgb, Qt.hsla, Qt.hsl, Qt.hsva,
#      Qt.hsv, Qt.color.
# Comments are scanned too: a quoted colour in a comment is a violation, so the
# comment says what colour it means without quoting it. The theme is JSON
# (src/qml_api/themes/default.json) and is not in the scanned set. Agent icons
# are SVG and PNG files, not QML: out of scope.
#
# tests/gates/qml_colors/ holds one tree per case, laid out like the repository.
# "clean" must pass, and every other case breaks the rule one way and must fail
# with its file named. A gate that only checked rejection would also pass a
# gate that rejects everything; the clean case rules that out.
#
# Usage: tools/check_qml_colors.sh            the repository, then the cases
#        tools/check_qml_colors.sh <root>     only the tree at <root>
set -uo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)

names='aliceblue|antiquewhite|aqua|aquamarine|azure|beige|bisque|black|blanchedalmond|blue|blueviolet|brown|burlywood|cadetblue|chartreuse|chocolate|coral|cornflowerblue|cornsilk|crimson|cyan|darkblue|darkcyan|darkgoldenrod|darkgray|darkgreen|darkgrey|darkkhaki|darkmagenta|darkolivegreen|darkorange|darkorchid|darkred|darksalmon|darkseagreen|darkslateblue|darkslategray|darkslategrey|darkturquoise|darkviolet|deeppink|deepskyblue|dimgray|dimgrey|dodgerblue|firebrick|floralwhite|forestgreen|fuchsia|gainsboro|ghostwhite|gold|goldenrod|gray|grey|green|greenyellow|honeydew|hotpink|indianred|indigo|ivory|khaki|lavender|lavenderblush|lawngreen|lemonchiffon|lightblue|lightcoral|lightcyan|lightgoldenrodyellow|lightgray|lightgreen|lightgrey|lightpink|lightsalmon|lightseagreen|lightskyblue|lightslategray|lightslategrey|lightsteelblue|lightyellow|lime|limegreen|linen|magenta|maroon|mediumaquamarine|mediumblue|mediumorchid|mediumpurple|mediumseagreen|mediumslateblue|mediumspringgreen|mediumturquoise|mediumvioletred|midnightblue|mintcream|mistyrose|moccasin|navajowhite|navy|oldlace|olive|olivedrab|orange|orangered|orchid|palegoldenrod|palegreen|paleturquoise|palevioletred|papayawhip|peachpuff|peru|pink|plum|powderblue|purple|rebeccapurple|red|rosybrown|royalblue|saddlebrown|salmon|sandybrown|seagreen|seashell|sienna|silver|skyblue|slateblue|slategray|slategrey|snow|springgreen|steelblue|tan|teal|thistle|tomato|turquoise|violet|wheat|white|whitesmoke|yellow|yellowgreen'
quote="[\"']"
hex="${quote}#[0-9A-Fa-f]{3,12}${quote}"
named="${quote}(${names})${quote}"
call='Qt\.(rgba|rgb|hsla|hsl|hsva|hsv|color)[[:space:]]*\('

# The files of the rule under $1, one per line.
scanned() {
    local base=$1
    [ -d "$base/src/roost" ] &&
        find "$base/src/roost" -maxdepth 1 -name '*.qml'
    local dir
    for dir in "$base"/plugins/*/qml; do
        [ -d "$dir" ] && find "$dir" -name '*.qml'
    done
    [ -d "$base/templates/plugin-qml" ] &&
        find "$base/templates/plugin-qml" -name '*.qml'
    return 0
}

# violations <root>: the violating lines as file:line:text, relative to root.
violations() {
    local base=$1 file
    while IFS= read -r file; do
        {
            grep -nE "$hex" "$file"
            grep -niE "$named" "$file"
            grep -nE "$call" "$file"
        } 2>/dev/null | sort -t: -k1,1n -u | sed "s|^|${file#"$base"/}:|"
    done < <(scanned "$base" | sort)
    return 0
}

if [ $# -ge 1 ]; then
    found=$(violations "$1")
    if [ -n "$found" ]; then
        printf '%s\n' "$found"
        exit 1
    fi
    exit 0
fi

failed=0

if found=$(violations "$root") && [ -z "$found" ]; then
    echo "ok:   no colour in the QML of the repository"
else
    echo "FAIL: the QML of the repository has colours; take them from Roost.theme:"
    printf '%s\n' "$found" | sed 's/^/    /'
    failed=1
fi

fixtures=$root/tests/gates/qml_colors

# check <case> <pass|fail> [expected file]
check() {
    local name=$1 expect=$2 file=${3:-} output status=0
    output=$("$0" "$fixtures/$name") || status=$?
    if [ "$expect" = pass ]; then
        if [ "$status" -ne 0 ]; then
            echo "FAIL: $name has no colour and was rejected:"
            printf '%s\n' "$output" | sed 's/^/    /'
            failed=1
        else
            echo "ok:   $name passes"
        fi
    elif [ "$status" -eq 0 ]; then
        echo "FAIL: $name breaks the rule and passed"
        failed=1
    elif ! grep -qF "$file" <<<"$output"; then
        echo "FAIL: $name failed, but without naming $file:"
        printf '%s\n' "$output" | sed 's/^/    /'
        failed=1
    else
        echo "ok:   $name is rejected, naming $file"
    fi
}

check clean pass
check hex6_window fail src/roost/Window.qml
check hex8_plugin fail plugins/demo/qml/deep/Row.qml
check hex3_single_quotes fail src/roost/Short.qml
check name_template fail templates/plugin-qml/qml/Hello.qml
check name_case fail src/roost/Upper.qml
check call_rgba fail plugins/demo/qml/Call.qml

exit "$failed"
