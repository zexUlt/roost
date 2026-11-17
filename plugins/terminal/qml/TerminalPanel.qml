import QtQuick
import Roost.Api

// The panel terminal.view: the session chosen in the list, shown in a
// TerminalItem. The shell writes `session`; the item opens it and reports how
// it stands. Enter reopens a session that ended.
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the input of the panel.
    property var session: null

    TerminalItem {
        id: terminal

        objectName: "terminal"
        anchors.fill: parent
        focus: true
        session: root.session

        Keys.onReturnPressed: event => {
            if (status === "ended" || status === "failed") {
                reopen();
            } else {
                event.accepted = false;
            }
        }
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
                return "No session. Choose one in the list.";
            case "opening":
                return root.session ? "Opening " + root.session.agent + "/" + root.session.id + "…" : "";
            case "ended":
            case "failed":
                return "Session ended: " + terminal.statusMessage + ". Press Enter to reopen.";
            default:
                return "";
            }
        }
    }
}
