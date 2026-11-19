import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The window of the shell (design.md §6): the fixed layout "list | terminal".
// roost::Shell fills the slots and writes the properties below; the one link
// of the layout is the Binding at the bottom.
Window {
    id: root

    // "connecting", "connected", "disconnected" or "refused"; written by C++.
    property string connection: "connecting"
    // The reason of the disconnect, or for "refused" the message with the
    // versions and the hint.
    property string connectionMessage: ""
    // Strings: the problems of roostd's plugins, of the panels and of links.
    property var problems: []

    // The Retry button of a refused connection.
    signal retryRequested

    // The width of the list slot in pixels: a step of `<` or `>` in the list
    // panel, and its bounds. Not saved until the layout is (M5).
    readonly property int listWidthStep: 20
    readonly property int listWidthMin: 200
    readonly property int listWidthMax: 560

    width: 960
    height: 600
    visible: true
    title: "roost"

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Rectangle {
            id: banner

            objectName: "connectionBanner"
            Layout.fillWidth: true
            visible: root.connection !== "connected"
            implicitHeight: bannerRow.implicitHeight + 16
            color: root.connection === "refused" ? "#5c1f1f" : "#4a4a2a"

            RowLayout {
                id: bannerRow

                anchors.fill: parent
                anchors.margins: 8
                spacing: 12

                Label {
                    objectName: "connectionText"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: "white"
                    text: {
                        switch (root.connection) {
                        case "connecting":
                            return "Connecting to roostd...";
                        case "refused":
                            return root.connectionMessage;
                        default:
                            return "Disconnected from roostd, reconnecting. " + root.connectionMessage;
                        }
                    }
                }

                Button {
                    objectName: "retryButton"
                    visible: root.connection === "refused"
                    text: "Retry"
                    onClicked: root.retryRequested()
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 1

            PanelSlot {
                id: listSlot

                objectName: "panelSlot"
                focus: true
                panelType: "sessions.list"
                Layout.preferredWidth: 320
                Layout.fillHeight: true
            }

            PanelSlot {
                id: terminalSlot

                objectName: "panelSlot"
                panelType: "terminal.view"
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
        }

        Rectangle {
            objectName: "problemsStrip"
            Layout.fillWidth: true
            visible: root.problems.length > 0
            implicitHeight: Math.min(problemsList.contentHeight, 96) + 8
            color: "#3a2a1a"

            ListView {
                id: problemsList

                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: root.problems
                delegate: Label {
                    required property string modelData

                    width: ListView.view.width
                    text: modelData
                    color: "#ffd9a0"
                    elide: Text.ElideRight
                }
            }
        }
    }

    // The one link of the layout: the output of the list panel to the input
    // of the terminal panel. Off while a panel or a name is missing; an output
    // that is undefined (an uninitialised `property var`) is written as null.
    Binding {
        target: terminalSlot.panel
        property: terminalSlot.linkInput
        value: listSlot.panel && listSlot.linkOutput !== "" ? (listSlot.panel[listSlot.linkOutput] ?? null) : null
        when: terminalSlot.panel !== null && listSlot.panel !== null && terminalSlot.linkInput !== "" && listSlot.linkOutput !== ""
    }

    // The list panel's optional signals. `activated()`: the user chose a row, so
    // the keyboard goes to the terminal. `widthStepRequested(step)`: `<` or `>`,
    // a narrower (-1) or a wider (+1) list, within bounds. `respawned(session)`:
    // the agent respawned a session of the list; if the terminal panel shows it
    // and has the optional function `reopenWhenEnded(session)`, the session is
    // opened again there once the old attach has ended. A list panel without
    // them is fine.
    Connections {
        target: listSlot.panel
        ignoreUnknownSignals: true

        function onActivated() {
            // Without a terminal panel the slot shows a placeholder: the
            // keyboard stays in the list.
            if (terminalSlot.panel !== null) {
                terminalSlot.forceActiveFocus();
            }
        }

        function onRespawned(session) {
            const panel = terminalSlot.panel;
            if (panel !== null && typeof panel.reopenWhenEnded === "function") {
                panel.reopenWhenEnded(session);
            }
        }

        function onWidthStepRequested(step) {
            listSlot.Layout.preferredWidth = Math.max(root.listWidthMin, Math.min(root.listWidthMax, listSlot.Layout.preferredWidth + step * root.listWidthStep));
        }
    }

    // Ctrl+\ (the physical Control key; Qt's Ctrl is Command on macOS) moves
    // the keyboard to the list from anywhere, the terminal included. Not a
    // toggle: in the list it does nothing.
    Shortcut {
        context: Qt.WindowShortcut
        sequence: Qt.platform.os === "osx" ? "Meta+\\" : "Ctrl+\\"
        onActivated: listSlot.forceActiveFocus()
    }
}
