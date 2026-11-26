import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The settings window (design.md §5): the general section, the settings page
// of each plugin that has one, and the keys, read-only. One top-level window,
// made once by Main.qml and shown again by open(); closing it only hides it.
//
// The general section is the document `settings` of the store; every edit is
// one `store.patch` of the one key that was edited, so that the edits of other
// windows and of other keys stay. A plugin's page (`contributes.settings`) is
// an Item that roost::SettingsPages makes with the loader and writes to
// `settingsPages`; it keeps its values in the document of its plugin under the
// key `settings`. The window never writes keymap.json: the button hands the
// file to the system.
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
    // The last failed write; empty when none.
    property string error
    // Why the document or the table could not be read; empty when they were.
    property string readError

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
    }

    onVisibleChanged: {
        if (!visible) {
            // The list of the installed families is read again at the next
            // opening of the window.
            installedFamilies = [];
        }
        if (!visible || shownOnce) {
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
                        Keys.onReturnPressed: clicked()
                        Keys.onEnterPressed: clicked()
                        Accessible.role: Accessible.RadioButton
                        Accessible.name: presetName === "" ? modelData : presetName + " (" + modelData + ")"
                        Accessible.description: "Picking it makes the preset Custom"
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
                    text: "Read-only. Edit the file to change keys."
                }

                FormButton {
                    objectName: "openKeymapButton"
                    text: "Open keymap.json"
                    onClicked: root.openKeymap()
                }
            }

            Repeater {
                model: root.keymap

                delegate: RowLayout {
                    id: keyRow

                    required property var modelData

                    objectName: "keymapRow"
                    Layout.fillWidth: true
                    spacing: 12

                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: keyRow.modelData.command
                    }

                    Label {
                        objectName: "keymapKey"
                        text: keyRow.modelData.key + (keyRow.modelData.when ? "  (" + keyRow.modelData.when + ")" : "")
                    }

                    Label {
                        Layout.preferredWidth: 90
                        color: Roost.theme.textMuted
                        text: keyRow.modelData.source
                    }
                }
            }
        }
    }
}
