import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// A yes/no question before something that cannot be taken back at once: a
// title, a text, the main button with the label of the action and Cancel.
// Escape and Cancel decline. The dialog changes nothing itself: the main
// button is `confirmed`. The look is the one of the other dialogs (a card, the
// accepting button primary), see docs/design/18-style.md.
Dialog {
    id: root

    // The question, under the title.
    property string text: ""
    // The label of the main button: the action, not "OK".
    property string confirmText: "OK"

    // The user pressed the main button; the dialog closes after it.
    signal confirmed

    objectName: "confirmDialog"
    modal: true
    width: 420
    anchors.centerIn: Overlay.overlay
    closePolicy: Popup.CloseOnEscape
    padding: Roost.theme.panelPadding

    background: Rectangle {
        radius: Roost.theme.radiusCard
        color: Roost.theme.surface
        border.width: 1
        border.color: Roost.theme.line
    }

    header: Label {
        objectName: "confirmTitle"
        text: root.title
        font: Roost.theme.headingFont
        elide: Text.ElideRight
        padding: Roost.theme.panelPadding
        bottomPadding: 0
    }

    // The keyboard starts on Cancel: a key pressed by mistake does not do
    // what the dialog warns about.
    onAboutToShow: cancelButton.forceActiveFocus()

    contentItem: Label {
        objectName: "confirmText"
        text: root.text
        wrapMode: Text.Wrap
    }

    footer: DialogButtonBox {
        padding: Roost.theme.panelPadding
        topPadding: 0
        spacing: 8
        background: null

        FormButton {
            objectName: "confirmButton"
            text: root.confirmText
            primary: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            onClicked: {
                root.confirmed();
                root.close();
            }
        }

        FormButton {
            id: cancelButton

            objectName: "cancelButton"
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
    }
}
