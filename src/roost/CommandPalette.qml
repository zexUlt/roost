import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The command palette (design.md §16, "Палитра"): every command that the
// plugin manifests declare, with its key and the marks of the keymap. Typing
// filters, Up and Down move, Enter runs the command, Escape closes. The
// command runs through Roost.commands.run after the palette has closed, so
// that a command that opens a dialog or moves the focus (shell.reportBug,
// shell.focusList) does not start while the modal popup is still there.
Popup {
    id: root

    // The model of the rows; a test reads it.
    readonly property alias commands: paletteModel
    // The row that Enter would run.
    readonly property alias currentRow: list.currentIndex
    // The command that runs when the popup has closed.
    property string pendingCommand: ""
    // The command of the chosen row: a rebuild of the rows chooses it again.
    property string chosenCommand: ""

    // Runs the command of the current row and closes the palette.
    function accept() {
        const command = paletteModel.commandAt(list.currentIndex);
        if (command === "") {
            return;
        }
        pendingCommand = command;
        close();
    }

    objectName: "commandPalette"
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(600, parent.width - 40)
    height: Math.min(440, parent.height - 40)
    padding: 8
    modal: true
    closePolicy: Popup.CloseOnEscape

    onAboutToShow: {
        pendingCommand = "";
        filterField.text = "";
        list.currentIndex = paletteModel.rowCount() > 0 ? 0 : -1;
    }
    onOpened: filterField.forceActiveFocus()
    onClosed: {
        const command = pendingCommand;
        pendingCommand = "";
        if (command !== "" && Roost.commands !== null) {
            Roost.commands.run(command);
        }
    }

    PaletteModel {
        id: paletteModel

        objectName: "paletteModel"
        onFilterChanged: list.currentIndex = rowCount() > 0 ? 0 : -1
        onRebuilt: {
            const row = rowOf(root.chosenCommand);
            list.currentIndex = row >= 0 ? row : (rowCount() > 0 ? 0 : -1);
        }
    }

    contentItem: ColumnLayout {
        spacing: 6

        TextField {
            id: filterField

            objectName: "paletteField"
            Layout.fillWidth: true
            placeholderText: "Type a command"
            onTextChanged: paletteModel.filter = text
            Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
            Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
            Keys.onReturnPressed: root.accept()
            Keys.onEnterPressed: root.accept()
        }

        ListView {
            id: list

            objectName: "paletteList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: paletteModel
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds
            // No delegates are made ahead of the view: one that is still being
            // made when the filter shrinks the list is cancelled with a Qt warning.
            cacheBuffer: 0
            onCurrentIndexChanged: {
                if (currentIndex >= 0) {
                    root.chosenCommand = paletteModel.commandAt(currentIndex);
                }
            }

            delegate: ItemDelegate {
                id: row

                required property int index
                required property string commandId
                required property string title
                required property string plugin
                required property string key
                required property string conflict
                required property bool overridden
                required property bool capturesInput

                objectName: "paletteRow"
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                onClicked: {
                    list.currentIndex = row.index;
                    root.accept();
                }

                contentItem: ColumnLayout {
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            objectName: "paletteTitle"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: row.title
                        }

                        Label {
                            objectName: "paletteOverridden"
                            visible: row.overridden
                            color: Roost.theme.accent
                            text: "keymap.json"
                        }

                        Label {
                            objectName: "paletteCaptures"
                            visible: row.capturesInput
                            color: Roost.theme.warning
                            text: "captures input"
                        }

                        Label {
                            objectName: "paletteKey"
                            color: Roost.theme.textSecondary
                            text: row.key
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        color: Roost.theme.textMuted
                        text: row.plugin + " · " + row.commandId
                    }

                    Label {
                        objectName: "paletteConflict"
                        Layout.fillWidth: true
                        visible: row.conflict !== ""
                        wrapMode: Text.Wrap
                        color: Roost.theme.warning
                        text: row.conflict
                    }
                }
            }
        }

        Label {
            objectName: "paletteEmpty"
            visible: list.count === 0
            color: Roost.theme.textMuted
            text: paletteModel.total === 0 ? "No commands." : "No command matches."
        }
    }
}
