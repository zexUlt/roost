import QtQuick
import Roost.Api

Window {
    width: 960
    height: 600
    visible: true
    title: "roost"

    TerminalItem {
        objectName: "terminal"
        anchors.fill: parent
        focus: true
        Component.onCompleted: forceActiveFocus()
    }
}
