import QtQuick
import QtQuick.Controls
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
// unfolds it. The head shows the title (Sessions, or Archive), the button
// "+ New", the search field and the tabs with their counts; the keys a, v, m,
// z, Z, p, ?, < and > act on the view (the help overlay lists them). The key /
// moves the keyboard to the search field (Esc clears the query, Enter or Down
// keeps it and returns to the list). The keys c and e open the prompt in the
// footer: the category and the name of the session under the cursor; n opens
// it for the task and the category of a new session. "+ New" opens the form of
// a new session in place of the list: a task, a name and a category; its draft lives
// in the panel. The keys d, x, r and R run the commands of the agent of the
// session (`<agent>.<verb>`, Roost.commands); the footer asks for a
// confirmation (y) and shows the outcome as a notice.
FocusScope {
    id: root

    // A SessionRef, {agent, id}, or null; the output of the panel.
    property var selected: null

    // The narrowest the window may make the list: the width its rows and head
    // are laid out for (the shell reads it, and `<` stops at it too).
    readonly property int minimumWidth: 200

    // The id of this panel in the layout of the window, set by the window
    // before Component.onCompleted: the view of the list (query, tab, archive,
    // compact, the open session) is kept in the document `sessions` under it,
    // so that two lists in a window keep their own.
    property string instanceId: ""

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

    // The prompt of the footer: "category", "rename", "task" and
    // "newCategory" (the two steps of n), or "" when there is none. The session
    // a category or a name is for is taken when the prompt opens (`promptRef`),
    // and the agent, the directory and the task of a new one are (`promptNew`),
    // whatever the cursor and the polls do while the user types.
    property string promptKind: ""
    property var promptRef: null
    property var promptNew: null
    // The start of n: while it runs Enter does nothing; a failure returns the
    // prompt to the task, kept, with `promptProblem`. `promptStart` numbers it,
    // as `newStart` does.
    property bool promptStarting: false
    property string promptProblem: ""
    property int promptStart: 0
    readonly property var promptLabels: ({
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

    // The colours come from Roost.theme: the categories by the `colorIndex`
    // of the model.
    readonly property var categoryColors: Roost.theme.categories
    readonly property color textColor: Roost.theme.text
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

    // The text with its control characters (a newline, a tab, the line and
    // paragraph separators) as spaces: a label shows each of them as a break
    // or a gap, and the row of a list is one line.
    function oneLine(text) {
        return String(text).replace(/[\u0000-\u001f\u007f-\u009f\u2028\u2029]/g, " ");
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

    // The form of a new session ("+ New"), shown in place of the list. Its
    // draft is kept here, in the panel, while the form is open, whatever
    // takes the keyboard: Cancel, Esc and Create end it.
    property bool newOpen: false
    // The task, the first message to the agent; the name is optional.
    property string draftTask: ""
    property string draftName: ""
    // The category of the draft: a known one, "" for none, or a new one typed
    // in `draftNewName` when `draftNewCategory` is set.
    property string draftCategory: ""
    property bool draftNewCategory: false
    property string draftNewName: ""
    // The agent and the directory of the session, taken from the cursor when
    // the form opens, as n does.
    property var draftTarget: null
    // The start of the form: while it runs Create does nothing; a failure
    // leaves the form with its draft and `newProblem` says why. `newStart`
    // numbers the start: a Cancel ends the form's claim on it (a late answer
    // is not the form's any more).
    property bool newStarting: false
    property string newProblem: ""
    property int newStart: 0

    // The entries of the category list of the form: none, the known ones, a
    // new one. The category of the draft stays among them while the form holds
    // it, even when no session has it any more (the list was replaced, or its
    // last session left): the box shows what Create applies.
    readonly property var categoryChoices: {
        const known = sessionsList.knownCategories;
        const kept = draftCategory !== "" && !draftNewCategory && known.indexOf(draftCategory) < 0 ? [draftCategory] : [];
        return ["Uncategorized"].concat(known, kept, ["New category…"]);
    }

    // The keyboard goes to the search field, with its query to edit.
    function focusSearch() {
        searchInput.forceActiveFocus();
        searchInput.cursorPosition = searchInput.text.length;
    }

    // Esc in the field: the query goes, the list has the keyboard.
    function clearSearch() {
        sessionsList.query = "";
        list.forceActiveFocus();
    }

    function openNewForm() {
        if (!newOpen) {
            // A question or a prompt of the list is dropped, not left for the
            // list that is hidden: the prompt would hand the keyboard to it
            // when the task takes it, and the question would wait for a key
            // that never comes.
            confirm = null;
            resetPrompt();
            const base = newSessionBase();
            draftTask = "";
            draftName = "";
            draftCategory = base.category;
            draftNewCategory = false;
            draftNewName = "";
            draftTarget = {
                "agent": base.agent,
                "cwd": base.cwd
            };
            newStarting = false;
            newProblem = "";
            newOpen = true;
        }
        newTaskField.forceActiveFocus();
    }

    // Cancel and Esc: the draft goes, the list has the keyboard. A start that
    // is still running goes on: a session it starts is listed, a failure of it
    // comes to the footer. `takeKeyboard` false (the answer of a start, which
    // comes whenever): the list is only the focus of its panel, the keyboard
    // stays where the user took it.
    function closeNewForm(takeKeyboard = true) {
        newOpen = false;
        newStarting = false;
        newStart++;
        draftTarget = null;
        if (takeKeyboard) {
            list.forceActiveFocus();
        } else {
            list.focus = true;
        }
    }

    function canCreate() {
        return draftTask.trim() !== "" && (!draftNewCategory || draftNewName.trim() !== "");
    }

    // Create: the session starts as n starts it, with the task as its first
    // message. A name, when there is one, becomes its title; without one the
    // title stays the agent's. The form stays open until the agent answers:
    // then it closes, or, on a failure, it stays with the draft and the
    // reason. A second Create while the start runs does nothing.
    function createFromForm() {
        if (!newOpen || newStarting || !canCreate()) {
            return;
        }
        const target = draftTarget;
        const task = draftTask.trim();
        const name = draftName.trim();
        const category = draftNewCategory ? draftNewName.trim() : draftCategory;
        const start = ++newStart;
        // The disabled button drops the keyboard: Esc needs it in the form.
        if (newCreate.activeFocus) {
            newTaskField.forceActiveFocus();
        }
        newStarting = true;
        newProblem = "";
        startSession(target.agent, target.cwd, task, category, name, problem => {
            if (start !== newStart) {
                // Cancelled meanwhile: the form is not waiting for this.
                if (problem !== "") {
                    showNotice(problem);
                }
                return;
            }
            newStarting = false;
            if (problem === "") {
                // The keyboard moves only if the form has it: the user may
                // have gone to another panel while the agent started.
                closeNewForm(root.activeFocus);
            } else {
                newProblem = problem;
            }
        });
    }

    // Opens the prompt of the footer with its text. The keys then go to the
    // text field, not to the list.
    function openPrompt(kind, ref, text) {
        promptKind = kind;
        promptRef = ref;
        promptStarting = false;
        promptStart++;
        promptProblem = "";
        promptInput.text = text;
        promptInput.cursorPosition = text.length;
        promptInput.forceActiveFocus();
    }

    // The prompt ends; the keyboard stays where it is.
    function resetPrompt() {
        promptKind = "";
        promptRef = null;
        promptNew = null;
        promptStarting = false;
        promptProblem = "";
        promptStart++;
        promptInput.text = "";
    }

    function closePrompt() {
        resetPrompt();
        list.forceActiveFocus();
    }

    // Enter: the category or the name goes to the model. In n the task goes on to the category, and
    // the category starts the session: the prompt stays until the agent
    // answers, and on a failure it goes back to the task, kept, with the
    // reason.
    function submitPrompt() {
        if (promptStarting) {
            return;
        }
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
        if (kind === "newCategory") {
            startFromPrompt(text.trim());
            return;
        }
        closePrompt();
        if (kind === "category") {
            sessionsList.setCategory(ref, text.trim());
        } else if (kind === "rename") {
            sessionsList.rename(ref, text);
        }
    }

    function startFromPrompt(category) {
        const started = promptNew;
        const start = ++promptStart;
        promptStarting = true;
        promptProblem = "";
        startSession(started.agent, started.cwd, started.task, category, "", problem => {
            if (start !== promptStart) {
                // Cancelled meanwhile: the prompt is not waiting for this.
                if (problem !== "") {
                    showNotice(problem);
                }
                return;
            }
            if (problem === "") {
                closePrompt();
                return;
            }
            // The task and the category come back as typed.
            promptNew = {
                "agent": started.agent,
                "cwd": started.cwd,
                "category": category,
                "task": started.task
            };
            openPrompt("task", null, started.task);
            promptProblem = problem;
        });
    }

    // Esc: nothing changes.
    function cancelPrompt() {
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
    // layout may need for them (on the Russian one ? is Shift on the key that
    // is the US slash). Ctrl, Alt or Meta: not a list key. Whether the key was
    // taken.
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
                focusSearch();
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
                askForSession("delete", "remove", "Delete");
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
                    askForSession("respawn", "respawn", "Respawn");
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

    // The code of the error of a command whose connection to roostd went away
    // before the answer (protocol::EErrorCode::ConnectionLost).
    readonly property int connectionLost: -32002

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

    // d and r ask only when the agent under the cursor offers the command.
    function askForSession(action, verb, label) {
        const ref = cursorRef();
        if (ref === null) {
            return;
        }
        const title = titleOf(ref);
        agentsWithCommand(verb, agents => {
            if (confirm !== null || promptKind !== "") {
                return;
            }
            if (!agents.has(ref.agent)) {
                showNotice(label + " unavailable for " + ref.agent);
                return;
            }
            confirm = {
                "action": action,
                "ref": ref,
                "title": title,
                "text": label + " \"" + title + "\"? (y: yes, any other key: no)"
            };
        });
    }

    // Commands are optional contributions, independent of terminal support.
    // `onError(message)`, when given, takes the failure of the list in place of
    // the notice.
    function agentsWithCommand(verb, callback, onError) {
        Roost.call("plugins.list", {}, (error, result) => {
            if (error) {
                if (onError) {
                    onError(failure(error));
                } else {
                    showNotice(failure(error));
                }
                return;
            }
            const suffix = "." + verb;
            // A command of a plugin that is no agent (`sessions.newSession`
            // is the panel's own) is no agent's command.
            const known = new Set();
            for (const plugin of result.plugins) {
                for (const agent of (plugin.contributes ?? {}).agents ?? []) {
                    known.add(agent.id);
                }
            }
            const agents = new Set();
            for (const plugin of result.plugins) {
                for (const command of (plugin.contributes ?? {}).commands ?? []) {
                    const agent = command.id.endsWith(suffix) ? command.id.slice(0, -suffix.length) : "";
                    if (known.has(agent)) {
                        agents.add(agent);
                    }
                }
            }
            callback(agents);
        });
    }

    // x archives a row in Roost and stops it if the adapter can do that.
    // In the archive view it restores the row at once.
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
        agentsWithCommand("stop", agents => {
            if (confirm !== null || promptKind !== "") {
                return;
            }
            const stop = agents.has(ref.agent);
            confirm = {
                "action": "archive",
                "ref": ref,
                "title": title,
                "stop": stop,
                "text": (stop ? "Archive and stop " : "Archive ") + "\"" + title + "\"? (y: yes, any other key: no)"
            };
        });
    }

    function runningCount(agents) {
        let count = 0;
        const rows = Roost.sessions.rowCount();
        for (let row = 0; row < rows; ++row) {
            const index = Roost.sessions.index(row, 0);
            if (agents.has(Roost.sessions.data(index, Qt.UserRole)) && Roost.sessions.data(index, Qt.UserRole + 4) === "working") {
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
        agentsWithCommand("respawnAll", supported => {
            if (confirm !== null || promptKind !== "") {
                return;
            }
            const agents = listedAgents().filter(agent => supported.has(agent));
            if (agents.length === 0) {
                return;
            }
            confirm = {
                "action": "respawnAll",
                "agents": agents,
                "text": "Respawn all sessions (" + runningCount(supported) + " running)? (y: yes, any other key: no)"
            };
        });
    }

    function runConfirmed(asked) {
        switch (asked.action) {
        case "delete":
            deleteSession(asked.ref, asked.title);
            break;
        case "archive":
            archiveSession(asked.ref, asked.title, asked.stop);
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
    function archiveSession(ref, title, stop) {
        sessionsList.setArchived(ref, true);
        if (isSame(selected, ref)) {
            selected = null;
        }
        if (!stop) {
            showNotice("\"" + title + "\" archived");
            return;
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

    // n opens the prompt immediately and keeps the cursor's directory and
    // category. The agent is chosen when the completed prompt is submitted.
    function openNewSessionPrompt() {
        const base = newSessionBase();
        promptNew = {
            "agent": base.agent,
            "cwd": base.cwd,
            "category": base.category,
            "task": ""
        };
        openPrompt("task", null, "");
    }

    // The agent, the directory and the category of a new session: those of the
    // session under the cursor, the home directory without one.
    function newSessionBase() {
        const ref = sessionsList.cursorSession;
        const data = ref === null ? null : sessionData(ref);
        const category = sessionsList.cursorCategory;
        return {
            "agent": ref === null ? "" : ref.agent,
            "cwd": data && data.cwd ? data.cwd : homeDirectory(),
            "category": category === null ? "" : category
        };
    }

    function homeDirectory() {
        return decodeURIComponent(StandardPaths.writableLocation(StandardPaths.HomeLocation).toString().replace(/^file:\/\//, ""));
    }

    // `title`, when given, is the title of the session once it is listed.
    // `done(problem)` is called once, when the agent has answered: with "" for a
    // session that started, else with the reason it did not (the caller shows
    // it).
    function startSession(preferredAgent, cwd, task, category, title, done) {
        agentsWithCommand("newSession", agents => {
            if (agents.size === 0) {
                done("No agent can start a session");
                return;
            }
            const agent = agents.has(preferredAgent) ? preferredAgent : agents.values().next().value;
            runCommand(agent, "newSession", {
                "cwd": cwd,
                "prompt": task
            }, (error, result) => {
                if (error) {
                    // A connection lost under the command: the agent may have
                    // started the session, and a second start would repeat it.
                    done((error.code === connectionLost ? "May have started: " : "Not started: ") + failure(error));
                    return;
                }
                if (!result || typeof result.id !== "string" || result.id === "") {
                    done("Not started: " + agent + " answered without a session id");
                    return;
                }
                done("");
                const ref = {
                    "agent": result.agent,
                    "id": result.id
                };
                showNotice("\"" + result.title + "\" started");
                if (category !== "") {
                    sessionsList.setCategory(ref, category);
                }
                if (title) {
                    sessionsList.rename(ref, title);
                }
            });
        }, message => done("Not started: " + message));
    }

    // The stored document: read on creation if connected and at every
    // connection (the window forgets its copy when the connection goes),
    // written as the model asks, by a merge patch of what it changed (the
    // document is shared with other lists and writers). When the connection
    // goes the list stops writing (storeLost) until the read at the next one:
    // what it holds may be older than the store's document; its edits made
    // meanwhile are kept over what the read brings.
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
        instanceId: root.instanceId
        source: Roost.sessions
        sourceLoaded: Roost.sessions.loaded

        onSaveRequested: patch => {
            Roost.store.patch("sessions", patch, error => {
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
        objectName: "sessionsBackground"
        anchors.fill: parent
        // The corners of the card the panel sits in.
        radius: Roost.theme.radiusCard
        color: Roost.theme.panelBackground
    }

    Connections {
        target: Roost.sessions

        function onListed(agent) {
            sessionsList.sourceListed(agent);
        }

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

    // The controls of the panel, in the light style (docs/design/18-style.md).
    // The plugin does not import them from the shell: they follow the same
    // tokens and sizes as FormButton, FormField and FormComboBox of the shell.
    component PanelButton: Button {
        id: control

        // The main button: the accepting one.
        property bool primary: false

        // At least 34, higher with the font.
        implicitHeight: Math.max(34, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: 12
        rightPadding: 12
        font: Roost.theme.rowFont

        Keys.onReturnPressed: {
            if (control.enabled) {
                control.clicked();
            }
        }
        Keys.onEnterPressed: {
            if (control.enabled) {
                control.clicked();
            }
        }

        contentItem: Text {
            text: control.text
            font: control.font
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !control.enabled ? Roost.theme.textMuted : control.primary ? Roost.theme.primaryText : control.hovered ? Roost.theme.text : Roost.theme.textSecondary
        }

        background: Rectangle {
            implicitWidth: 64
            radius: Roost.theme.radiusControl
            color: !control.enabled ? Roost.theme.controlBackground : control.primary ? (control.hovered || control.down ? Roost.theme.primaryHover : Roost.theme.primary) : (control.hovered || control.down ? Roost.theme.hoverBackground : Roost.theme.surface)
            border.width: 1
            border.color: control.primary && control.enabled ? color : Roost.theme.line

            // The ring of the focus, outside with a gap of 2 px.
            Rectangle {
                anchors.fill: parent
                anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                visible: control.visualFocus
                radius: Roost.theme.radiusControl + 2 + Roost.theme.cursorRingWidth
                color: "transparent"
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }
        }
    }

    // The link back to the list: text only, the ring of the focus around it.
    component PanelLink: Button {
        id: link

        implicitHeight: Math.max(28, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: 4
        rightPadding: 4
        font: Roost.theme.rowFont

        Keys.onReturnPressed: link.clicked()
        Keys.onEnterPressed: link.clicked()

        contentItem: Text {
            text: link.text
            font: link.font
            verticalAlignment: Text.AlignVCenter
            color: link.hovered ? Roost.theme.text : Roost.theme.textSecondary
        }

        background: Rectangle {
            radius: Roost.theme.radiusSmall
            color: "transparent"
            border.width: link.visualFocus ? Roost.theme.cursorRingWidth : 0
            border.color: Roost.theme.focus
        }
    }

    component PanelField: TextField {
        implicitHeight: Math.max(38, contentHeight + topPadding + bottomPadding)
        leftPadding: 12
        rightPadding: 12
        font: Roost.theme.uiFont
        color: Roost.theme.text
        placeholderTextColor: Roost.theme.textMuted
        selectionColor: Roost.theme.selection
        selectedTextColor: Roost.theme.textStrong

        background: Rectangle {
            radius: Roost.theme.radiusControl
            color: Roost.theme.inputBackground
            border.width: parent.activeFocus ? Roost.theme.cursorRingWidth : 1
            border.color: parent.activeFocus ? Roost.theme.focus : Roost.theme.line
        }
    }

    component PanelComboBox: ComboBox {
        id: combo

        implicitHeight: Math.max(38, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: 12
        font: Roost.theme.uiFont

        delegate: ItemDelegate {
            required property int index

            width: ListView.view ? ListView.view.width : combo.width
            implicitHeight: Math.max(34, implicitContentHeight + topPadding + bottomPadding)
            leftPadding: 12
            highlighted: combo.highlightedIndex === index
            font: Roost.theme.rowFont
            text: combo.textAt(index)

            contentItem: Text {
                text: parent.text
                font: parent.font
                elide: Text.ElideRight
                verticalAlignment: Text.AlignVCenter
                color: Roost.theme.text
            }

            background: Rectangle {
                radius: Roost.theme.radiusControl
                color: parent.highlighted ? Roost.theme.hoverBackground : "transparent"
            }
        }

        background: Rectangle {
            radius: Roost.theme.radiusControl
            color: Roost.theme.inputBackground
            border.width: 1
            border.color: Roost.theme.line

            Rectangle {
                anchors.fill: parent
                anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                visible: combo.visualFocus
                radius: Roost.theme.radiusControl + 2 + Roost.theme.cursorRingWidth
                color: "transparent"
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }
        }

        popup: Popup {
            y: combo.height + 2
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + 2 * padding, 300)
            padding: 4

            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                spacing: Roost.theme.rowGap
            }

            background: Rectangle {
                radius: Roost.theme.radiusControl
                color: Roost.theme.surface
                border.width: 1
                border.color: Roost.theme.line
            }
        }
    }

    // The head: the title and "+ New", the search field, and the filter chips
    // with the counts of the view.
    Column {
        id: head

        objectName: "sessionsHead"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        visible: !root.newOpen
        // The rows are cut at the panel edge, not drawn over its neighbour.
        clip: true

        // 76 high, or as high as the two lines with the padding above and below
        // when a large font of the interface needs more (as the head of the
        // terminal).
        Item {
            width: parent.width
            height: Math.max(76, titleColumn.height + 2 * Roost.theme.panelPadding)

            // The title, and under it the label of the keyboard while the
            // panel has it. The label keeps its place when it is not shown, so
            // that the title does not move.
            Column {
                id: titleColumn

                objectName: "sessionsTitleColumn"
                x: 20
                anchors.verticalCenter: parent.verticalCenter
                // The title takes what the button leaves.
                width: Math.min(Math.max(title.implicitWidth, inputLabel.implicitWidth), Math.max(0, newButton.x - 8 - x))
                spacing: 3

                Label {
                    id: title

                    objectName: "sessionsTitle"
                    width: parent.width
                    elide: Text.ElideRight
                    color: root.textColor
                    font: Roost.theme.titleFont
                    text: sessionsList.viewArchive ? "Archive" : "Sessions"
                }

                // "Input in list": a dot of `focus` and the words, 11 semibold,
                // only while the panel has the keyboard (its active focus: where
                // the keys go now; the ring of the card follows `lastFocused`
                // of the layout and stays while a popup has the keyboard), and
                // only if it fits left of "+ New": in a narrow list it goes, as
                // in the head of the terminal.
                Row {
                    id: inputLabel

                    objectName: "sessionsFocusLabel"
                    // Not `visible`: the title would move with it.
                    opacity: root.activeFocus && implicitWidth <= Math.max(0, newButton.x - 8 - titleColumn.x) ? 1 : 0
                    spacing: 6

                    Rectangle {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 8
                        height: 8
                        radius: 4
                        color: Roost.theme.focus
                    }

                    Label {
                        objectName: "sessionsFocusLabelText"
                        font: Roost.theme.withWeight(Roost.theme.metaFont, Font.DemiBold)
                        color: Roost.theme.focus
                        text: "Input in list"
                    }
                }
            }

            PanelButton {
                id: newButton

                objectName: "sessionsNewButton"
                primary: true
                anchors.right: parent.right
                anchors.rightMargin: Roost.theme.panelPadding
                anchors.verticalCenter: parent.verticalCenter
                text: "+ New"
                onClicked: root.openNewForm()
            }
        }

        // The search field: a hairline of `line`, which the ring of the focus
        // replaces while the field has the keyboard.
        Item {
            width: parent.width
            height: 62

            Rectangle {
                objectName: "sessionsSearch"
                x: Roost.theme.panelPadding
                y: 4
                width: parent.width - 2 * Roost.theme.panelPadding
                height: 40
                radius: Roost.theme.radiusControl
                color: Roost.theme.controlBackground
                border.width: searchInput.activeFocus ? Roost.theme.cursorRingWidth : 1
                border.color: searchInput.activeFocus ? Roost.theme.focus : Roost.theme.line

                // A tap anywhere on the frame, the icon and the margins
                // included, gives the keyboard to the field. It lies under the
                // field, whose own line of text takes its taps itself (the
                // cursor goes where the tap is).
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.IBeamCursor
                    onClicked: root.focusSearch()
                }

                Label {
                    objectName: "sessionsSearchIcon"
                    x: 13
                    anchors.verticalCenter: parent.verticalCenter
                    font: Roost.theme.headingFont
                    color: Roost.theme.textMuted
                    text: "⌕"
                }

                Label {
                    objectName: "sessionsSearchPlaceholder"
                    x: 39
                    width: parent.width - x - 12
                    anchors.verticalCenter: parent.verticalCenter
                    visible: searchInput.text === "" && searchInput.preeditText === ""
                    elide: Text.ElideRight
                    color: Roost.theme.textMuted
                    font: Roost.theme.uiFont
                    text: "Search sessions"
                }

                // The query is the model's: the field shows it and every edit
                // sets it, so that the list follows at once. The text is typed
                // as it is, not through bindingKey, so that any layout types
                // its letters.
                TextInput {
                    id: searchInput

                    objectName: "sessionsSearchInput"
                    x: 39
                    width: parent.width - x - 12
                    anchors.verticalCenter: parent.verticalCenter
                    clip: true
                    // A TextInput does not inherit the font of the window.
                    font: Roost.theme.uiFont
                    color: root.textColor
                    selectionColor: Roost.theme.selection
                    selectedTextColor: Roost.theme.textStrong
                    selectByMouse: true
                    text: sessionsList.query

                    onTextEdited: sessionsList.query = text
                    // Enter and Down keep the query and give the list the
                    // keyboard; Esc drops the query first.
                    onAccepted: list.forceActiveFocus()
                    Keys.onDownPressed: list.forceActiveFocus()
                    Keys.onEscapePressed: root.clearSearch()
                    // Tab does not leave the field.
                    Keys.onTabPressed: event => event.accepted = true
                    Keys.onBacktabPressed: event => event.accepted = true
                }
            }
        }

        // The current chip and the open title are DemiBold: `font:` cannot be
        // combined with `font.weight:` on one item, and a Control around them
        // would take the right button that goes to the menu of the panel, so the
        // font comes whole from `Roost.theme.withWeight`.
        Item {
            x: Roost.theme.panelPadding
            implicitWidth: chips.implicitWidth
            implicitHeight: chips.implicitHeight

            Row {
                id: chips

                spacing: Roost.theme.rowGap

                Repeater {
                    model: root.tabs

                    // A filter chip; a tap on it selects the tab.
                    delegate: Label {
                        id: tab

                        required property var modelData
                        readonly property bool current: sessionsList.tab === modelData.name

                        objectName: "sessionsTab"
                        height: Math.max(32, implicitHeight)
                        // The text of a chip starts where the title does (20),
                        // and three chips with counts of two digits fit the
                        // panel of 200.
                        leftPadding: 4
                        rightPadding: 4
                        verticalAlignment: Text.AlignVCenter
                        color: current ? Roost.theme.focus : Roost.theme.textSecondary
                        font: current ? Roost.theme.withWeight(Roost.theme.metaFont, Font.DemiBold) : Roost.theme.metaFont
                        text: modelData.label + " " + (sessionsList.counts[modelData.name] ?? 0)
                        background: Rectangle {
                            radius: Roost.theme.radiusSmall
                            color: tab.current ? Roost.theme.selectedBackground : tabHover.hovered ? Roost.theme.hoverBackground : "transparent"
                        }

                        HoverHandler {
                            id: tabHover
                        }

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
    }

    ListView {
        id: list

        objectName: "sessionList"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: Roost.theme.panelPadding
        anchors.rightMargin: Roost.theme.panelPadding
        anchors.top: head.bottom
        anchors.topMargin: 8
        // The list ends above the list of unavailable agents, and above the
        // footer.
        anchors.bottom: unavailable.visible ? unavailable.top : footer.top
        visible: !root.newOpen
        focus: true
        clip: true
        model: sessionsList
        spacing: Roost.theme.rowGap
        highlightMoveDuration: 0
        // The cursor is drawn by the delegate, which combines it with the
        // other signs of its row; the view only keeps the row in sight.
        highlight: null

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
            // The signs of a row (docs/design/18-style.md), each on its own:
            // the cursor, an unread session, a session that waits.
            readonly property bool isCursor: index === list.currentIndex
            readonly property bool isUnread: !isHeader && model.unread
            readonly property bool isWaiting: !isHeader && (model.state === "blocked" || model.state === "needs_input")
            // The second line of a session; in the compact view only under the
            // cursor.
            readonly property bool showsDetail: !isHeader && (!sessionsList.compact || index === sessionsList.cursorRow)

            // The cursor ring is drawn only while the list has the keyboard.
            readonly property bool ringShown: isCursor && list.activeFocus
            // The room of a row above and below its text.
            readonly property real textPadding: 6
            readonly property real inset: Math.max(0, Math.min(15, (height - 12) / 2))

            width: ListView.view.width
            // A row is as high as its text needs and not lower than the row of
            // the style (`rowHeight`, a header 34): a large interface font
            // makes the row taller, not the next one overlap. In the compact
            // view a session row is as high as its text: the title alone, or
            // with the second line under the cursor.
            height: isHeader ? Math.max(34, headerText.implicitHeight + 8) : sessionsList.compact ? lines.implicitHeight + 2 * textPadding : Math.max(Roost.theme.rowHeight, lines.implicitHeight + 2 * textPadding)

            // The line between the pinned categories and the rest: on the
            // top edge of the first unpinned header, declared before the
            // background and the ring so they cover it, and it takes no height.
            Rectangle {
                objectName: "pinnedRule"
                visible: row.isHeader && row.model.startsUnpinned
                x: 12
                y: 0
                width: parent.width - 24
                height: 1
                color: Roost.theme.line
            }

            // The ground of the row: the open one is `selectedBackground`
            // whether the cursor is on it or not; the cursor is the focused or
            // unfocused tone of the list; a row under the mouse is lighter.
            Rectangle {
                objectName: "rowBackground"
                anchors.fill: parent
                radius: Roost.theme.radiusControl
                color: row.isOpen ? Roost.theme.selectedBackground : row.isCursor ? (list.activeFocus ? Roost.theme.cursorFocused : Roost.theme.cursorUnfocused) : rowHover.hovered && !row.isHeader ? Roost.theme.hoverBackground : "transparent"
            }

            HoverHandler {
                id: rowHover
            }

            // The open session: a bar at the left edge, moved right of the
            // ring while the ring is drawn.
            Rectangle {
                objectName: "openBar"
                visible: row.isOpen
                x: row.ringShown ? 4 : 1
                y: row.inset
                width: 3
                height: parent.height - 2 * row.inset
                radius: 2
                color: Roost.theme.accent
            }

            // The cursor: a ring inside the row, only while the list has the
            // keyboard focus.
            Rectangle {
                objectName: "cursorRing"
                visible: row.ringShown
                anchors.fill: parent
                radius: Roost.theme.radiusControl
                color: "transparent"
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }

            // A header: the folded or unfolded mark and the category
            // ("Uncategorized" for none), the mark of its colour after the
            // name, and at the right the marks of a pinned category and of the
            // unread sessions of a folded one, then the number of its
            // sessions.
            Label {
                id: headerText

                objectName: "headerTitle"
                visible: row.isHeader
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, Math.max(0, marks.x - 8 - x - (categoryMark.visible ? categoryMark.width + 6 : 0)))
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Roost.theme.textSecondary
                font: Roost.theme.metaFont
                text: row.isHeader ? (row.model.folded ? "▸ " : "▾ ") + (row.model.category === "" ? "Uncategorized" : root.oneLine(row.model.category)) : ""
            }

            // The colour of the category, a dot next to its name.
            Rectangle {
                id: categoryMark

                objectName: "categoryMark"
                visible: row.isHeader && row.model.colorIndex >= 0
                x: headerText.x + headerText.width + 6
                anchors.verticalCenter: parent.verticalCenter
                width: 8
                height: 8
                radius: 4
                color: visible ? root.categoryColors[row.model.colorIndex % root.categoryColors.length] : "transparent"
            }

            Row {
                id: marks

                visible: row.isHeader
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 8

                Label {
                    objectName: "pinMark"
                    visible: row.isHeader && row.model.pinned
                    color: Roost.theme.textSecondary
                    font: Roost.theme.metaFont
                    text: "▲"
                }

                Label {
                    objectName: "headerUnread"
                    visible: row.isHeader && row.model.folded && row.model.unreadCount > 0
                    color: Roost.theme.accent
                    font: Roost.theme.metaFont
                    text: row.isHeader ? "● " + row.model.unreadCount : ""
                }

                // The digits of one width, so that the counts line up.
                Label {
                    objectName: "headerCount"
                    color: Roost.theme.textSecondary
                    font: Roost.theme.withWeight(Roost.theme.metaFont, Roost.theme.metaFont.weight, true)
                    text: row.isHeader ? String(row.model.count) : ""
                }
            }

            // Unread: a dot at the right edge of the row.
            Rectangle {
                objectName: "unreadDot"
                visible: row.isUnread
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                radius: 3
                color: Roost.theme.accent
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
                x: 12
                y: Math.round(lines.y + (titleText.implicitHeight - height) / 2)
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

            // The title and the second line, centred in the row.
            Column {
                id: lines

                visible: !row.isHeader
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 36
                // The dot of an unread session keeps its own place.
                anchors.rightMargin: row.isUnread ? 26 : 12
                anchors.verticalCenter: parent.verticalCenter

                Label {
                    id: titleText

                    objectName: "sessionTitle"
                    textFormat: Text.PlainText
                    width: parent.width
                    elide: Text.ElideRight
                    color: row.isOpen ? Roost.theme.accent : Roost.theme.text
                    font: row.isOpen ? Roost.theme.withWeight(Roost.theme.rowFont, Font.DemiBold) : Roost.theme.rowFont
                    text: row.isHeader ? "" : root.oneLine(row.model.title)
                }

                Label {
                    id: detailText

                    objectName: "sessionDetail"
                    textFormat: Text.PlainText
                    width: parent.width
                    visible: row.showsDetail
                    elide: Text.ElideMiddle
                    color: row.isWaiting ? Roost.theme.attention : Roost.theme.textSecondary
                    font: Roost.theme.smallFont
                    text: row.isHeader ? "" : root.oneLine(root.stateWord(row.model.state) + " · " + row.model.cwd)
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

    // The form of a new session, in place of the head and the list. Tab and
    // Shift+Tab walk its controls; Esc, or the link back, cancels it. The
    // draft is kept in `draft*` of the panel.
    FocusScope {
        id: newForm

        objectName: "sessionsNewForm"
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        // The form ends above the list of unavailable agents, as the list does.
        anchors.bottom: unavailable.visible ? unavailable.top : footer.top
        visible: root.newOpen
        clip: true

        Keys.onEscapePressed: root.closeNewForm()

        // The content scrolls when the panel is low or the font large, so that
        // Create is never cut off or covered by the hint; the control that
        // gets the keyboard is brought into view.
        Flickable {
            id: newScroll

            objectName: "sessionsNewScroll"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: newRule.top
            contentWidth: width
            contentHeight: newContent.height
            boundsBehavior: Flickable.StopAtBounds
            interactive: contentHeight > height
            clip: true

            // Scrolls by the least that puts the control inside the view, with
            // room for the ring of its focus. A control outside the content
            // (another panel's) is left alone.
            function reveal(item) {
                let owner = item;
                while (owner && owner !== newContent) {
                    owner = owner.parent;
                }
                if (!owner) {
                    return;
                }
                const margin = 6;
                const top = item.mapToItem(newContent, 0, 0).y - margin;
                const bottom = top + item.height + 2 * margin;
                if (top < contentY) {
                    contentY = Math.max(0, top);
                } else if (bottom > contentY + height) {
                    contentY = Math.max(0, Math.min(contentHeight - height, bottom - height));
                }
            }

            Item {
                id: newContent

                width: newScroll.width
                height: newFields.y + newFields.height + 20

                PanelLink {
                    id: newBack

                    objectName: "sessionsNewBack"
                    x: 16
                    y: 18
                    text: "← Sessions"
                    KeyNavigation.tab: newTaskField
                    KeyNavigation.backtab: newCreate
                    onClicked: root.closeNewForm()
                }

                Label {
                    id: newTitle

                    objectName: "sessionsNewTitle"
                    x: 20
                    anchors.top: newBack.bottom
                    anchors.topMargin: 16
                    color: root.textColor
                    font: Roost.theme.titleFont
                    text: "New session"
                }

                Column {
                    id: newFields

                    x: 20
                    anchors.top: newTitle.bottom
                    anchors.topMargin: 20
                    width: parent.width - 40
                    spacing: 8

                    Label {
                        color: Roost.theme.textSecondary
                        font: Roost.theme.withWeight(Roost.theme.metaFont, Font.DemiBold)
                        text: "Task"
                    }

                    PanelField {
                        id: newTaskField

                        objectName: "sessionsNewTask"
                        width: parent.width
                        text: root.draftTask
                        onTextEdited: root.draftTask = text
                        onAccepted: root.createFromForm()
                        KeyNavigation.tab: newNameField
                        KeyNavigation.backtab: newBack
                    }

                    Label {
                        topPadding: 8
                        color: Roost.theme.textSecondary
                        font: Roost.theme.withWeight(Roost.theme.metaFont, Font.DemiBold)
                        text: "Name"
                    }

                    PanelField {
                        id: newNameField

                        objectName: "sessionsNewName"
                        width: parent.width
                        placeholderText: "Named by the agent"
                        text: root.draftName
                        onTextEdited: root.draftName = text
                        onAccepted: root.createFromForm()
                        KeyNavigation.tab: newCategoryBox
                        KeyNavigation.backtab: newTaskField
                    }

                    Label {
                        topPadding: 8
                        color: Roost.theme.textSecondary
                        font: Roost.theme.withWeight(Roost.theme.metaFont, Font.DemiBold)
                        text: "Category"
                    }

                    PanelComboBox {
                        id: newCategoryBox

                        objectName: "sessionsNewCategory"
                        width: parent.width
                        model: root.categoryChoices
                        currentIndex: root.draftNewCategory ? root.categoryChoices.length - 1 : Math.max(0, root.categoryChoices.indexOf(root.draftCategory))
                        // An arrow on the closed box walks the entries and keeps the
                        // keyboard; the keyboard goes to the field of the name only
                        // when the entry is chosen in the list that the box opens
                        // (Enter on it, or a click).
                        onActivated: index => {
                            if (index === root.categoryChoices.length - 1) {
                                root.draftNewCategory = true;
                                if (popup.visible) {
                                    newCategoryName.forceActiveFocus();
                                }
                            } else {
                                root.draftNewCategory = false;
                                root.draftCategory = index === 0 ? "" : root.categoryChoices[index];
                            }
                        }
                        KeyNavigation.tab: newCategoryName.visible ? newCategoryName : newCreate
                        KeyNavigation.backtab: newNameField
                    }

                    PanelField {
                        id: newCategoryName

                        objectName: "sessionsNewCategoryName"
                        width: parent.width
                        visible: root.draftNewCategory
                        placeholderText: "Name of the category"
                        text: root.draftNewName
                        onTextEdited: root.draftNewName = text
                        onAccepted: root.createFromForm()
                        KeyNavigation.tab: newCreate
                        KeyNavigation.backtab: newCategoryBox
                    }

                    Item {
                        width: 1
                        height: 12
                    }

                    PanelButton {
                        id: newCreate

                        objectName: "sessionsNewCreate"
                        primary: true
                        enabled: root.canCreate() && !root.newStarting
                        text: root.newStarting ? "Starting…" : "Create session"
                        onClicked: root.createFromForm()
                        KeyNavigation.tab: newBack
                        KeyNavigation.backtab: newCategoryName.visible ? newCategoryName : newCategoryBox
                    }

                    // Why the start failed; the draft is as it was.
                    Label {
                        objectName: "sessionsNewProblem"
                        textFormat: Text.PlainText
                        width: parent.width
                        visible: root.newProblem !== ""
                        color: Roost.theme.error
                        font: Roost.theme.metaFont
                        wrapMode: Text.Wrap
                        text: root.newProblem
                    }
                }
            }
        }

        Connections {
            target: root.Window.window

            function onActiveFocusItemChanged() {
                newScroll.reveal(root.Window.window.activeFocusItem);
            }
        }

        Rectangle {
            id: newRule

            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: newHint.top
            height: 1
            color: Roost.theme.line
        }

        Label {
            id: newHint

            objectName: "sessionsNewHint"
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            padding: 18
            leftPadding: 20
            color: Roost.theme.textMuted
            font: Roost.theme.metaFont
            elide: Text.ElideRight
            text: "Esc: back to the list"
        }
    }

    Label {
        objectName: "sessionsStatus"
        anchors.fill: list
        anchors.margins: 12
        visible: text !== "" && !root.newOpen
        color: Roost.theme.textMuted
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
    Label {
        id: unavailable

        objectName: "sessionsUnavailable"
        textFormat: Text.PlainText
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: footer.top
        anchors.margins: Roost.theme.panelPadding
        visible: text !== ""
        color: Roost.theme.warning
        font: Roost.theme.metaFont
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
        Label {
            objectName: "sessionsProblem"
            width: parent.width
            leftPadding: Roost.theme.panelPadding
            rightPadding: Roost.theme.panelPadding
            bottomPadding: 4
            visible: text !== ""
            color: sessionsList.readOnly ? Roost.theme.error : Roost.theme.warning
            font: Roost.theme.metaFont
            wrapMode: Text.Wrap
            text: sessionsList.problem
        }

        // A command that waits for y.
        Label {
            objectName: "sessionsConfirm"
            textFormat: Text.PlainText
            width: parent.width
            leftPadding: Roost.theme.panelPadding
            rightPadding: Roost.theme.panelPadding
            bottomPadding: 4
            visible: root.confirm !== null
            color: Roost.theme.text
            font: Roost.theme.rowFont
            wrapMode: Text.Wrap
            text: root.confirm === null ? "" : root.confirm.text
        }

        // The prompt, one for the whole footer: the category and the name of a
        // session, and the Task and the Category of a new one.
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

                Label {
                    id: promptLabel

                    objectName: "sessionsPromptLabel"
                    anchors.left: parent.left
                    anchors.leftMargin: Roost.theme.panelPadding
                    anchors.verticalCenter: parent.verticalCenter
                    color: Roost.theme.textSecondary
                    font: Roost.theme.rowFont
                    text: root.promptLabels[root.promptKind] ?? ""
                }

                TextInput {
                    id: promptInput

                    objectName: "sessionsPromptInput"
                    anchors.left: promptLabel.right
                    anchors.leftMargin: 6
                    anchors.right: parent.right
                    anchors.rightMargin: Roost.theme.panelPadding
                    anchors.verticalCenter: parent.verticalCenter
                    clip: true
                    // A TextInput does not inherit the font of the window; the
                    // label beside it does.
                    font: promptLabel.font
                    color: root.textColor
                    selectionColor: Roost.theme.selection
                    selectByMouse: true
                    // The task is on its way: what is typed now would not go
                    // with it.
                    readOnly: root.promptStarting

                    onAccepted: root.submitPrompt()
                    // A tap on the list takes the keyboard away: the prompt is
                    // dropped. Not when the whole window lost the focus.
                    onActiveFocusChanged: {
                        if (!activeFocus && root.promptKind !== "" && Window.active) {
                            root.cancelPrompt();
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
                        if (!root.promptStarting && root.promptPicksCategory && text === "" && plain && index >= 0 && index < 9 && index < sessionsList.knownCategories.length) {
                            text = sessionsList.knownCategories[index];
                            root.submitPrompt();
                            event.accepted = true;
                        }
                    }
                }
            }

            // The start of n: under way, or why it failed (the task is as typed).
            Label {
                objectName: "sessionsPromptProblem"
                textFormat: Text.PlainText
                width: parent.width
                leftPadding: Roost.theme.panelPadding
                rightPadding: Roost.theme.panelPadding
                visible: root.promptStarting || root.promptProblem !== ""
                color: root.promptStarting ? Roost.theme.textMuted : Roost.theme.error
                font: Roost.theme.metaFont
                wrapMode: Text.Wrap
                text: root.promptStarting ? "Starting…" : root.promptProblem
            }

            // The categories the digits pick.
            Label {
                objectName: "sessionsPromptHint"
                width: parent.width
                leftPadding: Roost.theme.panelPadding
                rightPadding: Roost.theme.panelPadding
                visible: root.promptPicksCategory && promptInput.text === "" && sessionsList.knownCategories.length > 0
                color: Roost.theme.textMuted
                font: Roost.theme.metaFont
                elide: Text.ElideRight
                text: sessionsList.knownCategories.slice(0, 9).map((name, i) => (i + 1) + " " + name).join("   ")
            }
        }

        // The outcome of a command, for `noticeMs`.
        Label {
            objectName: "sessionsNotice"
            textFormat: Text.PlainText
            width: parent.width
            leftPadding: Roost.theme.panelPadding
            rightPadding: Roost.theme.panelPadding
            bottomPadding: 4
            visible: root.notice !== ""
            color: Roost.theme.textSecondary
            font: Roost.theme.metaFont
            wrapMode: Text.Wrap
            text: root.notice
        }
    }

    // The keys that work, over the list: `?` or Esc closes it.
    Rectangle {
        objectName: "sessionsHelp"
        anchors.fill: parent
        visible: root.helpOpen
        // The corners of the card the panel sits in.
        radius: Roost.theme.radiusCard
        color: Roost.theme.overlayBackground

        TapHandler {
            onTapped: root.helpOpen = false
        }

        Column {
            anchors.fill: parent
            anchors.margins: 12
            spacing: 10

            Label {
                color: root.textColor
                font.bold: true
                text: "Keyboard shortcuts"
            }

            Label {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Move\n↑ ↓ j k   move the cursor\nEnter   open the session, unfold a folded category"
            }

            Label {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "View\na   next tab (All, Active, Unread)\nv   sessions or archive\nm   compact: the second line only under the cursor\nz   fold the category under the cursor\nShift+z   unfold all\np   pin the category under the cursor\n/   search field, live (Enter or Down keeps the query, Esc clears it)\nc   category of the session under the cursor (1-9 picks a known one, empty clears)\ne   rename the session under the cursor (empty restores the agent's title)"
            }

            Label {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Sessions\nd   delete the session under the cursor (asks)\nx   archive and stop it (asks); in the archive: restore it\nr   respawn it (asks)\nShift+r   respawn all sessions (asks)\nn   new session: Task, then Category"
            }

            Label {
                width: parent.width
                color: root.textColor
                wrapMode: Text.Wrap
                text: "Window\n< >   narrower or wider list\n?   this help (Esc closes it)\nCtrl+\\   from the terminal back to the list"
            }
        }
    }
}
