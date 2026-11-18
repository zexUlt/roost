import QtQuick
import Roost.Api

// The panel sessions.list: the sessions of all agents, from Roost.sessions.
// Up, Down, j and k move the cursor; Enter, or a tap on a row, writes the
// session under it to `selected`, the output of the panel. Moving does not
// change `selected`: opening a session is not free.
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the output of the panel.
    property var selected: null

    // The session the cursor was on last, {agent, id}, kept while the list is
    // empty or being replaced, to put the cursor back.
    property var cursorKey: null
    // Between the start of a reset of the model and its reconcile: the cursor
    // that ListView resets to is not where the user had it.
    property bool resetting: false

    // The ref of a row; the roles of Roost.sessions are the fields of
    // sessions.list in this order from Qt.UserRole: agent, id.
    function refAt(row) {
        const at = Roost.sessions.index(row, 0);
        return {
            agent: Roost.sessions.data(at, Qt.UserRole),
            id: Roost.sessions.data(at, Qt.UserRole + 1)
        };
    }

    function isSame(a, b) {
        return a !== null && b !== null && a.agent === b.agent && a.id === b.id;
    }

    // The row of a ref, or -1.
    function rowOf(ref) {
        if (!ref) {
            return -1;
        }
        const count = Roost.sessions.rowCount();
        for (let row = 0; row < count; ++row) {
            if (isSame(refAt(row), ref)) {
                return row;
            }
        }
        return -1;
    }

    function trackCursor() {
        if (!resetting && Roost.sessions.rowCount() > 0 && list.currentIndex >= 0) {
            cursorKey = refAt(list.currentIndex);
        }
    }

    // Enter: the session under the cursor becomes `selected`, unless it is
    // already, so that a session that is open is not opened again.
    function openCurrent() {
        if (list.currentIndex < 0 || list.currentIndex >= Roost.sessions.rowCount()) {
            return;
        }
        const ref = refAt(list.currentIndex);
        if (!isSame(selected, ref)) {
            selected = ref;
        }
    }

    // After a reset of the model, which says before it changes `loaded`: run
    // later, when it has. Without the first list of the connection (the
    // connection is gone) `selected` stays: the terminal opens the same
    // session when the connection is back.
    function reconcile() {
        resetting = false;
        if (!Roost.sessions.loaded) {
            return;
        }
        if (selected !== null && rowOf(selected) < 0) {
            selected = null;
        }
        let row = rowOf(cursorKey);
        if (row < 0) {
            row = rowOf(selected);
        }
        if (row < 0) {
            row = Roost.sessions.rowCount() > 0 ? 0 : -1;
        }
        list.currentIndex = row;
        trackCursor();
    }

    Rectangle {
        anchors.fill: parent
        color: "#1e1e1e"
    }

    Connections {
        target: Roost.sessions

        function onModelAboutToBeReset() {
            root.resetting = true;
        }

        function onModelReset() {
            Qt.callLater(root.reconcile);
        }

        function onRowsRemoved() {
            if (root.selected !== null && root.rowOf(root.selected) < 0) {
                root.selected = null;
            }
            Qt.callLater(root.trackCursor);
        }

        function onRowsInserted() {
            Qt.callLater(root.trackCursor);
        }
    }

    ListView {
        id: list

        objectName: "sessionList"
        anchors.fill: parent
        focus: true
        clip: true
        model: Roost.sessions
        keyNavigationEnabled: true
        keyNavigationWraps: false
        highlightMoveDuration: 0
        highlight: Rectangle {
            color: list.activeFocus ? "#3d5a80" : "#3a3a3a"
        }

        onCurrentIndexChanged: root.trackCursor()
        Component.onCompleted: root.trackCursor()

        // j and k, with no modifier.
        Keys.onPressed: event => {
            if (event.modifiers !== Qt.NoModifier) {
                return;
            }
            if (event.key === Qt.Key_J) {
                incrementCurrentIndex();
                event.accepted = true;
            } else if (event.key === Qt.Key_K) {
                decrementCurrentIndex();
                event.accepted = true;
            }
        }

        // Enter, on the main keys and on the keypad.
        Keys.onReturnPressed: root.openCurrent()
        Keys.onEnterPressed: root.openCurrent()

        delegate: Item {
            id: row

            required property int index
            required property var model

            readonly property bool isOpen: root.isSame(root.selected, {
                "agent": model.agent,
                "id": model.id
            })

            width: ListView.view.width
            height: titleText.implicitHeight + detailText.implicitHeight + 12

            Rectangle {
                visible: row.isOpen
                width: 3
                height: parent.height
                color: "#5aa0ff"
            }

            Column {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 8
                anchors.topMargin: 6

                Text {
                    id: titleText

                    objectName: "sessionTitle"
                    width: parent.width
                    elide: Text.ElideRight
                    color: "#e0e0e0"
                    font.bold: row.isOpen
                    text: row.model.title
                }

                Text {
                    id: detailText

                    objectName: "sessionDetail"
                    width: parent.width
                    elide: Text.ElideMiddle
                    color: "#808080"
                    font.pixelSize: titleText.font.pixelSize * 0.85
                    text: row.model.state + " · " + row.model.cwd
                }
            }

            TapHandler {
                onTapped: {
                    list.currentIndex = row.index;
                    list.forceActiveFocus();
                    root.openCurrent();
                }
            }
        }
    }

    Text {
        objectName: "sessionsStatus"
        anchors.fill: parent
        anchors.margins: 12
        visible: text !== ""
        color: "#808080"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: {
            if (!Roost.sessions.loaded) {
                return "Loading sessions…";
            }
            return list.count === 0 ? "No sessions." : "";
        }
    }

    // An agent whose adapter did not answer with a list: without this a
    // missing `claude` would look like "No sessions".
    Text {
        objectName: "sessionsUnavailable"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        visible: text !== ""
        color: "#e0a060"
        wrapMode: Text.Wrap
        text: Roost.sessions.unavailable.map(entry => entry.agent + ": " + entry.reason).join("\n")
    }
}
