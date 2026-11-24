import QtQuick
import QtQuick.Controls
import Roost.Api

// The panel terminal.view: the session chosen in the list, shown in a
// TerminalItem under a head (76 high at least) with the session's title and meta line
// and the buttons "Lists" and "Commands". The shell writes `session`; the item
// opens it and reports how it stands. Enter reopens a session that ended or
// failed to open.
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the input of the panel.
    property var session: null

    // The agent respawned `ref`: the shell calls this (the list panel's signal
    // `respawned`). When the panel shows that session the old attach is ending
    // or has ended, and the session is opened again as soon as it has: a new
    // attach shows the new run. Not for another session; the wish lapses after
    // `reopenWaitMs` or when another session is shown.
    function reopenWhenEnded(ref) {
        if (!ref || !session || ref.agent !== session.agent || ref.id !== session.id) {
            return;
        }
        if (terminal.status === "ended" || terminal.status === "failed") {
            terminal.reopen();
            return;
        }
        reopenWanted = true;
        reopenTimer.restart();
    }

    property bool reopenWanted: false
    property int reopenWaitMs: 10000
    onSessionChanged: {
        reopenWanted = false;
        refreshAll();
    }

    // {title, cwd, state} of the shown session in Roost.sessions, or null when
    // it is not listed (yet) or the window has no API; the roles are the fields
    // of sessions.list in order: agent, id, title, cwd, state.
    property var info: null

    // The shown session in rows `first`..`last` of Roost.sessions: its
    // {title, cwd, state}, or null if it is not there.
    function findInfo(first, last) {
        let found = null;
        if (session && Roost.sessions !== null) {
            const count = Roost.sessions.rowCount();
            for (let row = Math.max(0, first); row <= Math.min(last, count - 1); ++row) {
                const at = Roost.sessions.index(row, 0);
                if (Roost.sessions.data(at, Qt.UserRole) === session.agent && Roost.sessions.data(at, Qt.UserRole + 1) === session.id) {
                    found = {
                        "title": Roost.sessions.data(at, Qt.UserRole + 2),
                        "cwd": Roost.sessions.data(at, Qt.UserRole + 3),
                        "state": Roost.sessions.data(at, Qt.UserRole + 4)
                    };
                    break;
                }
            }
        }
        return found;
    }

    // The whole model, for a change of the session or of the rows.
    function refreshAll() {
        setInfo(findInfo(0, Number.MAX_SAFE_INTEGER));
    }

    // A change of rows `first`..`last`: a row is replaced by remove and insert,
    // never by a change of its agent and id, so only a row that is the session
    // can bring news of it.
    function refreshRows(first, last) {
        const found = findInfo(first, last);
        if (found !== null) {
            setInfo(found);
        }
    }

    // `info` changes only when the title, cwd or state does: the bindings of
    // the head wake on an assignment, equal or not.
    function setInfo(found) {
        const same = found === null || info === null ? found === info : found.title === info.title && found.cwd === info.cwd && found.state === info.state;
        if (!same) {
            info = found;
        }
    }

    // One line of plain text: the controls and the line breaks of a title or
    // a cwd become spaces. The twin of `oneLine` in SessionsPanel.qml: plugins
    // cannot share QML code, so the function is copied.
    function oneLine(text) {
        return String(text).replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, " ");
    }

    // The words of the meta line; the twin of `stateWord` in SessionsPanel.qml.
    function stateWord(state) {
        return state === "blocked" || state === "needs_input" ? "waiting" : state;
    }

    // The head: the title the list shows for the session (its id, if it has
    // none), or the placeholder without a session.
    readonly property string headTitle: {
        if (!session) {
            return "No session";
        }
        return oneLine(info && info.title ? info.title : session.id);
    }

    // `state · cwd`, as the row of the list; empty without a listed session.
    readonly property string headMeta: {
        if (!session || !info) {
            return "";
        }
        return oneLine([stateWord(info.state), info.cwd].filter(part => part).join(" · "));
    }

    Connections {
        target: Roost.sessions

        function onModelReset() {
            root.refreshAll();
        }
        function onRowsInserted() {
            root.refreshAll();
        }
        function onRowsRemoved() {
            root.refreshAll();
        }
        function onDataChanged(topLeft, bottomRight) {
            root.refreshRows(topLeft.row, bottomRight.row);
        }
    }

    // The command terminal.copy (declared by the manifest of the plugin
    // `shell`, which gives it Cmd+C and Ctrl+Shift+C in this panel) copies the
    // selection. It goes when the panel does. The item still copies on the same
    // keys by itself (IsCopyShortcut) whenever the table does not take the
    // key. With two terminal panels the one that has the focus runs, else the
    // later registration.
    // `Roost.commands` is null in a window that has no API (some tests).
    Component.onCompleted: {
        if (Roost.commands !== null) {
            Roost.commands.registerCommand("terminal.copy", () => terminal.copy());
        }
        refreshAll();
    }

    // The head, on the card: title and meta on the left, the quiet buttons on
    // the right. It has no fill: the card paints `surface` under it, and a
    // square of its own would stick out of the arc of the card's corners. The
    // buttons take no focus, so a click leaves the keyboard in the terminal
    // (as the buttons of the tab bar, TabNode.qml). It is 76 high, or as high
    // as the two lines with the padding above and below when a large font of
    // the interface needs more.
    Item {
        id: head

        objectName: "terminalHead"
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: Math.max(76, heading.implicitHeight + 2 * Roost.theme.panelPadding)

        Column {
            id: heading

            objectName: "terminalHeading"

            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.right: buttons.left
            anchors.rightMargin: Roost.theme.panelPadding
            anchors.verticalCenter: parent.verticalCenter

            Text {
                objectName: "terminalTitle"
                width: parent.width
                textFormat: Text.PlainText
                text: root.headTitle
                font: Roost.theme.headingFont
                color: Roost.theme.text
                elide: Text.ElideRight
            }

            Text {
                objectName: "terminalMeta"
                width: parent.width
                visible: text !== ""
                textFormat: Text.PlainText
                text: root.headMeta
                font: Roost.theme.metaFont
                color: Roost.theme.textSecondary
                // The end of a path is the part that tells.
                elide: Text.ElideMiddle
            }
        }

        // The title keeps at least `titleMin`: when the head is too narrow
        // for it and both buttons, "Commands" goes first, then "Lists".
        Row {
            id: buttons

            readonly property real titleMin: 80
            readonly property real room: head.width - 20 - 2 * Roost.theme.panelPadding - titleMin

            anchors.right: parent.right
            anchors.rightMargin: Roost.theme.panelPadding
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8

            HeadButton {
                id: listsButton

                objectName: "listsButton"
                visible: implicitWidth <= buttons.room
                text: "Lists"
                tip: "Show or hide the session lists"
                onClicked: root.runCommand("layout.toggleLists")
            }

            HeadButton {
                objectName: "commandsButton"
                visible: listsButton.implicitWidth + buttons.spacing + implicitWidth <= buttons.room
                text: "Commands"
                tip: "Open the command palette"
                onClicked: root.runCommand("shell.palette")
            }
        }
    }

    function runCommand(command) {
        if (Roost.commands !== null) {
            Roost.commands.run(command);
        }
    }

    // A quiet button of the head: no fill, `hoverBackground` under the mouse,
    // 34 high, `radiusControl`. Not FormButton of the shell: a plugin does not
    // import it, and it takes the focus.
    component HeadButton: Button {
        id: control

        property string tip: ""

        implicitHeight: 34
        leftPadding: 12
        rightPadding: 12
        focusPolicy: Qt.NoFocus
        font: Roost.theme.rowFont
        ToolTip.visible: hovered && tip !== ""
        ToolTip.text: tip
        ToolTip.delay: 600

        contentItem: Text {
            text: control.text
            font: control.font
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: control.hovered ? Roost.theme.text : Roost.theme.textSecondary
        }

        background: Rectangle {
            implicitWidth: 64
            radius: Roost.theme.radiusControl
            color: control.hovered || control.down ? Roost.theme.hoverBackground : "transparent"
        }
    }

    TerminalItem {
        id: terminal

        objectName: "terminal"
        anchors.top: head.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Roost.theme.cornerInset
        anchors.rightMargin: Roost.theme.cornerInset
        // The cells are square: off the rounded corners of the card, where
        // the square lies inside the arc of radiusCard (and its line). The
        // head covers the top corners, so there is no inset under it. Not
        // anchored to the bottom: a card shorter than the head would give a
        // negative height.
        height: Math.max(0, root.height - head.height - Roost.theme.cornerInset)
        focus: true
        session: root.session
        // The family and size of settings.terminalFont, in every window.
        font: Roost.theme.terminalFont

        // Another panel that shows the same session took it over: this one
        // takes it back when it gets the focus or Enter.
        onActiveFocusChanged: {
            if (activeFocus && superseded) {
                takeBack();
            }
        }

        // Enter, on the main keys and on the keypad.
        function handleEnter(event) {
            if (superseded) {
                takeBack();
            } else if (status === "ended" || status === "failed") {
                reopen();
            } else {
                event.accepted = false;
            }
        }

        Keys.onReturnPressed: event => handleEnter(event)
        Keys.onEnterPressed: event => handleEnter(event)

        onStatusChanged: {
            if (root.reopenWanted && status === "ended") {
                root.reopenWanted = false;
                reopen();
            }
        }
    }

    Timer {
        id: reopenTimer

        interval: root.reopenWaitMs
        onTriggered: root.reopenWanted = false
    }

    Label {
        objectName: "terminalStatus"
        anchors.top: head.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 12
        visible: text !== ""
        color: Roost.theme.textMuted
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: {
            if (terminal.superseded) {
                return "Shown in another panel.";
            }
            switch (terminal.status) {
            case "none":
                // A session is chosen but the item has no size yet, so it has
                // not started opening.
                return root.session ? "" : "No session. Choose one in the list.";
            case "opening":
                return root.session ? "Opening " + root.session.agent + "/" + root.session.id + "…" : "";
            case "ended":
                return "Session ended: " + terminal.statusMessage + ". Press Enter to reopen.";
            case "failed":
                return "Could not open the session: " + terminal.statusMessage + ". Press Enter to try again.";
            default:
                return "";
            }
        }
    }
}
