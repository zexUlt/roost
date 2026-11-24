import QtQuick
import QtQuick.Controls
import Roost.Api

// A button of the toolbar (Linux only: macOS has the native menu bar) in the
// light style, derived with no mockup (docs/design/18-style.md): `radiusControl`,
// `hoverBackground` under the mouse, the ring of the focus outside with a gap of
// 2 px. The Fusion button is restyled by its own background and content, no
// Controls style.
ToolButton {
    id: control

    implicitHeight: 34
    leftPadding: 12
    rightPadding: 12
    font: Roost.theme.rowFont

    contentItem: Text {
        text: control.text
        font: control.font
        elide: Text.ElideRight
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        color: !control.enabled ? Roost.theme.textMuted : control.hovered ? Roost.theme.text : Roost.theme.textSecondary
    }

    background: Rectangle {
        implicitWidth: 34
        radius: Roost.theme.radiusControl
        color: control.hovered || control.down ? Roost.theme.hoverBackground : "transparent"

        Rectangle {
            objectName: "focusRing"
            anchors.fill: parent
            anchors.margins: -(2 + Roost.theme.cursorRingWidth)
            visible: control.visualFocus
            radius: Roost.theme.radiusControl + 2 + Roost.theme.cursorRingWidth
            color: "transparent"
            border.width: Roost.theme.cursorRingWidth
            border.color: Roost.theme.focus
        }
    }
}
