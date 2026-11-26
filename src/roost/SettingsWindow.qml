import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The settings window (design.md §5): the general section, the settings page
// of each plugin that has one, and the keys. One top-level window, made once
// by Main.qml and shown again by open(); closing it only hides it.
//
// The general section is the document `settings` of the store; every edit is
// one `store.patch` of the one key that was edited, so that the edits of other
// windows and of other keys stay. A plugin's page (`contributes.settings`) is
// an Item that roost::SettingsPages makes with the loader and writes to
// `settingsPages`; it keeps its values in the document of its plugin under the
// key `settings`.
//
// The keys are edited by asking roostd, which writes keymap.json (design.md
// §16, "Запись из окна"): one `keymap.setCommand` carries the whole set of the
// keys of a command, one `keymap.resetCommand` takes the user's keys of it
// back. The window never writes the file itself; the button hands it to the
// system.
ApplicationWindow {
    id: root

    // The themes the window knows.
    readonly property var themeIds: ["default"]
    // Written by C++: [{pluginId, page}], the page being the live Item.
    property var settingsPages: []
    // Written by C++: the user's keymap.json.
    property url keymapFile
    // Whether the button opens the file with the system. A test turns it off;
    // keymapFileRequested follows either way.
    property bool opensFiles: true
    // The document `settings` as last seen, and the keys of the table.
    property var settingsDocument: ({})
    property var keymap: []
    // The plugins as `plugins.list` says them: the commands and the keys that
    // they contribute.
    property var plugins: []
    // The last failed write; empty when none.
    property string error
    // Why the document or the table could not be read; empty when they were.
    property string readError
    // The last failed write of the keys; empty when none.
    property string keysError
    // A write of the keys that the window made is under way: from the call
    // until the table that follows it has arrived (the first one after the
    // answer; roostd writes the file, and its watcher publishes the table 100 ms
    // after), or the call fails, or `keysWriteTimeout` passes. The controls of
    // the keys wait for it (as the prompt of the sessions panel waits for the
    // start, `promptStarting`), so that no second write is made from a table
    // that does not have the first yet. A write that changes nothing is
    // answered and has no table after it: the timeout ends it.
    property bool keysWriting: false
    // The write was answered without an error, and its table is awaited.
    property bool keysAnswered: false
    // The longest wait for the answer, and then for the table, in ms.
    property int keysWriteTimeout: 3000
    // What to do when the write ends: done(error), error being null.
    property var afterWrite: null
    // Numbers the writes: the answer of one that was given up on is ignored.
    property int keysWriteId: 0

    readonly property var terminalFont: settingsDocument.terminalFont ?? ({})
    // The families of the terminal font box. The list of the installed ones is
    // made when the box opens, not before; until then the box has the default
    // entry and the stored family. `familyValues` is the family each text
    // stands for, "" for the default entry.
    property var installedFamilies: []
    property var familyValues: [""]
    // The family the window wrote and the document has not answered yet ("" is
    // the default), undefined when none: one user action writes one patch,
    // although a pick can end in two signals of the box.
    property var pendingFamily: undefined
    // The text the box was last given by the window: leaving the box with it
    // unchanged writes nothing (the document may have changed meanwhile).
    property string shownFamilyText: ""
    readonly property string notInstalledMark: " (not installed)"
    // settings.focusColor {preset, custom}, and the presets in the order of
    // the box.
    readonly property var focusColor: settingsDocument.focusColor ?? ({})
    readonly property var focusPresets: ["base", "contrast", "custom"]
    // settings.updates.check as stored; on when the key is not a bool.
    readonly property bool updatesCheck: settingsDocument.updates?.check !== false
    readonly property string platformName: Qt.platform.os === "osx" ? "mac" : "linux"
    readonly property string otherPlatformName: platformName === "mac" ? "linux" : "mac"
    // The rows of the keys: one for each command and each (args, when) that
    // its keys are for, from the commands of the plugins and the table.
    readonly property var keyRows: buildKeyRows(plugins, keymap)
    // The row the capture dialog is for, null when it is closed; and what a
    // key that another command holds waits with until Confirm.
    property var capturing: null
    property var held: null

    // The user pressed "Open keymap.json".
    signal keymapFileRequested(url file)

    // Shows the window, or raises the one that is shown.
    function open() {
        const shown = visible;
        show();
        if (shown) {
            raise();
        }
        requestActivate();
    }

    function openKeymap() {
        keymapFileRequested(keymapFile);
        if (opensFiles && keymapFile.toString() !== "") {
            Qt.openUrlExternally(keymapFile);
        }
    }

    // Writes one key of the general section: the patch is what changed.
    function patchSettings(patch) {
        if (Roost.store === null) {
            return;
        }
        Roost.store.patch("settings", patch, err => {
            error = err ? err.message : "";
            if (err) {
                // The write did not happen: the fields show what is stored.
                pendingFamily = undefined;
                showDocument(true);
            }
        });
    }

    // Writes one key of the terminal font. The last key of it that goes takes
    // the object with it: a merge patch would leave `{}`.
    function patchTerminalFont(key, value) {
        const font = terminalFont;
        const others = (typeof font === "object" && !Array.isArray(font)) ? Object.keys(font).filter(k => k !== key) : [];
        patchSettings({
            "terminalFont": (value === null && others.length === 0) ? null : {
                [key]: value
            }
        });
    }

    // Makes the entries of the terminal font box: the default first, the stored
    // family if the list does not have it (marked when the theme will not take
    // it, so that the box never shows another entry in its place), then the
    // installed families. The box shows the entry of the stored family.
    function showFamily() {
        const stored = pendingFamily !== undefined ? pendingFamily : terminalFont.family;
        // A name of only spaces is no family: the default entry.
        const name = (stored === undefined || stored === null) ? "" : String(stored).trim();
        const texts = ["System monospace (" + Roost.theme.defaultTerminalFont.family + ")"];
        const values = [""];
        if (name !== "" && !installedFamilies.includes(name)) {
            // "monospace" is the system font itself: the theme takes it.
            const taken = name.toLowerCase() === "monospace" || Roost.theme.isMonospaceFamily(name);
            texts.push(taken ? name : name + notInstalledMark);
            values.push(name);
        }
        for (const family of installedFamilies) {
            texts.push(family);
            values.push(family);
        }
        familyValues = values;
        terminalFamilyBox.model = texts;
        terminalFamilyBox.currentIndex = values.indexOf(name);
        // The text too: the index may not change while the text was typed over.
        terminalFamilyBox.editText = terminalFamilyBox.textAt(terminalFamilyBox.currentIndex);
        shownFamilyText = terminalFamilyBox.editText;
    }

    // The list opens: the installed families are read now, the first time since
    // the window was opened (the first read is the slow one, see the docs).
    // What is typed in the box and not written yet stays.
    function openFamilies() {
        const input = terminalFamilyBox.contentItem;
        const typed = terminalFamilyBox.editText;
        const edited = input.activeFocus;
        if (installedFamilies.length === 0) {
            installedFamilies = Roost.theme.monospaceFamilies();
        }
        showFamily();
        if (edited) {
            terminalFamilyBox.editText = typed;
        }
    }

    // Writes the family that was picked or typed: the text of an entry stands
    // for its family, any other text is a name; empty is the default, as an
    // empty field was.
    function commitFamily(text) {
        const typed = text.trim();
        const at = terminalFamilyBox.model.indexOf(typed);
        const family = at >= 0 ? familyValues[at] : typed;
        const value = family === "" ? null : family;
        if (family !== (pendingFamily ?? (terminalFont.family ?? ""))) {
            pendingFamily = family;
            patchTerminalFont("family", value);
        }
        // The text becomes the entry of what is written (a name that is not
        // installed gets its mark). An open list does it when it closes.
        if (!terminalFamilyBox.popup.visible) {
            showFamily();
        }
    }

    // Escape in the text: back to the shown value, nothing is written.
    function revertFamily() {
        showFamily();
    }

    // Writes one key of the focus colour, as patchTerminalFont does.
    function patchFocusColor(key, value) {
        const stored = focusColor;
        const others = (typeof stored === "object" && !Array.isArray(stored)) ? Object.keys(stored).filter(k => k !== key) : [];
        patchSettings({
            "focusColor": (value === null && others.length === 0) ? null : {
                [key]: value
            }
        });
    }

    // A swatch of the ring colour is picked: the colour is the custom one, in
    // the same patch as the preset when that is not Custom yet (as the box
    // does for Custom). The colour that is stored already is not written again;
    // the theme reads it in lower case, so the case does not tell them apart.
    function pickFocusColor(color) {
        if (focusColor.preset === "custom") {
            const stored = focusColor.custom;
            if (typeof stored !== "string" || stored.toLowerCase() !== color.toLowerCase()) {
                patchFocusColor("custom", color);
            }
            return;
        }
        patchSettings({
            "focusColor": {
                "preset": "custom",
                "custom": color
            }
        });
    }

    function isObject(value) {
        return typeof value === "object" && value !== null && !Array.isArray(value);
    }

    // Whether the key is in the document in any form, a wrong one too: there
    // is something to reset.
    function isStored(value) {
        return value !== undefined && value !== null;
    }

    // The fields show the document. What the user is typing in is not
    // replaced, unless `force`.
    function showDocument(force) {
        for (const field of [terminalSizeField, uiSizeField, focusCustomField]) {
            if (force || !field.activeFocus) {
                field.text = field.shown();
            }
        }
        // The document answered what was written, if it did.
        if (pendingFamily !== undefined && String(terminalFont.family ?? "") === pendingFamily) {
            pendingFamily = undefined;
        }
        // Not under an open list (it would close or shift) or a text in use:
        // the box is made again when the list closes or the text is left.
        if (force || !(terminalFamilyBox.popup.visible || terminalFamilyBox.contentItem.activeFocus)) {
            showFamily();
        }
        // A preset that the box does not know shows Base, as the theme takes
        // it (and says so in its problems), not an empty box.
        focusPresetBox.currentIndex = Math.max(0, focusPresets.indexOf(focusColor.preset ?? "base"));
        updatesCheckSwitch.checked = updatesCheck;
        themeBox.currentIndex = themeIds.indexOf(settingsDocument.theme ?? "default");
    }

    // Scrolls the form so that the control with the keyboard is in view, with
    // its ring. Only for a control of the form.
    function revealFocused(scrollView) {
        const item = root.activeFocusItem;
        const flick = scrollView.contentItem;
        let inForm = false;
        for (let up = item; up !== null && !inForm; up = up.parent) {
            inForm = up === flick.contentItem;
        }
        if (!inForm) {
            return;
        }
        const top = item.mapToItem(flick.contentItem, 0, 0).y - root.ringSpace;
        const bottom = top + item.height + 2 * root.ringSpace;
        if (top < flick.contentY) {
            flick.contentY = Math.max(0, top);
        } else if (bottom > flick.contentY + flick.height) {
            flick.contentY = Math.min(bottom - flick.height, flick.contentHeight - flick.height);
        }
    }

    function applyDocument(document) {
        settingsDocument = document;
        showDocument(false);
    }

    function applyKeymap(bindings) {
        keymap = bindings ?? [];
        // The table that follows the write: the first after its answer.
        if (keysWriting && keysAnswered) {
            endWrite(null);
        }
    }

    function sameArgs(a, b) {
        return JSON.stringify(a ?? null) === JSON.stringify(b ?? null);
    }

    // The key that a plugin's keybinding gives on a platform, "" for none.
    function contributedKey(binding, platform) {
        if (platform === "mac") {
            if (typeof binding.mac === "string") {
                return binding.mac;
            }
            if (binding.mac === null) {
                return "";
            }
        }
        return typeof binding.key === "string" ? binding.key : "";
    }

    // The title of a command, its id when no plugin declares it.
    function titleOf(command) {
        for (const plugin of plugins) {
            for (const item of plugin.contributes?.commands ?? []) {
                if (item.id === command) {
                    return item.title ?? command;
                }
            }
        }
        return command;
    }

    // One row for each command of the plugins (in their order) and for each
    // (args, when) that it has keys for: those of the table, and where the
    // plugins give it a key. A command with no key has one row, to give it
    // one. `changed`: the user has a key of it in the file, or a key that the
    // plugins give it is not in the table (the user took it off).
    function buildKeyRows(pluginList, bindings) {
        const commands = {};
        const order = [];
        for (const plugin of pluginList) {
            for (const item of plugin.contributes?.commands ?? []) {
                if (typeof item.id === "string" && !(item.id in commands)) {
                    commands[item.id] = {
                        "title": item.title ?? item.id,
                        "groups": {},
                        "names": []
                    };
                    order.push(item.id);
                }
            }
        }
        const groupOf = (command, args, when) => {
            const name = JSON.stringify([args ?? null, when ?? ""]);
            if (!(name in command.groups)) {
                command.groups[name] = {
                    "args": args,
                    "when": when ?? "",
                    "keys": [],
                    "contributed": []
                };
                command.names.push(name);
            }
            return command.groups[name];
        };
        for (const plugin of pluginList) {
            for (const binding of plugin.contributes?.keybindings ?? []) {
                const text = contributedKey(binding, platformName);
                if (text !== "" && binding.command in commands) {
                    groupOf(commands[binding.command], undefined, binding.when).contributed.push(text);
                }
            }
        }
        for (const binding of bindings) {
            if (binding.command in commands) {
                groupOf(commands[binding.command], binding.args, binding.when).keys.push({
                    "text": binding.key,
                    "source": binding.source
                });
            }
        }
        const rows = [];
        for (const id of order) {
            const command = commands[id];
            if (command.names.length === 0) {
                groupOf(command, undefined, "");
            }
            for (const name of command.names) {
                const group = command.groups[name];
                const present = group.keys.map(k => keyChords.sameKey(k.text));
                rows.push({
                    "command": id,
                    "title": command.title,
                    "args": group.args,
                    "when": group.when,
                    "keys": group.keys,
                    "changed": group.keys.some(k => k.source === "user") || group.contributed.some(c => !present.includes(keyChords.sameKey(c)))
                });
            }
        }
        return rows;
    }

    // The keys that a table has for one command with these args and `when`.
    function keysIn(bindings, command, args, when) {
        return bindings.filter(b => b.command === command && sameArgs(b.args, args) && (b.when ?? "") === (when ?? "")).map(b => b.key);
    }

    // The entries of keymap.setCommand for the keys of a command on this
    // platform (`own`) and on the other (`other`), as the file has them
    // (design.md §16): a key that both platforms have is one entry with `key`;
    // one of this platform alone is `mac` on macOS and `key` with `"mac": null`
    // on Linux, so that it applies here only; one of the other platform alone
    // is kept the same way for it.
    function entriesOf(own, other) {
        const entries = [];
        const rest = other.slice();
        for (const key of own) {
            const at = rest.findIndex(k => keyChords.sameKey(k) === keyChords.sameKey(key));
            if (at >= 0) {
                rest.splice(at, 1);
                entries.push({
                    "key": key
                });
            } else if (platformName === "mac") {
                entries.push({
                    "mac": key
                });
            } else {
                entries.push({
                    "key": key,
                    "mac": null
                });
            }
        }
        for (const key of rest) {
            entries.push(platformName === "mac" ? {
                "key": key,
                "mac": null
            } : {
                "mac": key
            });
        }
        return entries;
    }

    // The params that name the keys of a command in keymap.setCommand and
    // keymap.resetCommand.
    function targetOf(command, args, when) {
        const params = {
            "command": command
        };
        if (args !== undefined && args !== null) {
            params.args = args;
        }
        if (when !== "") {
            params.when = when;
        }
        return params;
    }

    // The keys a command has in a table (`bindings`, that of `platform`), and
    // the keys that its plugin gives it which the table gives to another
    // command only because an entry of the user's file binds them there: the
    // user has not taken those off the command, and a set that left them out
    // would have roostd unbind them (design.md §16, "Запись из окна"). The
    // plugins' keys come from `plugins.list`, `contributes.keybindings`; a key
    // that another plugin holds is not the user's doing, and is left out.
    function latentKeys(bindings, platform, command, args, when) {
        const keys = keysIn(bindings, command, args, when);
        // The plugins give no args.
        if (args !== undefined && args !== null) {
            return keys;
        }
        for (const plugin of plugins) {
            for (const binding of plugin.contributes?.keybindings ?? []) {
                if (binding.command !== command || (binding.when ?? "") !== (when ?? "")) {
                    continue;
                }
                const text = contributedKey(binding, platform);
                if (text === "" || keys.some(k => keyChords.sameKey(k) === keyChords.sameKey(text))) {
                    continue;
                }
                const holder = bindings.find(b => (b.when ?? "") === (when ?? "") && keyChords.sameKey(b.key) === keyChords.sameKey(text));
                if (holder !== undefined && holder.source === "user" && holder.command !== command) {
                    keys.push(text);
                }
            }
        }
        return keys;
    }

    // The refusal for a caller that is a plugin's code: the helpers of the
    // window that write keys are the window's own. A plugin sets the keys of
    // the commands it declares with Roost.call (which is judged by the
    // dispatcher). Null for the window's own code.
    function pluginRefusal() {
        const plugin = Roost.callerPlugin();
        if (plugin === "") {
            return null;
        }
        return {
            "message": "The keys are set by the settings window itself; plugin " + plugin + " sets the keys of its own commands with Roost.call(\"keymap.setCommand\")"
        };
    }

    function messageOf(err) {
        return err.message ?? String(err);
    }

    function showWrite(err) {
        keysError = err ? messageOf(err) : "";
    }

    // The write of the keys ends: with `error` or, when its table has arrived
    // (or the wait has run out), without. What was waiting for it goes on.
    function endWrite(err) {
        keysWriteTimer.stop();
        keysWriting = false;
        keysAnswered = false;
        const next = afterWrite;
        afterWrite = null;
        if (next) {
            next(err);
        }
    }

    // One write of the keys, alone: `send(answered)` makes the call (or the
    // calls) and says answered(error) with the answer to the one that writes;
    // done(error) follows when the write has ended, see keysWriting. A second
    // write while one is under way is refused, not queued: the controls are
    // off meanwhile, so it is a caller that does not look at them.
    function writeKeys(send, done) {
        if (keysWriting) {
            done({
                "message": "The previous change of the keys is still being saved"
            });
            return;
        }
        const id = ++keysWriteId;
        keysWriting = true;
        keysAnswered = false;
        afterWrite = done;
        // No answer is waited for longer than this either: a call that is
        // never answered must not leave the controls off.
        keysWriteTimer.restart();
        send(err => {
            if (id !== keysWriteId || !keysWriting) {
                return;
            }
            if (err) {
                endWrite(err);
            } else {
                keysAnswered = true;
                keysWriteTimer.restart();
            }
        });
    }

    // Sets the whole set of the keys of a command in one keymap.setCommand:
    // `change(keys)` makes its keys on this platform from the keys it has
    // when the write is made (the table as it is then, not as it was when
    // the user began: the table may have moved on), those of the other
    // platform are read now and sent as they are, so that the file keeps them.
    // done(error), when the table that follows the write has arrived.
    function setKeys(command, args, when, change, done) {
        const refusal = pluginRefusal();
        if (refusal !== null) {
            done(refusal);
            return;
        }
        writeKeys(answered => {
            Roost.call("keymap.get", {
                "platform": otherPlatformName
            }, (err, result) => {
                if (err) {
                    answered(err);
                    return;
                }
                const params = targetOf(command, args, when);
                params.entries = entriesOf(change(latentKeys(keymap, platformName, command, args, when)), latentKeys(result.bindings ?? [], otherPlatformName, command, args, when));
                Roost.call("keymap.setCommand", params, answered);
            });
        }, done);
    }

    function withoutKey(keys, text) {
        return keys.filter(k => keyChords.sameKey(k) !== keyChords.sameKey(text));
    }

    function withKey(keys, text) {
        return withoutKey(keys, text).concat([text]);
    }

    function removeKey(row, text) {
        setKeys(row.command, row.args, row.when, keys => withoutKey(keys, text), showWrite);
    }

    function resetKeys(row) {
        const refusal = pluginRefusal();
        if (refusal !== null) {
            showWrite(refusal);
            return;
        }
        writeKeys(answered => Roost.call("keymap.resetCommand", targetOf(row.command, row.args, row.when), answered), showWrite);
    }

    function addKey(row) {
        if (keysWriting) {
            return;
        }
        capturing = row;
        held = null;
        captureDialog.commandTitle = row.title;
        captureDialog.open();
    }

    // The write of a capture has failed: the dialog says why and stays for
    // another key, and the keys section keeps the text after it closes.
    function captureFailed(err, prefix) {
        const text = prefix + messageOf(err);
        keysError = text;
        held = null;
        captureDialog.problem = text;
        captureDialog.conflict = "";
    }

    // Gives the command of the row the chord, from the keys it has now; the
    // dialog closes when the write has ended.
    function addChord(row, chord) {
        setKeys(row.command, row.args, row.when, keys => withKey(keys, chord), err => {
            if (err) {
                captureFailed(err, "");
            } else {
                keysError = "";
                captureDialog.close();
            }
        });
    }

    // A chord was pressed in the dialog. A key that the command has is no
    // news; one that another command holds in the same panel type waits for
    // Confirm; any other is written. While a write is under way the chord is
    // dropped: the dialog says so.
    function captureChord(chord) {
        const row = capturing;
        if (row === null || keysWriting) {
            return;
        }
        captureDialog.problem = "";
        if (keysIn(keymap, row.command, row.args, row.when).some(k => keyChords.sameKey(k) === keyChords.sameKey(chord))) {
            captureDialog.problem = chord + " is already a key of " + row.title;
            return;
        }
        const holder = keyChords.holder(chord, row.when);
        if (holder.command === undefined) {
            addChord(row, chord);
            return;
        }
        warnOf(chord, holder);
    }

    function warnOf(chord, holder) {
        held = {
            "chord": chord,
            "command": holder.command,
            "args": holder.args
        };
        captureDialog.conflict = chord + " is bound to " + titleOf(holder.command) + "; it will lose this key";
    }

    // The user confirmed: this command gets the key first (the lower entry of
    // the file wins, so the table has the key on it from then on), then the
    // other gives it up, so that it does not take the key back when this one
    // is reset. Each set is made from the table as it is at that moment, and
    // the second waits for the table that follows the first. If the second
    // write fails the table is right all the same, and the dialog says so.
    function confirmHeld() {
        const row = capturing;
        const key = held;
        if (row === null || key === null || keysWriting) {
            return;
        }
        // The command has the key now (the file gave it meanwhile): nothing
        // to take from another.
        if (keysIn(keymap, row.command, row.args, row.when).some(k => keyChords.sameKey(k) === keyChords.sameKey(key.chord))) {
            held = null;
            captureDialog.conflict = "";
            captureDialog.problem = key.chord + " is already a key of " + row.title;
            return;
        }
        // Who holds the key now: a change meanwhile (another window, the
        // file) is warned of anew, not overwritten by the old warning.
        const now = keyChords.holder(key.chord, row.when);
        if (now.command === undefined) {
            addChord(row, key.chord);
            return;
        }
        if (now.command !== key.command || !sameArgs(now.args, key.args)) {
            warnOf(key.chord, now);
            return;
        }
        setKeys(row.command, row.args, row.when, keys => withKey(keys, key.chord), err => {
            if (err) {
                captureFailed(err, "");
                return;
            }
            setKeys(key.command, key.args, row.when, keys => withoutKey(keys, key.chord), err2 => {
                if (err2) {
                    captureFailed(err2, row.title + " has " + key.chord + " now, but " + titleOf(key.command) + " could not give it up: ");
                } else {
                    keysError = "";
                    captureDialog.close();
                }
            });
        });
    }

    // The text of a size field: a number, or empty for the default. A text
    // that is not a number is no edit: undefined.
    function sizeOf(text) {
        const trimmed = text.trim();
        if (trimmed === "") {
            return null;
        }
        const value = Number(trimmed);
        return Number.isFinite(value) ? value : undefined;
    }

    objectName: "settingsWindow"
    title: "Settings"
    width: 640
    height: 560
    visible: false
    // A window of its own, not a sheet of the main one.
    transientParent: null
    color: Roost.theme.canvas
    font: Roost.theme.uiFont

    palette {
        window: Roost.theme.windowBackground
        windowText: Roost.theme.windowText
        base: Roost.theme.inputBackground
        text: Roost.theme.windowText
        button: Roost.theme.controlBackground
        buttonText: Roost.theme.windowText
        highlight: Roost.theme.selection
        highlightedText: Roost.theme.textStrong
        placeholderText: Roost.theme.textMuted
        accent: Roost.theme.accent
    }

    // The first show loads the document and the table: a window that is
    // never opened asks the daemon for nothing (a refused connection is
    // retried by a request). A read that fails shows why and is made again
    // at the next connection, which also brings what changed while there was
    // none.
    property bool shownOnce: false
    // A read sent before the connection went is of the old one.
    property int readGeneration: 0

    function read() {
        const generation = ++readGeneration;
        if (!Roost.connected) {
            readError = "Not connected to roostd; the settings are read when it is.";
            return;
        }
        readError = "";
        const failed = err => {
            readError = "Could not read the settings: " + (err.message ?? err);
        };
        if (Roost.store !== null) {
            Roost.store.get("settings", (err, result) => {
                if (generation !== readGeneration) {
                    return;
                }
                if (err) {
                    failed(err);
                } else {
                    applyDocument(result.document);
                }
            });
        }
        Roost.call("keymap.get", undefined, (err, result) => {
            if (generation !== readGeneration) {
                return;
            }
            if (err) {
                failed(err);
            } else {
                applyKeymap(result.bindings);
            }
        });
        readPlugins(generation, failed);
    }

    // The commands and the keys of the plugins; read at each opening too, as
    // the plugins may have changed while the window was closed.
    function readPlugins(generation, failed) {
        Roost.call("plugins.list", undefined, (err, result) => {
            if (generation !== readGeneration) {
                return;
            }
            if (err) {
                failed(err);
            } else {
                plugins = result.plugins ?? [];
            }
        });
    }

    onVisibleChanged: {
        if (!visible) {
            // The list of the installed families is read again at the next
            // opening of the window.
            installedFamilies = [];
        }
        if (!visible) {
            return;
        }
        if (shownOnce) {
            if (Roost.connected) {
                readPlugins(readGeneration, err => {
                    readError = "Could not read the settings: " + (err.message ?? err);
                });
            }
            return;
        }
        shownOnce = true;
        if (Roost.bus !== null) {
            Roost.bus.subscribe("keymap.changed", root, (name, payload) => {
                if (payload.platform === platformName) {
                    applyKeymap(payload.bindings);
                }
            });
        }
        read();
    }

    Connections {
        target: Roost

        function onConnectedChanged() {
            if (root.shownOnce) {
                root.read();
            }
        }
    }

    Connections {
        target: Roost.store

        function onChanged(plugin, document) {
            if (plugin === "settings") {
                root.applyDocument(document);
            }
        }
    }

    // What the ring of a focused control takes outside of it: the gap and the
    // ring (docs/design/18-style.md). The scroll area leaves it free.
    readonly property int ringSpace: 2 + Roost.theme.cursorRingWidth

    component SectionTitle: Label {
        font: Roost.theme.titleFont
        Layout.topMargin: 8
    }

    // What is wrong with one setting of the theme (Roost.theme.settingsProblems),
    // under the fields of it, in the column of the fields: the width of the
    // label of a row and its spacing.
    component SettingProblems: Label {
        required property string setting

        Layout.fillWidth: true
        Layout.leftMargin: 160 + 12
        visible: text !== ""
        wrapMode: Text.Wrap
        color: Roost.theme.warning
        text: Roost.theme.settingsProblems.filter(p => p.setting === setting).map(p => p.text).join("\n")
    }

    // What a setting means, under its row, in the column of the fields.
    component SettingHelp: Label {
        Layout.fillWidth: true
        Layout.leftMargin: 160 + 12
        wrapMode: Text.Wrap
        color: Roost.theme.textMuted
    }

    // The font in force under a field whose stored value is not the one in
    // force: shown while the theme has a problem with the key.
    component SettingInUse: SettingHelp {
        required property string setting
        required property font inUse

        visible: Roost.theme.settingsProblems.some(p => p.setting === setting)
        text: "In use: " + inUse.family + " / " + Roost.theme.sizeText(inUse)
    }

    // Puts a stored setting back to its default: shown only while one is
    // stored.
    component ResetButton: FormButton {
        text: "Reset"
    }

    component FieldRow: RowLayout {
        property alias label: title.text

        Layout.fillWidth: true
        spacing: 12

        Label {
            id: title

            Layout.preferredWidth: 160
        }
    }

    // The chord of a key press and the holder of a key, from C++.
    KeyChords {
        id: keyChords
    }

    // The longest a write of the keys is waited for: it ends the wait even if
    // the table that follows never comes (a write that changes nothing has
    // none, an event can be lost).
    Timer {
        id: keysWriteTimer

        interval: root.keysWriteTimeout
        onTriggered: root.endWrite(root.keysAnswered ? null : {
            "message": "roostd did not answer the change of the keys"
        })
    }

    KeyCaptureDialog {
        id: captureDialog

        busy: root.keysWriting
        onCaptured: chord => root.captureChord(chord)
        onConfirmed: root.confirmHeld()
        onClosed: {
            root.capturing = null;
            root.held = null;
        }
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: Roost.theme.windowMargin
        radius: Roost.theme.radiusCard
        color: Roost.theme.surface
        border.width: 1
        border.color: Roost.theme.line
    }

    ScrollView {
        id: scroll

        objectName: "settingsScroll"
        anchors.fill: parent
        anchors.margins: Roost.theme.windowMargin + Roost.theme.panelPadding
        contentWidth: availableWidth

        // The form is taller than the window at the default size, and the
        // keyboard goes to a control that is below the visible part: scroll
        // to it, with room for its ring.
        Connections {
            function onActiveFocusItemChanged() {
                root.revealFocused(scroll);
            }

            target: root
        }

        ColumnLayout {
            x: root.ringSpace
            width: parent.width - 2 * root.ringSpace
            spacing: 8

            SectionTitle {
                text: "General"
            }

            FieldRow {
                label: "Theme"

                FormComboBox {
                    id: themeBox

                    objectName: "themeBox"
                    Layout.fillWidth: true
                    model: root.themeIds
                    onActivated: root.patchSettings({
                        "theme": currentText
                    })
                }
            }

            SettingHelp {
                text: "Colors of the window and the terminals."
            }

            FieldRow {
                label: "Terminal font"

                // A list of the installed monospace families, and a name can be
                // typed: one that is not installed or not monospace keeps the
                // line of the problem under the row.
                FormComboBox {
                    id: terminalFamilyBox

                    objectName: "terminalFamilyBox"
                    Layout.fillWidth: true
                    editable: true
                    onActivated: index => root.commitFamily(textAt(index))
                    Keys.onEscapePressed: event => {
                        // With the list open, Escape closes the list.
                        event.accepted = !popup.visible;
                        if (event.accepted) {
                            root.revertFamily();
                        }
                    }
                    Component.onCompleted: root.showFamily()

                    Connections {
                        function onAboutToShow() {
                            root.openFamilies();
                        }

                        function onClosed() {
                            // What came while the list was open shows now;
                            // a text that is being typed stays.
                            if (!(terminalFamilyBox.contentItem.activeFocus && terminalFamilyBox.editText !== terminalFamilyBox.currentText)) {
                                root.showFamily();
                            }
                        }

                        target: terminalFamilyBox.popup
                    }

                    // Leaving the box or Enter, as a field of the form.
                    Connections {
                        function onEditingFinished() {
                            // An untouched text is not a choice: it may be of a
                            // family the document no longer has; show the
                            // document instead of writing the text back.
                            if (terminalFamilyBox.editText === root.shownFamilyText) {
                                root.showFamily();
                            } else {
                                root.commitFamily(terminalFamilyBox.editText);
                            }
                        }

                        target: terminalFamilyBox.contentItem
                        ignoreUnknownSignals: true
                    }
                }

                ResetButton {
                    objectName: "terminalFamilyReset"
                    visible: root.terminalFont.family !== undefined || (root.isStored(root.settingsDocument.terminalFont) && !root.isObject(root.settingsDocument.terminalFont))
                    onClicked: root.patchTerminalFont("family", null)
                }
            }

            SettingHelp {
                text: "Monospace font of the terminals."
            }

            FieldRow {
                label: "Terminal font size"

                FormField {
                    id: terminalSizeField

                    function shown() {
                        return String(root.terminalFont.size ?? "");
                    }

                    objectName: "terminalSizeField"
                    Layout.fillWidth: true
                    placeholderText: "default: " + Roost.theme.sizeText(Roost.theme.defaultTerminalFont)
                    onEditingFinished: {
                        const size = root.sizeOf(text);
                        if (size === undefined) {
                            text = shown();
                        } else if (String(size ?? "") !== shown()) {
                            root.patchTerminalFont("size", size);
                        }
                    }
                }

                ResetButton {
                    objectName: "terminalSizeReset"
                    visible: root.terminalFont.size !== undefined || (root.isStored(root.settingsDocument.terminalFont) && !root.isObject(root.settingsDocument.terminalFont))
                    onClicked: root.patchTerminalFont("size", null)
                }
            }

            SettingHelp {
                text: "Size of the text in terminals, 6-72."
            }

            SettingProblems {
                objectName: "terminalFontProblems"
                setting: "terminalFont"
            }

            SettingInUse {
                objectName: "terminalFontInUse"
                setting: "terminalFont"
                inUse: Roost.theme.terminalFont
            }

            FieldRow {
                label: "Interface font size"

                FormField {
                    id: uiSizeField

                    function shown() {
                        return String(root.settingsDocument.uiFontSize ?? "");
                    }

                    objectName: "uiSizeField"
                    Layout.fillWidth: true
                    placeholderText: "default: " + Roost.theme.sizeText(Roost.theme.defaultUiFont)
                    onEditingFinished: {
                        const size = root.sizeOf(text);
                        if (size === undefined) {
                            text = shown();
                        } else if (String(size ?? "") !== shown()) {
                            root.patchSettings({
                                "uiFontSize": size
                            });
                        }
                    }
                }

                ResetButton {
                    objectName: "uiSizeReset"
                    visible: root.settingsDocument.uiFontSize !== undefined
                    onClicked: root.patchSettings({
                        "uiFontSize": null
                    })
                }
            }

            SettingHelp {
                text: "Size of the text of the window: lists, menus, settings. 6-72."
            }

            SettingProblems {
                objectName: "uiFontSizeProblems"
                setting: "uiFontSize"
            }

            SettingInUse {
                objectName: "uiFontInUse"
                setting: "uiFontSize"
                inUse: Roost.theme.uiFont
            }

            FieldRow {
                label: "Focus ring color"

                FormComboBox {
                    id: focusPresetBox

                    objectName: "focusPresetBox"
                    Layout.fillWidth: true
                    model: ["Base", "Contrast", "Custom"]
                    onActivated: index => {
                        const preset = root.focusPresets[index];
                        // Custom starts from the colour the ring has now.
                        root.patchSettings({
                            "focusColor": preset === "custom" && root.focusColor.custom === undefined ? {
                                "preset": preset,
                                "custom": String(Roost.theme.focus)
                            } : {
                                "preset": preset
                            }
                        });
                    }
                }

                // The colour of the two presets, from the theme.
                Label {
                    objectName: "focusPresetColor"
                    visible: root.focusColor.preset !== "custom"
                    color: Roost.theme.textMuted
                    text: String(root.focusColor.preset === "contrast" ? Roost.theme.focusContrastText : Roost.theme.focusBaseText)
                }

                // Any colour that the swatches below do not have: a hex field.
                // What is typed is written; a colour that is hard to read is a
                // warning of the theme below the field, and what is not a
                // colour a reason.
                FormField {
                    id: focusCustomField

                    function shown() {
                        return root.focusColor.custom ?? "";
                    }

                    objectName: "focusCustomField"
                    visible: root.focusColor.preset === "custom"
                    Layout.preferredWidth: 110
                    placeholderText: "#rrggbb"
                    onEditingFinished: {
                        const color = text.trim() === "" ? null : text.trim();
                        if ((color ?? "") !== shown()) {
                            root.patchFocusColor("custom", color);
                        }
                    }
                }

                ResetButton {
                    objectName: "focusColorReset"
                    visible: root.isStored(root.settingsDocument.focusColor)
                    onClicked: root.patchSettings({
                        "focusColor": null
                    })
                }
            }

            // The swatches of the ring colour, in the column of the fields and
            // always shown with the preset box: a click on one is the whole
            // choice, whichever preset is on (it makes the preset Custom).
            // The colour in force is marked; the field above takes any other.
            Flow {
                id: swatchRow

                // The row is one stop of Tab, as a radio group: the swatch that
                // has the keyboard is the stop while it has it, otherwise the
                // colour in force (the first if none of the swatches is).
                readonly property int entry: {
                    for (let i = 0; i < swatches.count; ++i) {
                        if (swatches.itemAt(i)?.inForce) {
                            return i;
                        }
                    }
                    return 0;
                }
                property int focused: -1

                // Moves the keyboard to the swatch of the next row up (-1) or
                // down (1), the nearest in x; nothing if there is no such row.
                function moveRow(from, direction) {
                    const here = swatches.itemAt(from);
                    let rowY = null;
                    for (let i = 0; i < swatches.count; ++i) {
                        const y = swatches.itemAt(i).y;
                        if ((y - here.y) * direction > 0 && (rowY === null || (y - rowY) * direction < 0)) {
                            rowY = y;
                        }
                    }
                    let best = null;
                    for (let i = 0; i < swatches.count && rowY !== null; ++i) {
                        const item = swatches.itemAt(i);
                        if (item.y === rowY && (best === null || Math.abs(item.x - here.x) < Math.abs(best.x - here.x))) {
                            best = item;
                        }
                    }
                    if (best !== null) {
                        best.forceActiveFocus(Qt.TabFocusReason);
                    }
                }

                function moveAlong(from, step) {
                    const item = swatches.itemAt(from + step);
                    if (item) {
                        item.forceActiveFocus(Qt.TabFocusReason);
                    }
                }

                objectName: "focusSwatches"
                Accessible.role: Accessible.Grouping
                Accessible.name: "Focus ring color"
                Layout.fillWidth: true
                Layout.leftMargin: 160 + 12
                spacing: 8 + Roost.theme.cursorRingWidth

                Repeater {
                    id: swatches

                    model: Roost.theme.focusSwatches

                    delegate: AbstractButton {
                        id: swatch

                        required property int index
                        required property string modelData
                        readonly property bool inForce: Qt.colorEqual(Roost.theme.focus, modelData)
                        // The presets are named as the box names them.
                        readonly property string presetName: modelData.toLowerCase() === Roost.theme.focusBaseText.toLowerCase() ? "Base" : modelData.toLowerCase() === Roost.theme.focusContrastText.toLowerCase() ? "Contrast" : ""

                        objectName: "focusSwatch" + index
                        implicitWidth: 28
                        implicitHeight: 28
                        activeFocusOnTab: swatchRow.focused === index || (swatchRow.focused < 0 && swatchRow.entry === index)
                        onActiveFocusChanged: {
                            if (activeFocus) {
                                swatchRow.focused = index;
                            } else if (swatchRow.focused === index) {
                                swatchRow.focused = -1;
                            }
                        }
                        // Arrows move inside the row; Space and Enter pick.
                        Keys.onLeftPressed: swatchRow.moveAlong(index, -1)
                        Keys.onRightPressed: swatchRow.moveAlong(index, 1)
                        Keys.onUpPressed: swatchRow.moveRow(index, -1)
                        Keys.onDownPressed: swatchRow.moveRow(index, 1)
                        // A held Enter repeats: one pick per press.
                        Keys.onReturnPressed: event => {
                            if (!event.isAutoRepeat) {
                                clicked();
                            }
                        }
                        Keys.onEnterPressed: event => {
                            if (!event.isAutoRepeat) {
                                clicked();
                            }
                        }
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: presetName === "" ? modelData : presetName + " (" + modelData + ")"
                        Accessible.description: root.focusColor.preset === "custom" ? "" : "Picking it makes the preset Custom"
                        Accessible.checkable: true
                        Accessible.checked: inForce
                        onClicked: root.pickFocusColor(modelData)

                        background: Rectangle {
                            radius: width / 2
                            color: swatch.modelData
                            border.width: 1
                            border.color: Roost.theme.line

                            // The colour in force: a ring of the surface inside.
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: 4
                                visible: swatch.inForce
                                radius: width / 2
                                color: "transparent"
                                border.width: 2
                                border.color: Roost.theme.surface
                            }

                            // The ring of the focus, outside with a gap of 2 px.
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                                visible: swatch.visualFocus
                                radius: width / 2
                                color: "transparent"
                                border.width: Roost.theme.cursorRingWidth
                                border.color: Roost.theme.focus
                            }
                        }
                    }
                }
            }

            SettingHelp {
                text: "Outline of the control that the keyboard is on."
            }

            SettingProblems {
                objectName: "focusColorProblems"
                setting: "focusColor"
            }

            FieldRow {
                label: "Check for updates"

                Switch {
                    id: updatesCheckSwitch

                    objectName: "updatesCheckSwitch"
                    // The default until the document is read; showDocument()
                    // sets it, a binding would not survive a click.
                    checked: true
                    implicitHeight: 38
                    // The track and the knob in the tokens (derived, no mockup:
                    // docs/design/18-style.md): `primary` on, `textMuted` off
                    // (the white knob keeps its contrast).
                    indicator: Rectangle {
                        objectName: "switchTrack"
                        x: updatesCheckSwitch.leftPadding
                        y: (updatesCheckSwitch.height - height) / 2
                        implicitWidth: 40
                        implicitHeight: 22
                        radius: height / 2
                        color: updatesCheckSwitch.checked ? Roost.theme.primary : Roost.theme.textMuted

                        // The ring of the focus around the track, outside with
                        // a gap of 2 px, as at FormButton.
                        Rectangle {
                            objectName: "switchRing"
                            anchors.fill: parent
                            anchors.margins: -(2 + Roost.theme.cursorRingWidth)
                            visible: updatesCheckSwitch.visualFocus
                            radius: height / 2
                            color: "transparent"
                            border.width: Roost.theme.cursorRingWidth
                            border.color: Roost.theme.focus
                        }

                        Rectangle {
                            objectName: "switchKnob"
                            x: updatesCheckSwitch.checked ? parent.width - width - 2 : 2
                            y: 2
                            width: parent.height - 4
                            height: width
                            radius: width / 2
                            color: Roost.theme.surface
                        }
                    }
                    background: null
                    onToggled: root.patchSettings({
                        "updates": {
                            "check": checked
                        }
                    })
                }
            }

            SettingHelp {
                text: "Look for a new version of roost."
            }

            Label {
                objectName: "settingsError"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: Roost.theme.error
                text: [root.readError, root.error].filter(s => s !== "").join("\n")
            }

            Repeater {
                model: root.settingsPages

                delegate: ColumnLayout {
                    id: section

                    required property var modelData
                    readonly property string pluginId: modelData.pluginId

                    objectName: "pluginSettings"
                    Layout.fillWidth: true
                    spacing: 8

                    SectionTitle {
                        text: section.pluginId
                    }

                    Item {
                        id: host

                        property Item page: section.modelData.page

                        function attach() {
                            if (page === null) {
                                return;
                            }
                            page.parent = host;
                            page.anchors.fill = host;
                        }

                        Layout.fillWidth: true
                        implicitHeight: page ? page.implicitHeight : 0
                        onPageChanged: attach()
                        Component.onCompleted: attach()
                        // The loader deletes the page; not with this item.
                        Component.onDestruction: {
                            if (page !== null) {
                                page.parent = null;
                            }
                        }
                    }
                }
            }

            SectionTitle {
                text: "Keys"
            }

            RowLayout {
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    color: Roost.theme.textMuted
                    text: "Add a key to a command, take one off, or reset the command to the keys of its plugin. The window asks roostd to write keymap.json: the file is rewritten as indented JSON, so its formatting is not kept, and a file that is not valid JSON is left alone."
                }

                FormButton {
                    objectName: "openKeymapButton"
                    text: "Open keymap.json"
                    onClicked: root.openKeymap()
                }
            }

            Label {
                objectName: "keysStatus"
                Layout.fillWidth: true
                visible: root.keysWriting
                color: Roost.theme.textMuted
                text: "Saving the keys\u2026"
            }

            Label {
                objectName: "keysError"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: Roost.theme.error
                text: root.keysError
                Accessible.role: Accessible.AlertMessage
                Accessible.name: text
            }

            Repeater {
                model: root.keyRows

                delegate: RowLayout {
                    id: keyRow

                    required property var modelData

                    objectName: "keymapRow"
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.preferredWidth: 180
                        Layout.alignment: Qt.AlignTop
                        spacing: 0

                        Label {
                            objectName: "keymapTitle"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: keyRow.modelData.title
                        }

                        Label {
                            objectName: "keymapCommand"
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            color: Roost.theme.textMuted
                            font: Roost.theme.smallFont
                            text: keyRow.modelData.command + (keyRow.modelData.when ? "  (" + keyRow.modelData.when + ")" : "")
                        }
                    }

                    Flow {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        spacing: 8

                        Repeater {
                            model: keyRow.modelData.keys

                            delegate: RowLayout {
                                id: keyChip

                                required property var modelData

                                spacing: 4

                                Label {
                                    objectName: "keymapKey"
                                    text: keyChip.modelData.text
                                }

                                Label {
                                    objectName: "keymapSource"
                                    visible: text !== ""
                                    color: Roost.theme.textMuted
                                    font: Roost.theme.smallFont
                                    text: keyChip.modelData.source === "user" ? "user" : ""
                                }

                                FormButton {
                                    objectName: "removeKeyButton"
                                    implicitWidth: 28
                                    implicitHeight: 28
                                    leftPadding: 0
                                    rightPadding: 0
                                    text: "\u00d7"
                                    enabled: !root.keysWriting
                                    Accessible.name: "Remove " + keyChip.modelData.text + " from " + keyRow.modelData.title
                                    onClicked: root.removeKey(keyRow.modelData, keyChip.modelData.text)
                                }
                            }
                        }
                    }

                    FormButton {
                        objectName: "addKeyButton"
                        Layout.alignment: Qt.AlignTop
                        text: "Add key"
                        enabled: !root.keysWriting
                        Accessible.name: "Add a key to " + keyRow.modelData.title
                        onClicked: root.addKey(keyRow.modelData)
                    }

                    ResetButton {
                        objectName: "resetKeysButton"
                        Layout.alignment: Qt.AlignTop
                        visible: keyRow.modelData.changed
                        enabled: !root.keysWriting
                        Accessible.name: "Reset the keys of " + keyRow.modelData.title
                        onClicked: root.resetKeys(keyRow.modelData)
                    }
                }
            }
        }
    }
}
