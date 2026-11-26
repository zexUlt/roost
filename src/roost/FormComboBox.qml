import QtQuick
import QtQuick.Controls
import Roost.Api

// A drop-down list of a form in the light style (docs/design/18-style.md): 38
// high, a hairline of `line`, the ring of the focus outside with a gap of 2 px.
// The entries of the list are rows like the ones of a session list.
ComboBox {
    id: control

    implicitHeight: 38
    leftPadding: 12
    font: Roost.theme.uiFont

    delegate: ItemDelegate {
        required property int index

        width: ListView.view ? ListView.view.width : control.width
        implicitHeight: 34
        leftPadding: 12
        highlighted: control.highlightedIndex === index
        font: Roost.theme.rowFont
        text: control.textAt(index)

        contentItem: Text {
            text: parent.text
            font: parent.font
            elide: Text.ElideRight
            verticalAlignment: Text.AlignVCenter
            color: Roost.theme.text
        }

        background: Rectangle {
            radius: Roost.theme.radiusControl
            color: parent.highlighted ? Roost.theme.hoverBackground : "transparent"
        }
    }

    background: Rectangle {
        radius: Roost.theme.radiusControl
        color: Roost.theme.inputBackground
        border.width: 1
        border.color: Roost.theme.line

        Rectangle {
            anchors.fill: parent
            anchors.margins: -(2 + Roost.theme.cursorRingWidth)
            visible: control.visualFocus || (control.editable && control.contentItem.activeFocus)
            radius: Roost.theme.radiusControl + 2 + Roost.theme.cursorRingWidth
            color: "transparent"
            border.width: Roost.theme.cursorRingWidth
            border.color: Roost.theme.focus
        }
    }

    popup: Popup {
        y: control.height + 2
        width: control.width
        implicitHeight: Math.min(contentItem.implicitHeight + 2 * padding, 300)
        padding: 4

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            spacing: Roost.theme.rowGap
        }

        background: Rectangle {
            radius: Roost.theme.radiusControl
            color: Roost.theme.surface
            border.width: 1
            border.color: Roost.theme.line
        }
    }
}
