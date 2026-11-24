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

    // The fields show the document. What the user is typing in is not
    // replaced, unless `force`.
    function showDocument(force) {
        for (const field of [terminalFamilyField, terminalSizeField, uiSizeField]) {
            if (force || !field.activeFocus) {
                field.text = field.shown();
            }
        }
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

            FieldRow {
                label: "Terminal font"

                FormField {
                    id: terminalFamilyField

                    function shown() {
                        return root.terminalFont.family ?? "";
                    }

                    objectName: "terminalFamilyField"
                    Layout.fillWidth: true
                    placeholderText: "system monospace"
                    onEditingFinished: {
                        const family = text.trim() === "" ? null : text.trim();
                        if ((family ?? "") !== shown()) {
                            root.patchTerminalFont("family", family);
                        }
                    }
                }
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
                    placeholderText: "default"
                    onEditingFinished: {
                        const size = root.sizeOf(text);
                        if (size === undefined) {
                            text = shown();
                        } else if (String(size ?? "") !== shown()) {
                            root.patchTerminalFont("size", size);
                        }
                    }
                }
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
                    placeholderText: "default"
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
                    background: Rectangle {
                        radius: Roost.theme.radiusControl
                        color: "transparent"
                        border.width: updatesCheckSwitch.visualFocus ? Roost.theme.cursorRingWidth : 0
                        border.color: Roost.theme.focus
                    }
                    onToggled: root.patchSettings({
                        "updates": {
                            "check": checked
                        }
                    })
                }
            }

            Label {
                objectName: "fontProblems"
                Layout.fillWidth: true
                visible: text !== ""
                wrapMode: Text.Wrap
                color: Roost.theme.warning
                text: Roost.theme.fontProblems.join("\n")
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
