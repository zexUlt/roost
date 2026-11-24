import QtQuick
import QtQuick.Controls
import Roost.Api

// A menu in the light style (docs/design/18-style.md): a `surface` card of
// `radiusControl` with a hairline of `line` and a soft shadow, rows of MenuRow.
// The shadow is a few faint layers of the text colour, no effect module.
Menu {
    padding: 4
    implicitWidth: 180
    delegate: MenuRow {}

    background: Item {
        Repeater {
            model: 3

            Rectangle {
                required property int index

                anchors.fill: parent
                anchors.margins: -(index + 1)
                anchors.topMargin: -(index + 1) + 2
                anchors.bottomMargin: -(index + 1) - 2
                radius: Roost.theme.radiusControl + index + 1
                color: Roost.theme.text
                opacity: 0.04
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: Roost.theme.radiusControl
            color: Roost.theme.surface
            border.width: 1
            border.color: Roost.theme.line
        }
    }
}
