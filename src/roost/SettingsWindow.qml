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

    // The fields show the document. What the user is typing in is not
    // replaced, unless `force`.
    function showDocument(force) {
        for (const field of [terminalFamilyField, terminalSizeField, uiSizeField, focusCustomField]) {
            if (force || !field.activeFocus) {
                field.text = field.shown();
            }
        }
        // A preset that the box does not know shows Base, as the theme takes
        // it (and says so in its problems), not an empty box.
        focusPresetBox.currentIndex = Math.max(0, focusPresets.indexOf(focusColor.preset ?? "base"));
        updatesCheckSwitch.checked = updatesCheck;
        themeBox.currentIndex = themeIds.indexOf(settingsDocument.theme ?? "default");
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
        objectName: "settingsScroll"
        anchors.fill: parent
        anchors.margins: Roost.theme.windowMargin + Roost.theme.panelPadding
        contentWidth: availableWidth

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

                FormField {
                    id: terminalFamilyField

                    function shown() {
                        return root.terminalFont.family ?? "";
                    }

                    objectName: "terminalFamilyField"
                    Layout.fillWidth: true
                    placeholderText: "default: " + Roost.theme.terminalFont.family
                    onEditingFinished: {
                        const family = text.trim() === "" ? null : text.trim();
                        if ((family ?? "") !== shown()) {
                            root.patchTerminalFont("family", family);
                        }
                    }
                }

                ResetButton {
                    objectName: "terminalFamilyReset"
                    visible: root.terminalFont.family !== undefined
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
                    placeholderText: "default: " + Roost.theme.sizeText(Roost.theme.terminalFont)
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
                    visible: root.terminalFont.size !== undefined
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

            FieldRow {
                label: "Interface font size"

                FormField {
                    id: uiSizeField

                    function shown() {
                        return String(root.settingsDocument.uiFontSize ?? "");
                    }

                    objectName: "uiSizeField"
                    Layout.fillWidth: true
                    placeholderText: "default: " + Roost.theme.sizeText(Roost.theme.uiFont)
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
                    text: String(root.focusColor.preset === "contrast" ? Roost.theme.focusContrast : Roost.theme.focusBase)
                }

                // The project has no colour picker: a hex field. What is typed
                // is written; a colour that is hard to read is a warning of the
                // theme below the field, and what is not a colour a reason.
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
                    visible: root.focusColor.preset !== undefined
                    onClicked: root.patchSettings({
                        "focusColor": null
                    })
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
