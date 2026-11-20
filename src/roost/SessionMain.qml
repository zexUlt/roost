import QtQuick
import Roost.Api

Window {
    width: 960
    height: 600
    visible: true
    title: "roost"
    color: Roost.theme.windowBackground

    TerminalItem {
        objectName: "terminal"
        anchors.fill: parent
        focus: true
        font: Roost.theme.terminalFont
        Component.onCompleted: forceActiveFocus()
    }
}
