import QtQuick
import QtCore
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
// view (the help overlay lists them). The keys /, c and e open the prompt in
// the footer: the search, the category and the name of the session under the
// cursor; n opens it for the task and the category of a new session. The keys
// d, x, r and R run the commands of the agent of the session
// (`<agent>.<verb>`, Roost.commands); the footer asks for a confirmation (y)
// and shows the outcome as a notice.
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

    // The agent respawned a session that is open here (r, R): the shell may
    // reopen it in the terminal once its old attach has ended. `session` is
    // {agent, id}.
    signal respawned(var session)

    // The help overlay is open: it takes every key until `?` or Esc closes it.
    property bool helpOpen: false

    // The footer. `confirm` is null or {action, ref, text}: a command that
    // waits for y. `notice` is the outcome of a command, cleared after
    // `noticeMs`.
    property var confirm: null
    property string notice: ""
    property int noticeMs: 8000

    // The prompt of the footer: "search", "category", "rename", "task" and
    // "newCategory" (the two steps of n), or "" when there is none. The session
    // a category or a name is for is taken when the prompt opens (`promptRef`),
    // and the agent, the directory and the task of a new one are (`promptNew`),
    // whatever the cursor and the polls do while the user types.
    property string promptKind: ""
    property var promptRef: null
    property var promptNew: null
    readonly property var promptLabels: ({
            "search": "/",
            "category": "Category:",
            "rename": "Name:",
            "task": "Task:",
            "newCategory": "Category:"
        })
    // The field takes a known category by its digit.
    readonly property bool promptPicksCategory: promptKind === "category" || promptKind === "newCategory"

    // The sessions deleted (d) whose rows have not left Roost.sessions yet:
    // they are forgotten by the list only when they have, or the list would
    // take the session that is still listed for a new one.
    property var forgetPending: []

    // The colours of the categories, by the `colorIndex` of the model, and of
    // the states, hard-coded until the palette of the layout (M6).
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

    // The title the list shows for the session; its row is in view, being the
    // cursor's. The title of the agent, or the id, if it is not.
    function titleOf(ref) {
        const row = sessionsList.rowOf(ref);
        const item = row >= 0 ? list.itemAtIndex(row) : null;
        if (item && item.model && item.model.title) {
            return item.model.title;
        }
        const data = sessionData(ref);
        return data && data.title ? data.title : ref.id;
    }

    // {title, cwd, state} of a session of Roost.sessions, or null; the roles
    // are the fields of sessions.list in order: agent, id, title, cwd, state.
    function sessionData(ref) {
        const count = Roost.sessions.rowCount();
        for (let row = 0; row < count; ++row) {
            const at = Roost.sessions.index(row, 0);
            if (Roost.sessions.data(at, Qt.UserRole) === ref.agent && Roost.sessions.data(at, Qt.UserRole + 1) === ref.id) {
                return {
                    "title": Roost.sessions.data(at, Qt.UserRole + 2),
                    "cwd": Roost.sessions.data(at, Qt.UserRole + 3),
                    "state": Roost.sessions.data(at, Qt.UserRole + 4)
                };
            }
        }
        return null;
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
        // The help takes every key, Enter included; a confirmation is
        // cancelled by any key but y.
        if (confirm !== null) {
            confirm = null;
            return;
        }
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

    // The title of the row under the cursor, as the list shows it. Read from
    // the model: the delegate of the row may not exist yet. The title is the
    // fifth role of sessionsList from Qt.UserRole (kind, key, agent, id,
    // title); a header has none.
    function cursorTitle() {
        const row = sessionsList.cursorRow;
        if (row < 0) {
            return "";
        }
        return sessionsList.data(sessionsList.index(row, 0), Qt.UserRole + 4) ?? "";
    }

    // Opens the prompt of the footer with its text. The keys then go to the
    // text field, not to the list.
    function openPrompt(kind, ref, text) {
        promptKind = kind;
        promptRef = ref;
        promptInput.text = text;
        promptInput.cursorPosition = text.length;
        promptInput.forceActiveFocus();
    }

    function closePrompt() {
        promptKind = "";
        promptRef = null;
        promptNew = null;
        promptInput.text = "";
        list.forceActiveFocus();
    }

    // Enter: the category or the name goes to the model, the search keeps its
    // query, which is already set. In n the task goes on to the category, and
    // the category starts the session.
    function submitPrompt() {
        const ref = promptRef;
        const text = promptInput.text;
        const kind = promptKind;
        if (kind === "task") {
            const task = text.trim();
            // An empty task is not sent.
            if (task !== "") {
                promptNew = {
                    "agent": promptNew.agent,
                    "cwd": promptNew.cwd,
                    "category": promptNew.category,
                    "task": task
                };
                openPrompt("newCategory", null, promptNew.category);
            }
            return;
        }
        const started = promptNew;
        closePrompt();
        if (kind === "category") {
            sessionsList.setCategory(ref, text.trim());
        } else if (kind === "rename") {
            sessionsList.rename(ref, text);
        } else if (kind === "newCategory") {
            startSession(started.agent, started.cwd, started.task, text.trim());
        }
    }

    // Esc: nothing changes, but the search drops its query.
    function cancelPrompt() {
        if (promptKind === "search") {
            sessionsList.query = "";
        }
        closePrompt();
    }

    // c and e: the prompt for the session under the cursor; on a header there
    // is no session and nothing happens.
    function openSessionPrompt(kind) {
        const ref = sessionsList.cursorSession;
        if (ref === null) {
            return;
        }
        if (kind === "category") {
            openPrompt(kind, ref, sessionsList.cursorCategory ?? "");
        } else {
            openPrompt(kind, ref, cursorTitle());
        }
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
        case Qt.Key_C:
            if (shift) {
                return false;
            }
            if (act) {
                openSessionPrompt("category");
            }
            return true;
        case Qt.Key_E:
            if (shift) {
                return false;
            }
            if (act) {
                openSessionPrompt("rename");
            }
            return true;
        case Qt.Key_Slash:
            if (act) {
                openPrompt("search", null, sessionsList.query);
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

    // The keys of the commands, matched as the keys of the view. A key held
    // down does not ask again. On a header d, x and r do nothing. Whether the
    // key was taken.
    function commandKey(event, key) {
        if ((event.modifiers & ~(Qt.ShiftModifier | Qt.KeypadModifier)) !== 0) {
            return false;
        }
        const shift = (event.modifiers & Qt.ShiftModifier) !== 0;
        const act = !event.isAutoRepeat;
        switch (key) {
        case Qt.Key_D:
            if (shift) {
                return false;
            }
            if (act) {
                askForSession("delete", "Delete");
            }
            return true;
        case Qt.Key_X:
            if (shift) {
                return false;
            }
            if (act) {
                askArchive();
            }
            return true;
        case Qt.Key_R:
            if (act) {
                if (shift) {
                    askRespawnAll();
                } else {
                    askForSession("respawn", "Respawn");
                }
            }
            return true;
        case Qt.Key_N:
            if (shift) {
                return false;
            }
            if (act) {
                openNewSessionPrompt();
            }
            return true;
        }
        return false;
    }

    function isModifierKey(key) {
        return key === Qt.Key_Shift || key === Qt.Key_Control || key === Qt.Key_Alt || key === Qt.Key_AltGr || key === Qt.Key_Meta || key === Qt.Key_Super_L || key === Qt.Key_Super_R || key === Qt.Key_CapsLock || key === Qt.Key_NumLock || key === Qt.Key_ScrollLock;
    }

    // A key while a confirmation waits: y (and the Russian н) runs it with no
    // modifier; any other key cancels it and is taken. A bare modifier and a
    // held key do nothing, so that the key that asked does not cancel itself.
    function confirmKey(event, key) {
        if (isModifierKey(event.key) || event.isAutoRepeat) {
            return;
        }
        const asked = confirm;
        confirm = null;
        if (key === Qt.Key_Y && (event.modifiers & ~Qt.KeypadModifier) === 0) {
            runConfirmed(asked);
        }
    }

    function showNotice(text) {
        notice = text;
        noticeTimer.restart();
    }

    // callback(error, result) of a command `<agent>.<verb>`.
    function runCommand(agent, verb, args, callback) {
        Roost.commands.run(agent + "." + verb, args, callback);
    }

    function failure(error) {
        return error && error.message ? error.message : "the command failed";
    }

    // The session under the cursor as a plain {agent, id} of its own: what the
    // model returns is not kept stable when the edits that follow move the
    // cursor (x archives the session and the cursor leaves it).
    function cursorRef() {
        const ref = sessionsList.cursorSession;
        return ref === null ? null : {
            "agent": ref.agent,
            "id": ref.id
        };
    }

    // d and r ask about the session under the cursor.
    function askForSession(action, verb) {
        const ref = cursorRef();
        if (ref !== null) {
            const title = titleOf(ref);
            confirm = {
                "action": action,
                "ref": ref,
                "title": title,
                "text": verb + " \"" + title + "\"? (y: yes, any other key: no)"
            };
        }
    }

    // x archives and stops; in the archive view it restores, at once.
    function askArchive() {
        const ref = cursorRef();
        if (ref === null) {
            return;
        }
        if (sessionsList.viewArchive) {
            sessionsList.setArchived(ref, false);
            return;
        }
        const title = titleOf(ref);
        confirm = {
            "action": "archive",
            "ref": ref,
            "title": title,
            "text": "Archive and stop \"" + title + "\"? (y: yes, any other key: no)"
        };
    }

    function runningCount() {
        let count = 0;
        const rows = Roost.sessions.rowCount();
        for (let row = 0; row < rows; ++row) {
            if (Roost.sessions.data(Roost.sessions.index(row, 0), Qt.UserRole + 4) === "working") {
                ++count;
            }
        }
        return count;
    }

    // The agents of the listed sessions, each once, in the order of the list.
    function listedAgents() {
        const agents = [];
        const rows = Roost.sessions.rowCount();
        for (let row = 0; row < rows; ++row) {
            const agent = Roost.sessions.data(Roost.sessions.index(row, 0), Qt.UserRole);
            if (agents.indexOf(agent) < 0) {
                agents.push(agent);
            }
        }
        return agents;
    }

    function askRespawnAll() {
        const agents = listedAgents();
        if (agents.length === 0) {
            return;
        }
        confirm = {
            "action": "respawnAll",
            "agents": agents,
            "text": "Respawn all sessions (" + runningCount() + " running)? (y: yes, any other key: no)"
        };
    }

    function runConfirmed(asked) {
        switch (asked.action) {
        case "delete":
            deleteSession(asked.ref, asked.title);
            break;
        case "archive":
            archiveSession(asked.ref, asked.title);
            break;
        case "respawn":
            respawnSession(asked.ref, asked.title);
            break;
        case "respawnAll":
            respawnAll(asked.agents);
            break;
        }
    }

    // After the removal the window must not open the session again, so
    // `selected` goes first. The list forgets it once Roost.sessions has
    // dropped it. The notices name the session by the title it had when the
    // question was asked: it may be gone by the answer.
    function deleteSession(ref, title) {
        if (isSame(selected, ref)) {
            selected = null;
        }
        showNotice("Deleting…");
        runCommand(ref.agent, "remove", {
            "session": ref.id
        }, error => {
            if (error) {
                showNotice("Not deleted: " + failure(error));
                return;
            }
            showNotice("\"" + title + "\" deleted");
            forgetPending = forgetPending.concat([ref]);
            drainForget();
        });
    }

    // Forgets the pending sessions that Roost.sessions no longer lists; not
    // while the list is being replaced or missing, when nothing is known.
    function drainForget() {
        if (resetting || !Roost.sessions.loaded) {
            return;
        }
        const left = [];
        for (const ref of forgetPending) {
            if (listed(ref)) {
                left.push(ref);
            } else {
                sessionsList.forget(ref);
            }
        }
        forgetPending = left;
    }

    // Archived at once; a failed stop leaves it archived and says so.
    function archiveSession(ref, title) {
        sessionsList.setArchived(ref, true);
        if (isSame(selected, ref)) {
            selected = null;
        }
        showNotice("Stopping…");
        runCommand(ref.agent, "stop", {
            "session": ref.id
        }, error => {
            showNotice(error ? "Not stopped: " + failure(error) : "\"" + title + "\" archived and stopped");
        });
    }

    function respawnSession(ref, title) {
        showNotice("Respawning…");
        runCommand(ref.agent, "respawn", {
            "session": ref.id
        }, error => {
            if (error) {
                showNotice("Not respawned: " + failure(error));
                return;
            }
            showNotice("\"" + title + "\" respawned");
            if (isSame(selected, ref)) {
                respawned(ref);
            }
        });
    }

    // One respawnAll for each agent. The open session is offered for reopening
    // whatever its state: the terminal waits `reopenWaitMs` for its attach to
    // end and gives up by itself if the command did not touch it.
    function respawnAll(agents) {
        const open = selected;
        const reopen = open !== null;
        let left = agents.length;
        let failed = "";
        showNotice("Respawning all…");
        for (const agent of agents) {
            runCommand(agent, "respawnAll", undefined, error => {
                if (error && failed === "") {
                    failed = failure(error);
                }
                if (!error && reopen && open.agent === agent && isSame(selected, open)) {
                    respawned(open);
                }
                if (--left === 0) {
                    showNotice(failed === "" ? "Respawned all" : "Not respawned: " + failed);
                }
            });
        }
    }

    // n: the Task prompt. The agent, the directory and the category are taken
    // now, from the cursor, so that a poll while the user types does not
    // change them. A header gives its category; the directory is that of the
    // session under the cursor, else the home directory.
    function openNewSessionPrompt() {
        const ref = sessionsList.cursorSession;
        const data = ref === null ? null : sessionData(ref);
        const category = sessionsList.cursorCategory;
        promptNew = {
            "agent": ref === null ? "" : ref.agent,
            "cwd": data && data.cwd ? data.cwd : homeDirectory(),
            "category": category === null ? "" : category,
            "task": ""
        };
        openPrompt("task", null, "");
    }

    function homeDirectory() {
        return decodeURIComponent(StandardPaths.writableLocation(StandardPaths.HomeLocation).toString().replace(/^file:\/\//, ""));
    }

    // The agent that starts the session: the one of the session under the
    // cursor, else the first plugin, in the order roostd loaded them, that has
    // a command `<agent>.newSession`. callback(agent), "" if there is none.
    function newSessionAgent(agent, callback) {
        if (agent !== "") {
            callback(agent);
            return;
        }
        Roost.call("plugins.list", {}, (error, result) => {
            const suffix = ".newSession";
            for (const plugin of error ? [] : result.plugins) {
                for (const command of (plugin.contributes ?? {}).commands ?? []) {
                    if (command.id.endsWith(suffix)) {
                        callback(command.id.slice(0, command.id.length - suffix.length));
                        return;
                    }
                }
            }
            callback("");
        });
    }

    function startSession(agent, cwd, task, category) {
        showNotice("Starting…");
        newSessionAgent(agent, name => {
            if (name === "") {
                showNotice("Not started: no agent can start a session");
                return;
            }
            runCommand(name, "newSession", {
                "cwd": cwd,
                "prompt": task
            }, (error, result) => {
                if (error) {
                    showNotice("Not started: " + failure(error));
                    return;
                }
                const ref = {
                    "agent": result.agent,
                    "id": result.id
                };
                showNotice("\"" + result.title + "\" started");
                if (category !== "") {
                    sessionsList.setCategory(ref, category);
                }
            });
        });
    }

    // The stored document: read on creation if connected and at every
    // connection (the window forgets its copy when the connection goes),
    // written as the model asks. When the connection goes the list stops
    // writing (storeLost) until the read at the next one: what it holds may
    // be older than the store's document.
    // The answer of a read sent before the connection went comes after it
    // (answers are delivered later, the loss at once): it is of the old
    // connection, and is dropped, or it would end the wait for the new read.
    property int storeGeneration: 0

    function readStore() {
        const generation = storeGeneration;
        Roost.store.get("sessions", (error, result) => {
            if (generation !== storeGeneration) {
                return;
            }
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

    Timer {
        id: noticeTimer

        interval: root.noticeMs
        onTriggered: root.notice = ""
    }

    Connections {
        target: Roost

        function onConnectedChanged() {
            if (Roost.connected) {
                root.readStore();
            } else {
                root.storeGeneration++;
                sessionsList.storeLost();
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
            root.drainForget();
        }
    }

    function settle() {
        resetting = false;
        dropGone();
        restoreOpened();
        drainForget();
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
            } else if (root.confirm !== null) {
                root.confirmKey(event, key);
                event.accepted = true;
            } else if (event.key === Qt.Key_Down || (key === Qt.Key_J && plain)) {
                sessionsList.move(1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Up || (key === Qt.Key_K && plain)) {
                sessionsList.move(-1);
                event.accepted = true;
            } else if (event.key === Qt.Key_Escape && plain && sessionsList.query !== "") {
                // Esc drops the search the Enter of the prompt kept.
                sessionsList.query = "";
                event.accepted = true;
            } else if (root.viewKey(event, key) || root.commandKey(event, key)) {
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

            // The icon of the agent of the session, at the left of the title
            // line; the user's folder agent-icons overrides the bundled one.
            Image {
                id: agentIcon

                // The agent whose file of the user failed to load: its row
                // shows the bundled icon instead of an empty gap.
                property string failedAgent: ""
                readonly property bool bundledOnly: !row.isHeader && failedAgent === row.model.agent

                objectName: "agentIcon"
                visible: !row.isHeader
                x: 26
                y: Math.round(6 + (titleText.implicitHeight - height) / 2)
                width: 16
                height: 16
                fillMode: Image.PreserveAspectFit
                sourceSize.height: 32
                smooth: false
                asynchronous: false
                source: row.isHeader ? "" : bundledOnly ? sessionsList.bundledAgentIcon(row.model.agent) : sessionsList.agentIcon(row.model.agent)
                onStatusChanged: {
                    if (status === Image.Error && !row.isHeader && !bundledOnly)
                        failedAgent = row.model.agent;
                }
            }

            Column {
                visible: !row.isHeader
                anchors.fill: parent
                anchors.leftMargin: 48
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
    // places: the problem of the store; the confirmation of a command; the prompt;
    // the notice of a command.
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

        // A command that waits for y.
        Text {
            objectName: "sessionsConfirm"
            width: parent.width
            leftPadding: 8
            rightPadding: 8
            bottomPadding: 4
            visible: root.confirm !== null
            color: "#ffffff"
            wrapMode: Text.Wrap
            text: root.confirm === null ? "" : root.confirm.text
        }

        // The prompt, one for the whole footer: the search, the category and
        // the name of a session, and the Task and the Category of a new one.
        // Enter submits, Esc cancels; the text is typed as it is, not through
        // bindingKey, so that any layout types its letters.
        Column {
            objectName: "sessionsPrompt"
            width: parent.width
            visible: root.promptKind !== ""
            bottomPadding: 4

            Item {
                width: parent.width
                height: promptInput.implicitHeight + 4

                Text {
                    id: promptLabel

                    objectName: "sessionsPromptLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    color: "#a0a0a0"
                    text: root.promptLabels[root.promptKind] ?? ""
                }

                TextInput {
                    id: promptInput

                    objectName: "sessionsPromptInput"
                    anchors.left: promptLabel.right
                    anchors.leftMargin: 6
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    clip: true
                    color: root.textColor
                    selectionColor: "#3d5a80"
                    selectByMouse: true

                    // The search is live: the list follows every edit.
                    onTextEdited: {
                        if (root.promptKind === "search") {
                            sessionsList.query = text;
                        }
                    }
                    onAccepted: root.submitPrompt()
                    // A tap on the list takes the keyboard away: the search
                    // keeps its query, the others are dropped. Not when the
                    // whole window lost the focus.
                    onActiveFocusChanged: {
                        if (!activeFocus && root.promptKind !== "" && Window.active) {
                            if (root.promptKind === "search") {
                                root.closePrompt();
                            } else {
                                root.cancelPrompt();
                            }
                        }
                    }

                    Keys.onEscapePressed: root.cancelPrompt()
                    // Tab does not leave the field: it would take the
                    // keyboard to the list and drop what was typed.
                    Keys.onTabPressed: event => event.accepted = true
                    Keys.onBacktabPressed: event => event.accepted = true
                    // A digit 1-9 in an empty category field (of `c` or of
                    // `n`) picks the category with that number.
                    Keys.onPressed: event => {
                        const index = event.key - Qt.Key_1;
                        const plain = (event.modifiers & ~Qt.KeypadModifier) === Qt.NoModifier;
                        if (root.promptPicksCategory && text === "" && plain && index >= 0 && index < 9 && index < sessionsList.knownCategories.length) {
                            text = sessionsList.knownCategories[index];
                            root.submitPrompt();
                            event.accepted = true;
                        }
                    }
                }
            }

            // The categories the digits pick.
            Text {
                objectName: "sessionsPromptHint"
                width: parent.width
                leftPadding: 8
                rightPadding: 8
                visible: root.promptPicksCategory && promptInput.text === "" && sessionsList.knownCategories.length > 0
                color: "#808080"
                elide: Text.ElideRight
                text: sessionsList.knownCategories.slice(0, 9).map((name, i) => (i + 1) + " " + name).join("   ")
            }
        }

        // The outcome of a command, for `noticeMs`.
        Text {
            objectName: "sessionsNotice"
            width: parent.width
            leftPadding: 8
            rightPadding: 8
            bottomPadding: 4
            visible: root.notice !== ""
            color: "#a0a0a0"
            wrapMode: Text.Wrap
            text: root.notice
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
                text: "View\na   next tab (All, Active, Unread)\nv   sessions or archive\nm   compact: the second line only under the cursor\nz   fold the category under the cursor\nShift+z   unfold all\np   pin the category under the cursor\n/   search, live (Enter keeps it, Esc clears it)\nc   category of the session under the cursor (1-9 picks a known one, empty clears)\ne   rename the session under the cursor (empty restores the agent's title)"
            }

            Text {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Sessions\nd   delete the session under the cursor (asks)\nx   archive and stop it (asks); in the archive: restore it\nr   respawn it (asks)\nShift+r   respawn all sessions (asks)\nn   new session: Task, then Category"
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
