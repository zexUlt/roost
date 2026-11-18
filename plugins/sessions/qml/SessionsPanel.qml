import QtQuick
import Roost.Api
import RoostSessions

// The panel sessions.list: the sessions of all agents, grouped by category by
// SessionsList (module RoostSessions), which owns the rows, the cursor and the
// state kept in the store document `sessions`. Up, Down, j and k move the
// cursor over the stops: sessions and folded headers. Enter, or a tap on a
// row, writes the session under the cursor to `selected`, the output of the
// panel; Enter on a folded header unfolds it. Moving does not change
// `selected`: opening a session is not free.
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the output of the panel.
    property var selected: null

    // Enter or a tap opened or targeted a row, the open one included: the shell
    // may move the keyboard to the terminal. Not emitted when Enter does
    // nothing.
    signal activated

    // Between the start of a reset of Roost.sessions and the reconcile of
    // `selected`: the list is not settled, so Enter does nothing.
    property bool resetting: false
    // The open session of the stored document was taken into `selected`, or
    // there was none: from now on `selected` is written to the document.
    property bool restored: false

    function isSame(a, b) {
        return a !== null && b !== null && a.agent === b.agent && a.id === b.id;
    }

    // Whether Roost.sessions lists the session, whatever the view shows: the
    // roles of Roost.sessions are the fields of sessions.list in this order
    // from Qt.UserRole: agent, id.
    function listed(ref) {
        if (!ref) {
            return false;
        }
        const count = Roost.sessions.rowCount();
        for (let row = 0; row < count; ++row) {
            const at = Roost.sessions.index(row, 0);
            if (Roost.sessions.data(at, Qt.UserRole) === ref.agent && Roost.sessions.data(at, Qt.UserRole + 1) === ref.id) {
                return true;
            }
        }
        return false;
    }

    // `selected` goes when its session is no longer listed; not while the list
    // of the connection is missing: the terminal opens the same session when
    // the connection is back.
    function dropGone() {
        if (Roost.sessions.loaded && selected !== null && !listed(selected)) {
            selected = null;
        }
    }

    // Once the document was read and the first list is in: the session that was
    // open when the window closed is selected again, if it is still listed;
    // `selected` never takes a session that is not. Then the document follows
    // `selected`, whatever it is, so that a session that is gone is not kept
    // as the open one.
    function restoreOpened() {
        if (restored || !sessionsList.ready || !Roost.sessions.loaded) {
            return;
        }
        restored = true;
        const ref = sessionsList.opened;
        if (ref !== null && selected === null && listed(ref)) {
            selected = ref;
        }
        if (!isSame(selected, ref)) {
            sessionsList.opened = selected;
        }
    }

    // Enter: the session under the cursor becomes `selected`, unless it is
    // already, so that a session that is open is not opened again; a folded
    // header unfolds and opens nothing. Not while the list is being replaced:
    // the cursor is then not the user's.
    function openCurrent() {
        if (resetting) {
            return;
        }
        const ref = sessionsList.cursorSession;
        if (ref === null) {
            const category = sessionsList.cursorCategory;
            if (category !== null) {
                sessionsList.toggleFold(category);
            }
            return;
        }
        if (!isSame(selected, ref)) {
            selected = ref;
        }
        activated();
    }

    // The ListView follows the cursor of the model, whatever it did itself.
    function syncCursor() {
        if (list.currentIndex !== sessionsList.cursorRow) {
            list.currentIndex = sessionsList.cursorRow;
        }
    }

    // The stored document: read on creation if connected and at every
    // connection (the window forgets its copy when the connection goes),
    // written as the model asks.
    function readStore() {
        Roost.store.get("sessions", (error, result) => {
            if (error) {
                sessionsList.readFailed(error.message ? error.message : "the store read failed");
            } else {
                sessionsList.restore(result.document);
            }
        });
    }

    onSelectedChanged: {
        if (restored) {
            sessionsList.opened = selected;
        }
    }

    Component.onCompleted: {
        if (Roost.connected) {
            readStore();
        }
    }

    SessionsList {
        id: sessionsList

        objectName: "sessionsModel"
        source: Roost.sessions
        sourceLoaded: Roost.sessions.loaded

        onSaveRequested: document => {
            Roost.store.set("sessions", document, error => {
                sessionsList.saveFinished(error ? (error.message ? error.message : "the store write failed") : "");
            });
        }
        onReadyChanged: root.restoreOpened()
        onSourceLoadedChanged: root.restoreOpened()
        onCursorChanged: root.syncCursor()
    }

    Connections {
        target: Roost

        function onConnectedChanged() {
            if (Roost.connected) {
                root.readStore();
            }
        }
    }

    Connections {
        target: Roost.store

        function onChanged(plugin, document) {
            if (plugin === "sessions") {
                sessionsList.restore(document);
            }
        }
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

        // SessionsModel emits modelReset before it updates `loaded`: settle
        // after the emit has returned.
        function onModelReset() {
            Qt.callLater(root.settle);
        }

        function onRowsRemoved() {
            root.dropGone();
        }
    }

    function settle() {
        resetting = false;
        dropGone();
        restoreOpened();
    }

    ListView {
        id: list

        objectName: "sessionList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // The list ends above the list of unavailable agents.
        anchors.bottom: unavailable.visible ? unavailable.top : parent.bottom
        focus: true
        clip: true
        model: sessionsList
        highlightMoveDuration: 0
        highlight: Rectangle {
            color: list.activeFocus ? "#3d5a80" : "#3a3a3a"
        }

        // The model owns the cursor: whatever the view does to its index
        // (rows inserted, removed) is put back.
        onCurrentIndexChanged: Qt.callLater(root.syncCursor)
        onCountChanged: Qt.callLater(root.syncCursor)

        // Up and Down, and j and k with no modifier on any layout: bindingKey
        // is the key the US layout has at the physical position.
        Keys.onPressed: event => {
            const key = Roost.bindingKey(event);
            const plain = event.modifiers === Qt.NoModifier;
            if (event.key === Qt.Key_Down || (key === Qt.Key_J && plain)) {
                sessionsList.move(1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up || (key === Qt.Key_K && plain)) {
                sessionsList.move(-1);
                event.accepted = true;
            }
        }

        // Enter, on the main keys and on the keypad.
        // A held Enter (auto-repeat) does nothing: it would go on into the
        // terminal, and there reopen an ended or failed session at once.
        Keys.onReturnPressed: event => {
            event.accepted = true;
            if (!event.isAutoRepeat) {
                root.openCurrent();
            }
        }
        Keys.onEnterPressed: event => {
            event.accepted = true;
            if (!event.isAutoRepeat) {
                root.openCurrent();
            }
        }

        delegate: Item {
            id: row

            required property int index
            required property var model

            readonly property bool isHeader: model.kind === "header"
            readonly property bool isOpen: !isHeader && root.isSame(root.selected, {
                "agent": model.agent,
                "id": model.id
            })

            width: ListView.view.width
            height: isHeader ? headerText.implicitHeight + 8 : titleText.implicitHeight + detailText.implicitHeight + 12

            Rectangle {
                visible: row.isOpen
                width: 3
                height: parent.height
                color: "#5aa0ff"
            }

            // A header: the category ("Uncategorized" for none), the number of
            // its sessions, and a mark of a folded one.
            Text {
                id: headerText

                objectName: "headerTitle"
                visible: row.isHeader
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 8
                elide: Text.ElideRight
                color: "#a0a0a0"
                font.bold: true
                text: row.isHeader ? (row.model.folded ? "▸ " : "▾ ") + (row.model.category === "" ? "Uncategorized" : row.model.category) + " · " + row.model.count : ""
            }

            Column {
                visible: !row.isHeader
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
                    text: row.isHeader ? "" : row.model.title
                }

                Text {
                    id: detailText

                    objectName: "sessionDetail"
                    width: parent.width
                    elide: Text.ElideMiddle
                    color: "#808080"
                    font.pixelSize: titleText.font.pixelSize * 0.85
                    text: row.isHeader ? "" : row.model.state + " · " + row.model.cwd
                }
            }

            TapHandler {
                enabled: !row.isHeader
                onTapped: {
                    sessionsList.setCursorRow(row.index);
                    root.syncCursor();
                    list.forceActiveFocus();
                    root.openCurrent();
                }
            }
        }
    }

    Text {
        objectName: "sessionsStatus"
        anchors.fill: list
        anchors.margins: 12
        visible: text !== ""
        color: "#808080"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: {
            if (!Roost.sessions.loaded || !sessionsList.ready) {
                return "Loading sessions…";
            }
            // With an agent that did not answer, an empty list is not "No
            // sessions": the text below says why.
            const none = list.count === 0 && Roost.sessions.unavailable.length === 0;
            return none ? "No sessions." : "";
        }
    }

    // An agent whose adapter did not answer with a list: the status above does
    // not say "No sessions." while there is such an agent, so a missing
    // `claude` does not look like an empty list.
    Text {
        id: unavailable

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
