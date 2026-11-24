import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// "Report a Bug…": the user describes the problem, roost writes a zip with
// the logs and the description. Nothing leaves the machine: Show and Open
// work on a click only. The look is the one of the other dialogs (a card, the
// accepting button primary), derived from the style guide: no mockup.
Dialog {
    id: root

    // The window whose graphics API goes into about.txt; optional.
    property alias window: bugReport.window
    // Where the zip goes; empty: ~/.roost/bugreports. A test sets it.
    property alias outputDirectory: bugReport.outputDirectory
    // The C++ side; a test reads zipPath and error.
    readonly property alias report: bugReport
    // Whether the zip is written (the dialog then shows where).
    property bool done: false
    // The files that go in, apart from about.txt and description.txt.
    property var fileList: []

    // The text and the check of the form; a test fills them.
    property alias description: descriptionField.text
    property alias includeSettings: settingsBox.checked

    function refresh() {
        fileList = bugReport.files(settingsBox.checked);
    }

    // Writes the zip with the form as it is.
    function create() {
        done = bugReport.create(descriptionField.text, settingsBox.checked);
    }

    objectName: "bugReportDialog"
    title: "Report a Bug"
    modal: true
    width: 520
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
        text: root.title
        font: Roost.theme.headingFont
        elide: Text.ElideRight
        padding: Roost.theme.panelPadding
        bottomPadding: 0
    }

    onAboutToShow: {
        done = false;
        bugReport.clearError();
        descriptionField.text = "";
        settingsBox.checked = false;
        refresh();
        descriptionField.forceActiveFocus();
    }

    BugReport {
        id: bugReport
    }

    // A Label that can be selected and copied: no frame, the font and the
    // color of a Label.
    component SelectableText: TextEdit {
        readOnly: true
        selectByMouse: true
        selectByKeyboard: true
        color: root.palette.windowText
        font: root.font
    }

    contentItem: ColumnLayout {
        spacing: 8

        Label {
            text: "What went wrong?"
            visible: !root.done
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 140
            visible: !root.done

            background: Rectangle {
                radius: Roost.theme.radiusControl
                color: Roost.theme.inputBackground
                border.width: descriptionField.activeFocus ? Roost.theme.cursorRingWidth : 1
                border.color: descriptionField.activeFocus ? Roost.theme.focus : Roost.theme.line
            }

            TextArea {
                id: descriptionField

                objectName: "descriptionField"
                wrapMode: TextEdit.Wrap
                leftPadding: 10
                rightPadding: 10
                selectionColor: Roost.theme.selection
                selectedTextColor: Roost.theme.textStrong
                background: null
                placeholderText: "Describe the problem and how to reproduce it"
            }
        }

        Label {
            text: "The zip will contain about.txt, description.txt and:"
            visible: !root.done
        }

        Repeater {
            model: root.fileList
            delegate: SelectableText {
                required property var modelData

                objectName: "fileEntry"
                text: "  " + modelData.text
                visible: !root.done
            }
        }

        Label {
            objectName: "noLogsText"
            text: "  (no log files found)"
            visible: !root.done && root.fileList.length === 0
        }

        CheckBox {
            id: settingsBox

            objectName: "includeSettingsBox"
            text: "Include settings"
            visible: !root.done
            onToggled: root.refresh()
        }

        SelectableText {
            objectName: "errorText"
            Layout.fillWidth: true
            wrapMode: TextEdit.Wrap
            color: Roost.theme.windowError
            text: bugReport.error
            visible: bugReport.error !== "" && !root.done
        }

        SelectableText {
            objectName: "zipPathText"
            Layout.fillWidth: true
            wrapMode: TextEdit.WrapAnywhere
            text: "Saved: " + bugReport.zipPath
            visible: root.done
        }

        SelectableText {
            Layout.fillWidth: true
            wrapMode: TextEdit.Wrap
            text: "Send us the zip: open an issue and attach it."
            visible: root.done
        }

        RowLayout {
            visible: root.done

            FormButton {
                objectName: "showButton"
                text: Qt.platform.os === "osx" ? "Show in Finder" : "Show in Folder"
                onClicked: bugReport.showInFolder()
            }

            FormButton {
                objectName: "issueButton"
                text: "Open GitHub Issue"
                onClicked: bugReport.openIssue(descriptionField.text)
            }
        }
    }

    footer: DialogButtonBox {
        padding: Roost.theme.panelPadding
        topPadding: 0
        spacing: 8
        background: null

        FormButton {
            objectName: "createButton"
            text: "Create"
            primary: true
            visible: !root.done
            DialogButtonBox.buttonRole: DialogButtonBox.ActionRole
            onClicked: root.create()
        }

        FormButton {
            objectName: "cancelButton"
            text: root.done ? "Close" : "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.close()
        }
    }
}
