import QtQuick
import QtQuick.Controls
import Roost.Api

// The line between the groups of a RoostMenu: a hairline of `line` with 4 px
// above and below.
MenuSeparator {
    padding: 4
    topPadding: 4
    bottomPadding: 4

    contentItem: Rectangle {
        implicitHeight: 1
        color: Roost.theme.line
    }
}
