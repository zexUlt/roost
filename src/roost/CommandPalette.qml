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
    // The height of a palette row (18-style.md): the least the list may have.
    readonly property int rowHeight: 47
    // The card never gets higher than this: half of the window, but not below
    // the fixed parts and one row, and not above what the window leaves after
    // a margin on each side. Whole pixels: a half pixel blurs the hairline.
    readonly property real maxHeight: {
        // Summed from the parts, not taken from the content's implicit height:
        // that one holds the list's contentHeight, which depends on the height
        // of the list, which depends on this cap.
        const emptyLine = emptyLabel.visible ? emptyLabel.implicitHeight + emptyLabel.Layout.bottomMargin : 0;
        const fixedParts = topPadding + bottomPadding + header.Layout.topMargin + header.implicitHeight + filterField.Layout.topMargin + filterField.Layout.preferredHeight + list.Layout.topMargin + list.Layout.bottomMargin + emptyLine + hairline.Layout.preferredHeight + footer.Layout.preferredHeight;
        const half = Math.max(Math.floor(parent.height / 2), fixedParts + rowHeight);
        return Math.max(0, Math.min(half, parent.height - 2 * Roost.theme.windowMargin));
    }

    // A font of the scale at the weight of the prototype's titles.
    function demiBold(base) {
        const font = base;
        font.weight = Font.DemiBold;
        return font;
    }

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
    // The card of the prototype's side view (docs/design/18-style.md): 304
    // wide, as tall as its content (the rows of the list included) up to
    // maxHeight; the list scrolls when the rows do not fit. The top is where a
    // card of maxHeight would have it centred, so typing, which changes the
    // number of rows, grows and shrinks the card downward only and the header
    // and the field stay put.
    width: Math.min(304, parent.width - 2 * Roost.theme.windowMargin)
    height: Math.round(Math.min(contentItem.implicitHeight + topPadding + bottomPadding, maxHeight))
    x: Math.round((parent.width - width) / 2)
    y: Math.round((parent.height - maxHeight) / 2)
    padding: 0
    modal: true
    closePolicy: Popup.CloseOnEscape

    // A card: the surface, the hairline, and a soft shadow made of a few
    // translucent rounded rectangles behind it (no effect, no dependency).
    background: Item {
        Repeater {
            model: 4

            Rectangle {
                required property int index

                anchors.fill: parent
                anchors.margins: -(index + 1) * 3
                anchors.topMargin: -(index + 1) * 3 + 6
                anchors.bottomMargin: -(index + 1) * 3 - 6
                radius: Roost.theme.radiusCard + (index + 1) * 3
                color: Roost.theme.text
                opacity: 0.025
            }
        }

        Rectangle {
            anchors.fill: parent
            radius: Roost.theme.radiusCard
            color: Roost.theme.surface
            border.width: 1
            border.color: Roost.theme.line
        }
    }

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
        spacing: 0

        // The header: the title, and the line that says where the keys go.
        ColumnLayout {
            id: header

            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.topMargin: 17
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            spacing: 5

            Label {
                objectName: "paletteHeading"
                Layout.fillWidth: true
                elide: Text.ElideRight
                font: Roost.theme.titleFont
                color: Roost.theme.text
                text: "Commands"
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 6

                Rectangle {
                    implicitWidth: 8
                    implicitHeight: 8
                    radius: 4
                    color: Roost.theme.focus
                }

                Label {
                    objectName: "paletteHint"
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                    font: root.demiBold(Roost.theme.metaFont)
                    color: Roost.theme.focus
                    text: "Input in commands · Esc to close"
                }
            }
        }

        // The search field always has the focus while the palette is open, so
        // it always wears the ring.
        TextField {
            id: filterField

            objectName: "paletteField"
            Layout.fillWidth: true
            Layout.topMargin: 6
            Layout.leftMargin: Roost.theme.panelPadding
            Layout.rightMargin: Roost.theme.panelPadding
            Layout.preferredHeight: 40
            leftPadding: 39
            rightPadding: 12
            placeholderText: "Action or session"
            placeholderTextColor: Roost.theme.textMuted
            color: Roost.theme.text
            onTextChanged: paletteModel.filter = text
            Keys.onUpPressed: list.currentIndex = Math.max(0, list.currentIndex - 1)
            Keys.onDownPressed: list.currentIndex = Math.min(list.count - 1, list.currentIndex + 1)
            Keys.onReturnPressed: root.accept()
            Keys.onEnterPressed: root.accept()

            background: Rectangle {
                radius: Roost.theme.radiusControl
                color: Roost.theme.inputBackground
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }

            Label {
                objectName: "paletteSearchIcon"
                x: 13
                anchors.verticalCenter: parent.verticalCenter
                font: Roost.theme.headingFont
                color: Roost.theme.textMuted
                text: "⌕"
            }
        }

        ListView {
            id: list

            objectName: "paletteList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            // As tall as the rows; the card caps the height, and the layout
            // takes the rest from this list first.
            Layout.preferredHeight: contentHeight
            // One row at least, so a small window does not take the chosen
            // row away.
            Layout.minimumHeight: Math.min(root.rowHeight, contentHeight)
            Layout.topMargin: 8
            Layout.bottomMargin: 8
            Layout.leftMargin: Roost.theme.panelPadding
            Layout.rightMargin: Roost.theme.panelPadding
            clip: true
            model: paletteModel
            currentIndex: -1
            boundsBehavior: Flickable.StopAtBounds
            // No delegates are made ahead of the view: one that is still being
            // made when the filter shrinks the list is cancelled with a Qt warning.
            cacheBuffer: 0
            // A smaller window shortens the list: the chosen row stays on it.
            onHeightChanged: {
                if (currentIndex >= 0) {
                    positionViewAtIndex(currentIndex, ListView.Contain);
                }
            }
            onCurrentIndexChanged: {
                if (currentIndex >= 0) {
                    // Keep the row that Enter would run on the screen.
                    positionViewAtIndex(currentIndex, ListView.Contain);
                    root.chosenCommand = paletteModel.commandAt(currentIndex);
                }
            }

            // The model has no groups (a command belongs to a plugin, and the
            // palette lists them in one run), so there are no group headings.
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
                implicitHeight: Math.max(root.rowHeight, contentItem.implicitHeight + topPadding + bottomPadding)
                topPadding: 4
                bottomPadding: 4
                leftPadding: 12
                rightPadding: 12
                highlighted: ListView.isCurrentItem
                onClicked: {
                    list.currentIndex = row.index;
                    root.accept();
                }

                background: Rectangle {
                    radius: Roost.theme.radiusControl
                    color: row.highlighted ? Roost.theme.selectedBackground : (row.hovered ? Roost.theme.hoverBackground : "transparent")
                }

                contentItem: ColumnLayout {
                    spacing: 1

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            objectName: "paletteTitle"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font: root.demiBold(Roost.theme.rowFont)
                            color: Roost.theme.text
                            text: row.title
                        }

                        // The key takes at most 45% of the row, the rest is the
                        // title's; a long key is cut on the right.
                        Label {
                            objectName: "paletteKey"
                            Layout.maximumWidth: Math.floor(row.availableWidth * 0.45)
                            elide: Text.ElideRight
                            font: Roost.theme.smallFont
                            color: Roost.theme.textSecondary
                            text: row.key
                        }
                    }

                    // The second line: where the command comes from, then the
                    // marks of its key. The marks do not elide; the id does.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            font: Roost.theme.smallFont
                            color: Roost.theme.textMuted
                            text: row.plugin + " · " + row.commandId
                        }

                        Label {
                            objectName: "paletteOverridden"
                            visible: row.overridden
                            font: Roost.theme.smallFont
                            color: Roost.theme.accent
                            text: "keymap.json"
                        }

                        Label {
                            objectName: "paletteCaptures"
                            visible: row.capturesInput
                            font: Roost.theme.smallFont
                            color: Roost.theme.warning
                            text: "captures input"
                        }
                    }

                    Label {
                        objectName: "paletteConflict"
                        // The width is known when the row is made, so the
                        // wrapped height is too: the list measures its rows
                        // once, and a row that grows after it was placed
                        // leaves the chosen one out of view.
                        width: row.availableWidth
                        Layout.fillWidth: true
                        visible: row.conflict !== ""
                        wrapMode: Text.Wrap
                        font: Roost.theme.smallFont
                        color: Roost.theme.warning
                        text: row.conflict
                    }
                }
            }
        }

        Label {
            id: emptyLabel

            objectName: "paletteEmpty"
            Layout.leftMargin: Roost.theme.panelPadding + 12
            Layout.bottomMargin: 8
            visible: list.count === 0
            font: Roost.theme.metaFont
            color: Roost.theme.textMuted
            text: paletteModel.total === 0 ? "No commands." : "No command matches."
        }

        // The footer: a hairline, and what the keys do.
        Rectangle {
            id: hairline

            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Roost.theme.line
        }

        ColumnLayout {
            id: footer

            Layout.fillWidth: true
            Layout.fillHeight: false
            Layout.preferredHeight: 56
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            spacing: 4

            Item {
                Layout.fillHeight: true
            }

            Label {
                objectName: "paletteFooterKeys"
                font: Roost.theme.metaFont
                color: Roost.theme.textSecondary
                text: "↑↓ select · Enter run"
            }

            Label {
                objectName: "paletteFooterBack"
                font: Roost.theme.metaFont
                color: Roost.theme.textSecondary
                text: "Esc back"
            }

            Item {
                Layout.fillHeight: true
            }
        }
    }
}
