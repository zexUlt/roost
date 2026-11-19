import QtQuick
import Roost.Api

// The panel terminal.view: the session chosen in the list, shown in a
// TerminalItem. The shell writes `session`; the item opens it and reports how
// it stands. Enter reopens a session that ended or failed to open.
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
    onSessionChanged: reopenWanted = false

    TerminalItem {
        id: terminal

        objectName: "terminal"
        anchors.fill: parent
        focus: true
        session: root.session

        // Enter, on the main keys and on the keypad.
        function handleEnter(event) {
            if (status === "ended" || status === "failed") {
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

    Text {
        objectName: "terminalStatus"
        anchors.fill: parent
        anchors.margins: 12
        visible: text !== ""
        color: "#808080"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: {
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
