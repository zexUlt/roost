import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The capture of a key for a command (design.md §16, "Запись из окна"): the
// user presses the combination, and the dialog says it by `captured`; the
// window decides what follows. Escape and Cancel decline. While `conflict`
// is set the dialog does not capture: it shows the warning with Confirm and
// Cancel, and `confirmed` is Confirm (the dialog stays until the window
// closes it). The look is the one of the other dialogs
// (a card, the accepting button primary), see docs/design/18-style.md.
//
// A dialog, not a mode of the row: a key that is caught by the window's table
// does not reach a field of the window, but the matcher stands aside for the
// focus inside a Popup, so that the pressed key is only captured and does not
// run the command that has it.
Dialog {
    id: root

    // The command the key is for, as the row names it.
    property string commandTitle: ""
    // The warning of a key that another command has; empty while capturing.
    property string conflict: ""
    // What roostd refused with, or the window found wrong with the key; empty
    // when nothing is.
    property string problem: ""
    // A write of the window is under way: no key is taken (the window drops it,
    // and the dialog says why), Confirm waits.
    property bool busy: false

    // The user pressed a chord; `chord` is its text, as the table writes it.
    signal captured(string chord)
    // The user confirmed the warning; the window closes the dialog.
    signal confirmed

    objectName: "keyCaptureDialog"
    title: "Add key"
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
        objectName: "keyCaptureTitle"
        text: root.title
        font: Roost.theme.headingFont
        elide: Text.ElideRight
        padding: Roost.theme.panelPadding
        bottomPadding: 0
    }

    // The keyboard starts on the area that takes the pressed keys.
    onAboutToShow: {
        conflict = "";
        problem = "";
        capture.forceActiveFocus();
    }

    // The warning has the keyboard on Cancel: a key pressed by mistake does
    // not do what it warns about.
    onConflictChanged: {
        if (conflict !== "") {
            cancelButton.forceActiveFocus();
        } else if (visible) {
            capture.forceActiveFocus();
        }
    }

    KeyChords {
        id: chords
    }

    contentItem: ColumnLayout {
        spacing: 8

        // Takes every pressed key while capturing; Escape is left to the
        // popup, which closes.
        Item {
            id: capture

            objectName: "keyCapture"
            Layout.fillWidth: true
            implicitHeight: hint.implicitHeight
            focus: true
            activeFocusOnTab: true
            Accessible.role: Accessible.EditableText
            Accessible.name: "Press the new key for " + root.commandTitle
            Keys.onPressed: event => {
                // Escape is left to the popup, which closes. Tab and Shift+Tab
                // are the way to the buttons, so they move the focus and are
                // not captured: not even for a row with `when`, where roostd
                // would take them as a key. A user who wants them on a command
                // writes them in keymap.json, which can bind them.
                const navigates = (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) && (event.modifiers & ~Qt.ShiftModifier) === 0;
                if (event.key === Qt.Key_Escape || navigates) {
                    event.accepted = false;
                    return;
                }
                // Nothing reaches a control of the window: the key is the
                // user's answer, not a command.
                event.accepted = true;
                if (root.conflict !== "" || root.busy || event.isAutoRepeat) {
                    return;
                }
                const chord = chords.chordOf(event);
                if (chord !== "") {
                    root.captured(chord);
                }
            }

            Label {
                id: hint

                objectName: "keyCaptureHint"
                width: parent.width
                wrapMode: Text.Wrap
                color: Roost.theme.textSecondary
                text: root.busy ? "Saving the key\u2026" : (root.conflict === "" ? "Press the new key for " + root.commandTitle + ". Tab goes to the buttons, Esc cancels." : "")
            }
        }

        Label {
            objectName: "keyConflict"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: Roost.theme.warning
            text: root.conflict
            // A warning that the user must hear when it appears: the focus is
            // on Cancel, not on the text.
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
            onTextChanged: shown => {
                if (shown !== "") {
                    Accessible.announce(shown, Accessible.Assertive);
                }
            }
        }

        Label {
            objectName: "keyProblem"
            Layout.fillWidth: true
            visible: text !== ""
            wrapMode: Text.Wrap
            color: Roost.theme.error
            text: root.problem
            Accessible.role: Accessible.AlertMessage
            Accessible.name: text
            onTextChanged: shown => {
                if (shown !== "") {
                    Accessible.announce(shown, Accessible.Assertive);
                }
            }
        }
    }

    footer: DialogButtonBox {
        padding: Roost.theme.panelPadding
        topPadding: 0
        spacing: 8
        background: null

        FormButton {
            objectName: "keyConflictConfirm"
            text: "Confirm"
            primary: true
            visible: root.conflict !== ""
            enabled: !root.busy
            // Not AcceptRole, which closes the dialog: the window closes it
            // when the write has ended, or says in it why it failed.
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.confirmed()
        }

        FormButton {
            id: cancelButton

            objectName: "keyCaptureCancel"
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
    }
}
