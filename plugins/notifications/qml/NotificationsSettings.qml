import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The settings page of the notifications plugin (contributes.settings, design
// §5, 05-plugins.md): the switch of the desktop notification, the events that
// notify at all, and the commands that run for them, each with its events, a
// switch, a Test button and the outcome of its last run. The settings window
// shows it as the category "Notifications".
//
// The page reads and writes the document of its plugin: `settings` is the
// user's (written here, read by the backend), `results` is the backend's
// (read here). Every change is one store.patch of the one key that changed; a
// change of the commands patches the whole `commands` array, because a patch
// (RFC 7386) replaces an array. A text is written when its editing finishes
// (Enter, or the focus leaving the field), not at each key. The page follows
// store.changed, the results of the backend and a second writer among them,
// and does not touch a field whose text the user has changed and not yet
// finished.
//
// A row is its command, not its place: a document is matched to the rows by
// the name of the command, so that a row keeps its state (the text being
// typed, the last Test, "Show all") when another writer adds or removes other
// commands. A command is written as the object the document had, with the four
// fields of the page set over it, so that what the page does not know survives.
Item {
    id: root

    // Set by the window before Component.onCompleted.
    property string pluginId
    // The `results` of the document: {<command name>: {exitCode?, error?,
    // stderr, at, event}}.
    property var results: ({})
    // Writes sent whose answer is not here yet. The document of a window that
    // is read meanwhile can be older than the last write: it is read again
    // when the last answer is in.
    property int writing: 0
    // Counts the resets of `writing` (see writeWatchdog): an answer that comes
    // after its write was given up on is not counted again.
    property int writeEpoch: 0
    // The id of the next row; a row keeps it as long as the page lives.
    property int nextRowId: 1
    // The commands as the document last had them, by row id: the page writes a
    // command as its original with its own fields over it.
    property var originals: ({})
    // The items of the `commands` array of the document, in its order: {key:
    // row id} for a command, {raw: value} for an item that is no object (kept
    // in place, the page has no row for it).
    property var order: []

    // What the backend takes for a missing field (backend/settings.hpp): the
    // desktop notification on, the events blocked and failed, no commands. A
    // command without `events` runs for every event, the page shows it so.
    readonly property bool defaultDesktop: true
    readonly property var defaultEvents: ["blocked", "failed"]
    readonly property var eventNames: ["blocked", "done", "failed"]
    readonly property var eventTitles: ({
            "blocked": "Needs you",
            "done": "Done",
            "failed": "Failed"
        })
    // How many lines of the stderr of a command the row shows before "Show
    // all".
    readonly property int stderrLines: 3

    implicitHeight: column.implicitHeight

    function isObject(value) {
        return value !== null && typeof value === "object" && !Array.isArray(value);
    }

    // The names of the events of a stored array that are known, in the order of
    // eventNames; null when the value is no array.
    function knownEvents(value) {
        if (!Array.isArray(value)) {
            return null;
        }
        return eventNames.filter(name => value.includes(name));
    }

    // One command of the document as a row of the list: the events of the
    // command as three flags.
    function rowOf(command) {
        const events = knownEvents(command.events) ?? eventNames;
        return {
            "name": nameOf(command),
            "run": typeof command.run === "string" ? command.run : "",
            "blocked": events.includes("blocked"),
            "done": events.includes("done"),
            "failed": events.includes("failed"),
            "isEnabled": typeof command.enabled === "boolean" ? command.enabled : true,
            "saved": true
        };
    }

    function nameOf(command) {
        return typeof command.name === "string" ? command.name : "";
    }

    // The command of the row at `index` as the document keeps it: the object it
    // had, the four fields of the page set over it. The events the page does
    // not know stay behind the three it toggles.
    function commandOf(index) {
        const row = rows.get(index);
        const original = originals[row.rowId] ?? {};
        const unknown = Array.isArray(original.events) ? original.events.filter(name => !eventNames.includes(name)) : [];
        return Object.assign({}, original, {
            "name": row.name,
            "run": row.run,
            "events": eventNames.filter(name => row[name]).concat(unknown),
            "enabled": row.isEnabled
        });
    }

    function indexOfRow(rowId) {
        for (let i = 0; i < rows.count; ++i) {
            if (rows.get(i).rowId === rowId) {
                return i;
            }
        }
        return -1;
    }

    function eventsOfSwitches() {
        return eventNames.filter(name => eventSwitches.itemAt(eventNames.indexOf(name)).checked);
    }

    // Shows the document: the switches, the results and the rows. A field that
    // has the keyboard keeps its text (the delegate decides).
    function showDocument(document) {
        const settings = isObject(document?.settings) ? document.settings : {};
        desktopSwitch.checked = typeof settings.desktop === "boolean" ? settings.desktop : defaultDesktop;
        const events = knownEvents(settings.events) ?? defaultEvents;
        for (let i = 0; i < eventNames.length; ++i) {
            eventSwitches.itemAt(i).checked = events.includes(eventNames[i]);
        }
        results = isObject(document?.results) ? document.results : ({});
        showCommands(Array.isArray(settings.commands) ? settings.commands : []);
    }

    // The rows follow the `commands` array: a command of the document goes to
    // the row of the same name (a saved one), a new name makes a row, a name
    // that is gone takes its row away. The rows that are not saved yet stay,
    // after the others.
    function showCommands(items) {
        const taken = {};
        const matches = items.map(item => {
            if (!isObject(item)) {
                return -1;
            }
            for (let i = 0; i < rows.count; ++i) {
                const row = rows.get(i);
                if (row.saved && !taken[row.rowId] && row.name === nameOf(item)) {
                    taken[row.rowId] = true;
                    return row.rowId;
                }
            }
            return 0;
        });
        for (let i = rows.count - 1; i >= 0; --i) {
            const row = rows.get(i);
            if (row.saved && !taken[row.rowId]) {
                delete originals[row.rowId];
                rows.remove(i);
            }
        }
        const nextOrder = [];
        let place = 0;
        items.forEach((item, n) => {
            if (!isObject(item)) {
                nextOrder.push({
                    "raw": item
                });
                return;
            }
            const data = rowOf(item);
            let rowId = matches[n];
            if (rowId === 0) {
                rowId = nextRowId++;
                data.rowId = rowId;
                rows.insert(place, data);
            } else {
                const at = indexOfRow(rowId);
                if (at !== place) {
                    rows.move(at, place, 1);
                }
                for (const field in data) {
                    if (rows.get(place)[field] !== data[field]) {
                        rows.setProperty(place, field, data[field]);
                    }
                }
            }
            originals[rowId] = item;
            nextOrder.push({
                "key": rowId
            });
            ++place;
        });
        order = nextOrder;
    }

    function read() {
        Roost.store.get(pluginId, (error, result) => {
            if (!error && writing === 0) {
                showDocument(result.document);
            }
        });
    }

    // One patch of `settings`. The answer of the last write of a run of them
    // reads the document again.
    function write(settings) {
        const epoch = writeEpoch;
        // A call that cannot be made (the page has no store to call) has no
        // answer to wait for.
        if (Roost.store.patch(pluginId, {
            "settings": settings
        }, error => {
            if (epoch !== writeEpoch) {
                return;
            }
            --writing;
            if (writing === 0) {
                writeWatchdog.stop();
                read();
            }
        }) === false) {
            return;
        }
        ++writing;
        writeWatchdog.restart();
    }

    // Writes the commands: the items of the document in their order (the
    // commands the page has no row for, the rows that are gone left out), then
    // the rows that are new and have a command. A row whose command is empty
    // and never was written is not a command yet.
    function writeCommands() {
        const value = [];
        const nextOrder = [];
        const written = {};
        for (const entry of order) {
            if (entry.key === undefined) {
                value.push(entry.raw);
                nextOrder.push(entry);
                continue;
            }
            const index = indexOfRow(entry.key);
            if (index >= 0) {
                value.push(commandOf(index));
                nextOrder.push(entry);
                written[entry.key] = true;
            }
        }
        for (let i = 0; i < rows.count; ++i) {
            const row = rows.get(i);
            if (!written[row.rowId] && !row.saved && row.run !== "") {
                value.push(commandOf(i));
                nextOrder.push({
                    "key": row.rowId
                });
                rows.setProperty(i, "saved", true);
            }
        }
        order = nextOrder;
        for (const entry of nextOrder) {
            if (entry.key !== undefined) {
                originals[entry.key] = value[nextOrder.indexOf(entry)];
            }
        }
        write({
            "commands": value
        });
    }

    // Whether a change of the row at `index` is a change of the document: it
    // is for a command that is written, or one that has its command.
    function isWritten(index) {
        const row = rows.get(index);
        return row.saved || row.run !== "";
    }

    // The text of the problem with a name for the row at `index`, "" for none.
    function nameProblem(index, name) {
        if (name === "") {
            return "The name is empty.";
        }
        for (let i = 0; i < rows.count; ++i) {
            if (i !== index && rows.get(i).name === name) {
                return "A command named \"" + name + "\" exists already.";
            }
        }
        return "";
    }

    // "command 1", "command 2", ...: the first that is free.
    function freeName() {
        for (let n = 1; ; ++n) {
            const name = "command " + n;
            if (nameProblem(-1, name) === "") {
                return name;
            }
        }
    }

    // A new row is on the page only: it is written when it has a command.
    function addCommand() {
        rows.append({
            "rowId": nextRowId++,
            "name": freeName(),
            "run": "",
            "blocked": true,
            "done": true,
            "failed": true,
            "isEnabled": true,
            "saved": false
        });
        commands.itemAt(rows.count - 1).focusName();
    }

    // Takes the row out. The keyboard goes to the name of the next row, of the
    // previous one for the last, to Add command for none: the control that had
    // it goes with the row.
    function removeCommand(index) {
        const wasWritten = isWritten(index);
        rows.remove(index);
        if (wasWritten) {
            writeCommands();
        }
        const next = commands.itemAt(Math.min(index, rows.count - 1));
        if (next) {
            next.focusName();
        } else {
            addButton.forceActiveFocus(Qt.TabFocusReason);
        }
    }

    // The line of the last run of a command: "exit 0 · 12:03", "timeout ·
    // 12:03"; "" before the first run.
    function resultLine(result) {
        if (!isObject(result)) {
            return "";
        }
        const outcome = typeof result.error === "string" ? result.error : "exit " + result.exitCode;
        const at = typeof result.at === "number" ? Qt.formatTime(new Date(result.at), "hh:mm") : "";
        return at === "" ? outcome : outcome + " · " + at;
    }

    // The last lines of the stderr of a run, all of it when `all` is set.
    function stderrTail(result, all) {
        const text = isObject(result) && typeof result.stderr === "string" ? result.stderr.trim() : "";
        const lines = text === "" ? [] : text.split("\n");
        return all || lines.length <= stderrLines ? lines.join("\n") : "…\n" + lines.slice(-stderrLines).join("\n");
    }

    function stderrIsCut(result) {
        return stderrTail(result, true) !== stderrTail(result, false);
    }

    Component.onCompleted: read()

    // The controls of the page, in the light style (docs/design/18-style.md).
    // The plugin does not import them from the shell: they follow the same
    // tokens and sizes as the controls of the settings window and of the
    // sessions panel.
    component PageSwitch: Switch {
        id: toggle

        implicitHeight: 38
        font: Roost.theme.uiFont
        spacing: 12

        // The track and the knob in the tokens: `primary` on, `textMuted` off
        // (the knob keeps its contrast).
        indicator: Rectangle {
            x: toggle.leftPadding
            y: (toggle.height - height) / 2
            implicitWidth: 40
            implicitHeight: 22
            radius: height / 2
            color: toggle.checked ? Roost.theme.primary : Roost.theme.textMuted

            // The ring of the focus around the track, outside with a gap of 2
            // px.
            Rectangle {
                anchors.fill: parent
                anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                visible: toggle.visualFocus
                radius: height / 2
                color: "transparent"
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }

            Rectangle {
                x: toggle.checked ? parent.width - width - 2 : 2
                y: 2
                width: parent.height - 4
                height: width
                radius: width / 2
                color: Roost.theme.surface
            }
        }
        contentItem: Text {
            text: toggle.text
            font: toggle.font
            color: Roost.theme.text
            verticalAlignment: Text.AlignVCenter
            leftPadding: toggle.indicator.width + toggle.spacing
        }
        background: null
    }

    component PageCheckBox: CheckBox {
        id: box

        implicitHeight: 34
        font: Roost.theme.rowFont
        spacing: 8

        indicator: Rectangle {
            x: box.leftPadding
            y: (box.height - height) / 2
            implicitWidth: 20
            implicitHeight: 20
            radius: Roost.theme.radiusSmall
            color: box.checked ? Roost.theme.primary : Roost.theme.inputBackground
            border.width: 1
            border.color: box.checked ? color : Roost.theme.textMuted

            Text {
                anchors.centerIn: parent
                visible: box.checked
                text: "✓"
                font: box.font
                color: Roost.theme.primaryText
            }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                visible: box.visualFocus
                radius: Roost.theme.radiusSmall + 2 + Roost.theme.cursorRingWidth
                color: "transparent"
                border.width: Roost.theme.cursorRingWidth
                border.color: Roost.theme.focus
            }
        }
        contentItem: Text {
            text: box.text
            font: box.font
            color: Roost.theme.text
            verticalAlignment: Text.AlignVCenter
            leftPadding: box.indicator.width + box.spacing
        }
        background: null
    }

    component PageButton: Button {
        id: control

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
            color: !control.enabled ? Roost.theme.textMuted : control.hovered ? Roost.theme.text : Roost.theme.textSecondary
        }

        background: Rectangle {
            implicitWidth: 64
            radius: Roost.theme.radiusControl
            color: !control.enabled ? Roost.theme.controlBackground : (control.hovered || control.down ? Roost.theme.hoverBackground : Roost.theme.surface)
            border.width: 1
            border.color: Roost.theme.line

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

    // Text only, the ring of the focus around it.
    component PageLink: Button {
        id: link

        implicitHeight: Math.max(28, implicitContentHeight + topPadding + bottomPadding)
        leftPadding: 4
        rightPadding: 4
        font: Roost.theme.smallFont

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

    component PageField: TextField {
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

    // The commands of the document, a row of the list for each.
    ListModel {
        id: rows
    }

    ColumnLayout {
        id: column

        width: parent.width
        spacing: 8

        PageSwitch {
            id: desktopSwitch

            objectName: "desktopSwitch"
            text: "Desktop notifications"
            // The default until the document is read; showDocument() sets it.
            checked: root.defaultDesktop
            onToggled: root.write({
                "desktop": checked
            })
        }

        Label {
            Layout.topMargin: 8
            text: "Notify when a session"
            font: Roost.theme.rowFont
            color: Roost.theme.textSecondary
        }

        Repeater {
            id: eventSwitches

            model: root.eventNames

            delegate: PageSwitch {
                required property string modelData

                objectName: modelData + "Switch"
                text: root.eventTitles[modelData]
                checked: root.defaultEvents.includes(modelData)
                onToggled: root.write({
                    "events": root.eventsOfSwitches()
                })
            }
        }

        Label {
            Layout.topMargin: 16
            text: "Commands"
            font: Roost.theme.titleFont
        }

        Label {
            Layout.fillWidth: true
            text: "Each enabled command runs in bash for the events it is set to. It is told the session in ROOST_* variables and as JSON on stdin."
            font: Roost.theme.smallFont
            color: Roost.theme.textMuted
            wrapMode: Text.Wrap
        }

        Repeater {
            id: commands

            model: rows

            delegate: Rectangle {
                id: row

                objectName: "commandRow"
                required property int index
                required property int rowId
                required property bool saved
                required property string name
                required property string run
                required property bool blocked
                required property bool done
                required property bool failed
                required property bool isEnabled
                // The refusal of the last edit of the name, "" for none.
                property string problem: ""
                // The error of the last Test, "" for none.
                property string testError: ""
                // A test is under way: asked, and its result is not here yet.
                property bool testing: false
                property real testedAfter: 0
                property bool showAll: false
                // The user has typed in the field and not finished: only then
                // the field is the user's, otherwise it follows the document,
                // focus or not.
                property bool nameEdited: false
                property bool runEdited: false
                readonly property var result: root.results[name]

                function focusName() {
                    nameField.forceActiveFocus(Qt.TabFocusReason);
                    nameField.selectAll();
                }

                function runTest() {
                    testError = "";
                    testedAfter = root.isObject(result) && typeof result.at === "number" ? result.at : 0;
                    testing = true;
                    testTimeout.restart();
                    Roost.commands.run("notifications.test", {
                        "name": name
                    }, (error, reply) => {
                        if (error) {
                            testError = error.message;
                            testing = false;
                        } else if (!reply.ran.includes(name)) {
                            testError = "The command was not started.";
                            testing = false;
                        }
                    });
                }

                Layout.fillWidth: true
                implicitHeight: rowColumn.implicitHeight + 24
                radius: Roost.theme.radiusCard
                color: Roost.theme.surface
                border.width: 1
                border.color: Roost.theme.line

                // A field that the user is typing in keeps what is typed.
                onNameChanged: {
                    problem = "";
                    if (!nameEdited) {
                        nameField.text = name;
                    }
                }
                onRunChanged: {
                    if (!runEdited) {
                        runField.text = run;
                    }
                }
                onBlockedChanged: blockedBox.checked = blocked
                onDoneChanged: doneBox.checked = done
                onFailedChanged: failedBox.checked = failed
                onIsEnabledChanged: enabledSwitch.checked = isEnabled
                // The result of the test is there: it is a run later than the
                // one that was last seen.
                onResultChanged: {
                    if (testing && root.isObject(result) && result.at !== testedAfter) {
                        testing = false;
                    }
                }
                Component.onCompleted: {
                    nameField.text = name;
                    runField.text = run;
                    blockedBox.checked = blocked;
                    doneBox.checked = done;
                    failedBox.checked = failed;
                    enabledSwitch.checked = isEnabled;
                }

                // A test that never gets its result must not leave the button
                // off for ever. It is longer than the 10 s the backend lets a
                // command run, so that the result of a command that timed out
                // is there before this one fires.
                Timer {
                    id: testTimeout

                    interval: 30000
                    onTriggered: row.testing = false
                }

                ColumnLayout {
                    id: rowColumn

                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        PageField {
                            id: nameField

                            objectName: "commandName"
                            Layout.preferredWidth: 160
                            placeholderText: "Name"
                            Accessible.name: "Name of command " + (row.index + 1)
                            onTextEdited: row.nameEdited = true
                            onEditingFinished: {
                                if (!row.nameEdited) {
                                    return;
                                }
                                row.nameEdited = false;
                                const text = nameField.text.trim();
                                row.problem = root.nameProblem(row.index, text);
                                if (row.problem !== "") {
                                    nameField.text = row.name;
                                    return;
                                }
                                nameField.text = text;
                                if (text !== row.name) {
                                    rows.setProperty(row.index, "name", text);
                                    if (root.isWritten(row.index)) {
                                        root.writeCommands();
                                    }
                                }
                            }
                        }

                        PageField {
                            id: runField

                            objectName: "commandRun"
                            Layout.fillWidth: true
                            font: Roost.theme.terminalFont
                            placeholderText: "Command, run by bash"
                            Accessible.name: "Command line of " + row.name
                            onTextEdited: row.runEdited = true
                            onEditingFinished: {
                                if (!row.runEdited) {
                                    return;
                                }
                                row.runEdited = false;
                                if (runField.text !== row.run) {
                                    rows.setProperty(row.index, "run", runField.text);
                                    if (root.isWritten(row.index)) {
                                        root.writeCommands();
                                    }
                                }
                            }
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        spacing: 8

                        PageCheckBox {
                            id: blockedBox

                            objectName: "commandBlocked"
                            text: root.eventTitles["blocked"]
                            onToggled: {
                                rows.setProperty(row.index, "blocked", checked);
                                if (root.isWritten(row.index)) {
                                    root.writeCommands();
                                }
                            }
                        }

                        PageCheckBox {
                            id: doneBox

                            objectName: "commandDone"
                            text: root.eventTitles["done"]
                            onToggled: {
                                rows.setProperty(row.index, "done", checked);
                                if (root.isWritten(row.index)) {
                                    root.writeCommands();
                                }
                            }
                        }

                        PageCheckBox {
                            id: failedBox

                            objectName: "commandFailed"
                            text: root.eventTitles["failed"]
                            onToggled: {
                                rows.setProperty(row.index, "failed", checked);
                                if (root.isWritten(row.index)) {
                                    root.writeCommands();
                                }
                            }
                        }

                        PageSwitch {
                            id: enabledSwitch

                            objectName: "commandEnabled"
                            text: "Enabled"
                            onToggled: {
                                rows.setProperty(row.index, "isEnabled", checked);
                                if (root.isWritten(row.index)) {
                                    root.writeCommands();
                                }
                            }
                        }

                        PageButton {
                            objectName: "commandTest"
                            text: "Test"
                            // Not before the command is written: the backend
                            // runs what it has read.
                            enabled: !row.testing && root.writing === 0 && row.run !== ""
                            Accessible.name: "Test " + row.name
                            onClicked: row.runTest()
                        }

                        PageButton {
                            objectName: "commandRemove"
                            text: "Remove"
                            Accessible.name: "Remove " + row.name
                            onClicked: root.removeCommand(row.index)
                        }
                    }

                    Label {
                        objectName: "commandUnsaved"
                        Layout.fillWidth: true
                        visible: !row.saved && row.run === ""
                        text: visible ? "Not saved until it has a command" : ""
                        color: Roost.theme.textMuted
                        font: Roost.theme.smallFont
                    }

                    Label {
                        objectName: "commandProblem"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: row.problem
                        color: Roost.theme.error
                        wrapMode: Text.Wrap
                        font: Roost.theme.smallFont
                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: text
                    }

                    Label {
                        objectName: "commandTestError"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: row.testError
                        color: Roost.theme.error
                        wrapMode: Text.Wrap
                        font: Roost.theme.smallFont
                        Accessible.role: Accessible.AlertMessage
                        Accessible.name: text
                    }

                    Label {
                        objectName: "commandResult"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: root.resultLine(row.result)
                        color: root.isObject(row.result) && row.result.exitCode === 0 ? Roost.theme.textSecondary : Roost.theme.warning
                        font: Roost.theme.smallFont
                    }

                    Label {
                        objectName: "commandStderr"
                        Layout.fillWidth: true
                        visible: text !== ""
                        text: root.stderrTail(row.result, row.showAll)
                        color: Roost.theme.textSecondary
                        font: Roost.theme.terminalFont
                        wrapMode: Text.Wrap
                    }

                    PageLink {
                        objectName: "commandStderrToggle"
                        visible: root.stderrIsCut(row.result)
                        text: row.showAll ? "Show less" : "Show all"
                        onClicked: row.showAll = !row.showAll
                    }
                }
            }
        }

        PageButton {
            id: addButton

            objectName: "addCommand"
            text: "Add command"
            onClicked: root.addCommand()
        }
    }

    // A write whose answer never comes must not keep the page from the
    // document for ever: after this long the writes are given up on, and the
    // document is read.
    Timer {
        id: writeWatchdog

        interval: 10000
        onTriggered: {
            console.warn("notifications settings: no answer to", root.writing, "write(s) of the document; reading it again");
            ++root.writeEpoch;
            root.writing = 0;
            root.read();
        }
    }

    Connections {
        target: Roost.store

        function onChanged(plugin, document) {
            if (plugin === root.pluginId && root.writing === 0) {
                root.showDocument(document);
            }
        }
    }
}
