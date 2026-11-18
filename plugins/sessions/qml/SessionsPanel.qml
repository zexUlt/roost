import QtQuick
import Roost.Api
import RoostSessions

// The panel sessions.list: the sessions of all agents, grouped by category by
// SessionsList (module RoostSessions), which owns the rows, the cursor and the
// state kept in the store document `sessions`. Up, Down, j and k move the
// cursor over the stops: sessions and folded headers. Enter, or a tap on a
// row, writes the session under the cursor to `selected`, the output of the
// panel; Enter on a folded header unfolds it. Moving does not change
// `selected`: opening a session is not free. A tap on a header folds or
// unfolds it. The head shows the title (Sessions, or Archive), the query and
// the tabs with their counts; the keys a, v, m, z, Z, p, ?, < and > act on the
// view (the help overlay lists them).
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the output of the panel.
    property var selected: null

    // Enter or a tap opened or targeted a row, the open one included: the shell
    // may move the keyboard to the terminal. Not emitted when Enter does
    // nothing.
    signal activated

    // `<` or `>`: the user asks the shell for a narrower (-1) or a wider (+1)
    // list. The shell decides the width; a shell without a handler does
    // nothing.
    signal widthStepRequested(int step)

    // The help overlay is open: it takes every key until `?` or Esc closes it.
    property bool helpOpen: false

    // The colours of the categories, by the `colorIndex` of the model, and of
    // the states, hard-coded until the palette of the layout (M5).
    readonly property var categoryColors: ["#0087d7", "#d70000", "#008700", "#d75f00", "#8700d7", "#008787", "#d700af", "#875f00"]
    readonly property string textColor: "#e0e0e0"
    readonly property var tabs: [
        {
            "name": "all",
            "label": "All"
        },
        {
            "name": "active",
            "label": "Active"
        },
        {
            "name": "unread",
            "label": "Unread"
        }
    ]

    function stateColor(state) {
        switch (state) {
        case "working":
            return "#d78700";
        case "blocked":
        case "needs_input":
            return "#005fd7";
        case "failed":
            return "#d70000";
        default:
            return textColor;
        }
    }

    // The words of the second line: a session that waits for the user says so.
    function stateWord(state) {
        return state === "blocked" || state === "needs_input" ? "waiting" : state;
    }

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
        // The help takes every key, Enter included.
        if (resetting || helpOpen) {
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

    // The ListView follows the cursor of the model, whatever it did itself; its
    // highlight keeps the row in view.
    function syncCursor() {
        if (list.currentIndex !== sessionsList.cursorRow) {
            list.currentIndex = sessionsList.cursorRow;
        }
    }

    // z and p act on the category of the cursor: of the session under it, or of
    // the folded header it is on. "" is the uncategorized, so the absence of a
    // category is null, not falsy.
    function foldCursorCategory() {
        const category = sessionsList.cursorCategory;
        if (category !== null) {
            sessionsList.toggleFold(category);
        }
    }

    function pinCursorCategory() {
        const category = sessionsList.cursorCategory;
        if (category !== null) {
            sessionsList.togglePin(category);
        }
    }

    // A tap on a header folds or unfolds its category and puts the cursor on
    // it, when it is a stop: a folded header is one, an unfolded one is not.
    // Nothing is opened and `selected` stays.
    function tapHeader(row, category, folded) {
        if (folded) {
            sessionsList.setCursorRow(row);
            sessionsList.toggleFold(category);
        } else {
            sessionsList.toggleFold(category);
            sessionsList.setCursorRow(row);
        }
        syncCursor();
        list.forceActiveFocus();
    }

    // The keys of the view. `key` is Roost.bindingKey(event). Letters match the
    // key and Shift (z, Z); symbols match the key and ignore Shift, which a
    // layout may need for them (? is Shift+7 on the Russian one). Ctrl, Alt or
    // Meta: not a list key. Whether the key was taken.
    function viewKey(event, key) {
        if ((event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) !== 0) {
            return false;
        }
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const act = !event.isAutoRepeat;
        switch (key) {
        case Qt.Key_A:
            if (shift) {
                return false;
            }
            if (act) {
                sessionsList.nextTab();
            }
            return true;
        case Qt.Key_V:
            if (shift) {
                return false;
            }
            if (act) {
                sessionsList.viewArchive = !sessionsList.viewArchive;
            }
            return true;
        case Qt.Key_M:
            if (shift) {
                return false;
            }
            if (act) {
                sessionsList.compact = !sessionsList.compact;
            }
            return true;
        case Qt.Key_P:
            if (shift) {
                return false;
            }
            if (act) {
                pinCursorCategory();
            }
            return true;
        case Qt.Key_Z:
            if (act) {
                if (shift) {
                    sessionsList.unfoldAll();
                } else {
                    foldCursorCategory();
                }
            }
            return true;
        case Qt.Key_Question:
            if (act) {
                helpOpen = true;
            }
            return true;
        case Qt.Key_Less:
        case Qt.Key_Comma:
            widthStepRequested(-1);
            return true;
        case Qt.Key_Greater:
        case Qt.Key_Period:
            widthStepRequested(1);
            return true;
        }
        return false;
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

    // The head: the title and the query, and the tabs with the counts of the
    // view.
    Column {
        id: head

        objectName: "sessionsHead"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 4
        // The rows are cut at the panel edge, not drawn over its neighbour.
        clip: true

        Row {
            width: parent.width
            spacing: 8

            Text {
                objectName: "sessionsTitle"
                color: root.textColor
                font.bold: true
                text: sessionsList.viewArchive ? "Archive" : "Sessions"
            }

            Text {
                objectName: "sessionsQuery"
                visible: sessionsList.query !== ""
                // The query takes what the title leaves.
                width: Math.max(0, parent.width - x)
                elide: Text.ElideRight
                color: "#a0a0a0"
                text: "/ " + sessionsList.query
            }
        }

        Row {
            width: parent.width
            spacing: 12

            Repeater {
                model: root.tabs

                // A tap on a tab selects it.
                delegate: Text {
                    id: tab

                    required property var modelData
                    readonly property bool current: sessionsList.tab === modelData.name

                    objectName: "sessionsTab"
                    color: current ? "#ffffff" : "#808080"
                    font.bold: current
                    font.underline: current
                    text: modelData.label + " " + (sessionsList.counts[modelData.name] ?? 0)

                    TapHandler {
                        onTapped: {
                            sessionsList.tab = tab.modelData.name;
                            list.forceActiveFocus();
                        }
                    }
                }
            }
        }
    }

    ListView {
        id: list

        objectName: "sessionList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: head.bottom
        anchors.topMargin: 6
        // The list ends above the list of unavailable agents, and above the
        // footer.
        anchors.bottom: unavailable.visible ? unavailable.top : footer.top
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
        // is the key the US layout has at the physical position. With the help
        // open every key is taken, and `?` or Esc closes it.
        Keys.onPressed: event => {
            const key = Roost.bindingKey(event);
            const plain = event.modifiers === Qt.NoModifier;
            if (root.helpOpen) {
                // A held `?` or Esc closes it once, not through the keys behind.
                if (!event.isAutoRepeat && (key === Qt.Key_Question || event.key === Qt.Key_Escape)) {
                    root.helpOpen = false;
                }
                event.accepted = true;
            } else if (event.key === Qt.Key_Down || (key === Qt.Key_J && plain)) {
                sessionsList.move(1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up || (key === Qt.Key_K && plain)) {
                sessionsList.move(-1);
                event.accepted = true;
            } else if (root.viewKey(event, key)) {
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
            // The second line of a session; in the compact view only under the
            // cursor.
            readonly property bool showsDetail: !isHeader && (!sessionsList.compact || index === sessionsList.cursorRow)

            width: ListView.view.width
            height: isHeader ? headerText.implicitHeight + 8 : titleText.implicitHeight + (showsDetail ? detailText.implicitHeight : 0) + 12

            // The open session: a tint over the row and a bar at its edge.
            Rectangle {
                visible: row.isOpen
                anchors.fill: parent
                color: "#225aa0ff"
            }

            Rectangle {
                visible: row.isOpen
                width: 3
                height: parent.height
                color: "#5aa0ff"
            }

            // A header: the category ("Uncategorized" for none) in its colour,
            // the number of its sessions, and a mark of a folded one; at the
            // right the marks of a pinned category and of the unread sessions
            // of a folded one.
            Text {
                id: headerText

                objectName: "headerTitle"
                visible: row.isHeader
                anchors.left: parent.left
                anchors.right: marks.left
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 8
                elide: Text.ElideRight
                color: row.isHeader && row.model.colorIndex >= 0 ? root.categoryColors[row.model.colorIndex % root.categoryColors.length] : "#a0a0a0"
                font.bold: true
                text: row.isHeader ? (row.model.folded ? "▸ " : "▾ ") + (row.model.category === "" ? "Uncategorized" : row.model.category) + " · " + row.model.count : ""
            }

            Row {
                id: marks

                visible: row.isHeader
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Text {
                    objectName: "pinMark"
                    visible: row.isHeader && row.model.pinned
                    color: "#a0a0a0"
                    text: "▲"
                }

                Text {
                    objectName: "headerUnread"
                    visible: row.isHeader && row.model.folded && row.model.unreadCount > 0
                    color: "#5aa0ff"
                    text: row.isHeader ? "● " + row.model.unreadCount : ""
                }
            }

            Text {
                objectName: "unreadDot"
                visible: !row.isHeader && row.model.unread
                x: 10
                y: 6
                color: "#5aa0ff"
                text: "●"
            }

            Column {
                visible: !row.isHeader
                anchors.fill: parent
                anchors.leftMargin: 26
                anchors.rightMargin: 8
                anchors.topMargin: 6

                Text {
                    id: titleText

                    objectName: "sessionTitle"
                    width: parent.width
                    elide: Text.ElideRight
                    color: row.isHeader ? root.textColor : root.stateColor(row.model.state)
                    font.bold: row.isOpen
                    text: row.isHeader ? "" : row.model.title
                }

                Text {
                    id: detailText

                    objectName: "sessionDetail"
                    width: parent.width
                    visible: row.showsDetail
                    elide: Text.ElideMiddle
                    color: "#808080"
                    font.pixelSize: titleText.font.pixelSize * 0.85
                    text: row.isHeader ? "" : root.stateWord(row.model.state) + " · " + row.model.cwd
                }
            }

            TapHandler {
                onTapped: {
                    if (row.isHeader) {
                        root.tapHeader(row.index, row.model.category, row.model.folded);
                        return;
                    }
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
        anchors.bottom: footer.top
        anchors.margins: 8
        visible: text !== ""
        color: "#e0a060"
        wrapMode: Text.Wrap
        text: Roost.sessions.unavailable.map(entry => entry.agent + ": " + entry.reason).join("\n")
    }

    // The footer: one area under the list, lines one over the other, each only
    // as high as its text and none when it has no text. Its lines keep their
    // places: the problem of the store; the prompt, and the confirmation and
    // the notice of a command (steps that add them put them in this Column).
    Column {
        id: footer

        objectName: "footer"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        spacing: 2

        // A list that cannot be written says why; the list itself works.
        Text {
            objectName: "sessionsProblem"
            width: parent.width
            leftPadding: 8
            rightPadding: 8
            bottomPadding: 4
            visible: text !== ""
            color: sessionsList.readOnly ? "#e06060" : "#e0a060"
            wrapMode: Text.Wrap
            text: sessionsList.problem
        }
    }

    // The keys that work, over the list: `?` or Esc closes it.
    Rectangle {
        objectName: "sessionsHelp"
        anchors.fill: parent
        visible: root.helpOpen
        color: "#f01e1e1e"

        TapHandler {
            onTapped: root.helpOpen = false
        }

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Text {
                color: root.textColor
                font.bold: true
                text: "Keyboard shortcuts"
            }

            Text {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Move\n↑ ↓ j k   move the cursor\nEnter   open the session, unfold a folded category"
            }

            Text {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "View\na   next tab (All, Active, Unread)\nv   sessions or archive\nm   compact: the second line only under the cursor\nz   fold the category under the cursor\nShift+z   unfold all\np   pin the category under the cursor"
            }

            Text {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Window\n< >   narrower or wider list\n?   this help (Esc closes it)\nCtrl+\\   from the terminal back to the list"
            }
        }
    }
}
