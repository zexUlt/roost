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

    readonly property var terminalFont: settingsDocument.terminalFont ?? ({})
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
        });
    }

    function applyDocument(document) {
        settingsDocument = document;
        // What the user is typing in is not replaced.
        for (const field of [terminalFamilyField, terminalSizeField, uiSizeField]) {
            if (!field.activeFocus) {
                field.text = field.shown();
            }
        }
        themeBox.currentIndex = themeIds.indexOf(settingsDocument.theme ?? "default");
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
    color: Roost.theme.windowBackground
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
    // retried by a request).
    property bool loaded: false

    onVisibleChanged: {
        if (!visible || loaded) {
            return;
        }
        loaded = true;
        if (Roost.store !== null) {
            Roost.store.get("settings", (err, result) => {
                if (!err) {
                    applyDocument(result.document);
                }
            });
        }
        if (Roost.bus !== null) {
            Roost.bus.subscribe("keymap.changed", root, (name, payload) => {
                if (payload.platform === platformName) {
                    applyKeymap(payload.bindings);
                }
            });
            Roost.call("keymap.get", undefined, (err, result) => {
                if (!err) {
                    applyKeymap(result.bindings);
                }
            });
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

    component SectionTitle: Label {
        font.bold: true
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

    ScrollView {
        anchors.fill: parent
        anchors.margins: 16
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 8

            SectionTitle {
                text: "General"
            }

            FieldRow {
                label: "Theme"

                ComboBox {
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

                TextField {
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
                            root.patchSettings({
                                "terminalFont": {
                                    "family": family
                                }
                            });
                        }
                    }
                }
            }

            FieldRow {
                label: "Terminal font size"

                TextField {
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
                            root.patchSettings({
                                "terminalFont": {
                                    "size": size
                                }
                            });
                        }
                    }
                }
            }

            FieldRow {
                label: "Interface font size"

                TextField {
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
                visible: root.error !== ""
                wrapMode: Text.Wrap
                color: Roost.theme.error
                text: root.error
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

                Button {
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
