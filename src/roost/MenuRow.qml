import QtQuick
import QtQuick.Controls
import Roost.Api

// An item of a RoostMenu: 34 high, the hovered or highlighted one on
// `hoverBackground`, a disabled one in `textMuted`.
MenuItem {
    id: control

    implicitHeight: 34
    leftPadding: 12
    rightPadding: 12
    font: Roost.theme.rowFont

    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
        color: control.enabled ? Roost.theme.text : Roost.theme.textMuted
    }

    background: Rectangle {
        radius: Roost.theme.radiusControl
        color: control.highlighted ? Roost.theme.hoverBackground : "transparent"
    }
}
