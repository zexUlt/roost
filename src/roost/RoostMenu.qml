import QtQuick
import QtQuick.Controls
import Roost.Api

// A menu in the light style (docs/design/18-style.md): a `surface` card of
// `radiusControl` with a hairline of `line` and a soft shadow, rows of MenuRow.
// The shadow is a few faint layers of the text colour, no effect module.
Menu {
    // The widest item of the menu.
    function widest() {
        let width = 0;
        for (let i = 0; i < count; ++i) {
            width = Math.max(width, itemAt(i)?.implicitWidth ?? 0);
        }
        return width;
    }

    padding: 4
    // A long item widens the menu instead of being cut.
    implicitWidth: Math.max(180, widest() + leftPadding + rightPadding)
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
