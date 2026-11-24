import QtQuick
import QtQuick.Controls
import Roost.Api

// A text field of a form in the light style (docs/design/18-style.md): 38 high,
// a hairline of `line`, which the ring of the focus replaces while it has the
// keyboard.
TextField {
    implicitHeight: 38
    leftPadding: 12
    rightPadding: 12
    selectionColor: Roost.theme.selection
    selectedTextColor: Roost.theme.textStrong

    background: Rectangle {
        radius: Roost.theme.radiusControl
        color: Roost.theme.inputBackground
        border.width: parent.activeFocus ? Roost.theme.cursorRingWidth : 1
        border.color: parent.activeFocus ? Roost.theme.focus : Roost.theme.line
    }
}
