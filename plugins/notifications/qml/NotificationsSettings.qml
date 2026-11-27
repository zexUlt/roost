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
// and does not touch the field that has the keyboard.
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
            "name": typeof command.name === "string" ? command.name : "",
            "run": typeof command.run === "string" ? command.run : "",
            "blocked": events.includes("blocked"),
            "done": events.includes("done"),
            "failed": events.includes("failed"),
            "isEnabled": typeof command.enabled === "boolean" ? command.enabled : true
        };
    }

    function commandOf(row) {
        return {
            "name": row.name,
            "run": row.run,
            "events": eventNames.filter(name => row[name]),
            "enabled": row.isEnabled
        };
    }

    function eventsOfSwitches() {
        return eventNames.filter(name => eventSwitches.itemAt(eventNames.indexOf(name)).checked);
    }

    // The commands of the list as the document keeps them.
    function commandsValue() {
        const commands = [];
        for (let i = 0; i < rows.count; ++i) {
            commands.push(commandOf(rows.get(i)));
        }
        return commands;
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
        const commands = Array.isArray(settings.commands) ? settings.commands.filter(isObject).map(rowOf) : [];
        while (rows.count > commands.length) {
            rows.remove(rows.count - 1);
        }
        for (let i = 0; i < commands.length; ++i) {
            if (i >= rows.count) {
                rows.append(commands[i]);
                continue;
            }
            for (const field in commands[i]) {
                if (rows.get(i)[field] !== commands[i][field]) {
                    rows.setProperty(i, field, commands[i][field]);
                }
            }
        }
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
        ++writing;
        Roost.store.patch(pluginId, {
            "settings": settings
        }, error => {
            --writing;
            if (writing === 0) {
                read();
            }
        });
    }

    function writeCommands() {
        write({
            "commands": commandsValue()
        });
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

    function addCommand() {
        rows.append({
            "name": freeName(),
            "run": "",
            "blocked": true,
            "done": true,
            "failed": true,
            "isEnabled": true
        });
        writeCommands();
        commands.itemAt(rows.count - 1).focusName();
    }

    // Takes the row out. The keyboard goes to Add command: the control that had
    // it goes with the row.
    function removeCommand(index) {
        rows.remove(index);
        writeCommands();
        addButton.forceActiveFocus(Qt.TabFocusReason);
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

                // A field that has the keyboard keeps what is typed.
                onNameChanged: {
                    if (!nameField.activeFocus) {
                        nameField.text = name;
                    }
                }
                onRunChanged: {
                    if (!runField.activeFocus) {
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
                // off for ever: longer than the backend lets a command run.
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
                            onEditingFinished: {
                                const text = nameField.text.trim();
                                row.problem = root.nameProblem(row.index, text);
                                if (row.problem !== "") {
                                    nameField.text = row.name;
                                    return;
                                }
                                nameField.text = text;
                                if (text !== row.name) {
                                    rows.setProperty(row.index, "name", text);
                                    root.writeCommands();
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
                            onEditingFinished: {
                                if (runField.text !== row.run) {
                                    rows.setProperty(row.index, "run", runField.text);
                                    root.writeCommands();
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
                                root.writeCommands();
                            }
                        }

                        PageCheckBox {
                            id: doneBox

                            objectName: "commandDone"
                            text: root.eventTitles["done"]
                            onToggled: {
                                rows.setProperty(row.index, "done", checked);
                                root.writeCommands();
                            }
                        }

                        PageCheckBox {
                            id: failedBox

                            objectName: "commandFailed"
                            text: root.eventTitles["failed"]
                            onToggled: {
                                rows.setProperty(row.index, "failed", checked);
                                root.writeCommands();
                            }
                        }

                        PageSwitch {
                            id: enabledSwitch

                            objectName: "commandEnabled"
                            text: "Enabled"
                            onToggled: {
                                rows.setProperty(row.index, "isEnabled", checked);
                                root.writeCommands();
                            }
                        }

                        PageButton {
                            objectName: "commandTest"
                            text: "Test"
                            // Not before the command is written: the backend
                            // runs what it has read.
                            enabled: !row.testing && root.writing === 0
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

    Connections {
        target: Roost.store

        function onChanged(plugin, document) {
            if (plugin === root.pluginId && root.writing === 0) {
                root.showDocument(document);
            }
        }
    }
}
