import QtQuick
import QtQuick.Controls
import Roost.Api

// A button of a dialog or of a form in the light style (docs/design/18-style.md):
// quiet by default, primary when `primary`. The Fusion button is restyled by
// its own background and content, as the style guide asks; no Controls style.
Button {
    id: control

    // The main button of the dialog: the accepting one.
    property bool primary: false

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
        color: !control.enabled ? Roost.theme.textMuted : control.primary ? Roost.theme.primaryText : control.hovered ? Roost.theme.text : Roost.theme.textSecondary
    }

    background: Rectangle {
        implicitWidth: 64
        radius: Roost.theme.radiusControl
        color: !control.enabled ? Roost.theme.controlBackground : control.primary ? (control.hovered || control.down ? Roost.theme.primaryHover : Roost.theme.primary) : (control.hovered || control.down ? Roost.theme.hoverBackground : Roost.theme.surface)
        border.width: 1
        border.color: control.primary && control.enabled ? color : Roost.theme.line

        // The ring of the focus, outside with a gap of 2 px.
        Rectangle {
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
