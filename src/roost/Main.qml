import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// The window of the shell (design.md §6): the layout view, which builds the
// panel slots from the tree roost::Shell publishes in `layoutTree`. roost::Shell
// fills the slots and writes the properties below.
ApplicationWindow {
    id: root

    // "connecting", "starting" (roostd is being started by the window),
    // "startFailed" (the start failed; the message says why), "restarting"
    // (the old roostd is being stopped), "connected", "disconnected" or
    // "refused"; written by C++.
    property string connection: "connecting"
    // The reason of the disconnect, or for "refused" the message with the
    // versions and the hint.
    property string connectionMessage: ""
    // Whether the refusal is a version mismatch the window can fix by
    // restarting roostd (there is a launcher); written by C++.
    property bool restartOffered: false
    // Strings: the problems of roostd's plugins, of the panels and of links.
    property var problems: []
    // The problems of the keymap table (keymap.json and the keys of the
    // plugins) but those already among `problems`: roostd's plugin problems
    // hold the keys of its own platform.
    readonly property var keymapOnlyProblems: Roost.keymapProblems.filter(p => problems.indexOf(p) < 0)
    // What the strip shows: `problems`, then keymapOnlyProblems, then the
    // problems of the settings the theme reads (the window's own: other
    // windows have theirs).
    readonly property var shownProblems: problems.concat(keymapOnlyProblems, Roost.theme.settingsProblems.map(p => p.text), Roost.updates.problems)
    // Whether the link of the update line opens the browser. A test turns it
    // off; releaseOpened follows either way.
    property bool opensUrls: true

    // The Retry button of a refused connection.
    signal retryRequested
    // The Restart roostd button of a version mismatch.
    signal restartRequested
    // The user pressed the link of the update line (design/17-updates.md).
    signal releaseOpened(url page)

    // The layout tree (see LayoutView.qml), written by C++; null before it.
    property var layoutTree: null
    // The panel types that are lists of sessions (an output `selected` of type
    // SessionRef), written by C++.
    property var listPanelTypes: []
    // The lists of sessions are hidden (collapsed, not closed): the state for
    // a button "Hide list" / "Show list"; the command is `layout.toggleLists`.
    readonly property bool listsHidden: layoutView.listsHidden

    // A slot of the layout view is built; C++ hands it its panel.
    signal slotCreated(var slot)
    // The user finished resizing a split (path, sizes); C++ saves it.
    signal splitResized(var path, var sizes)
    // The user chose a tab of a stack (path, index); C++ saves it.
    signal tabChosen(var path, int index)
    // An edit of the layout (action, args), see LayoutView; C++ applies it.
    signal layoutEdit(string action, var args)
    // The panel types the layout can be split with; written by C++.
    property var panelTypes: []
    // Those of them that have inputs, so links to edit; written by C++.
    property var panelsWithInputs: []

    // C++ calls these: the keyboard to a panel after an edit, and the link
    // editor with the state of a panel.
    function focusInstance(instance) {
        layoutView.focusInstance(instance);
    }

    function openLinkEditor(state) {
        layoutView.openLinkEditor(state);
    }

    width: 960
    height: 600
    visible: true
    title: "roost"
    color: Roost.theme.canvas
    // The font of every Qt Quick Controls item below and of the Labels of the
    // panels: they inherit the font of the window, as they do its palette.
    // (A plain Text does not; the panels use Label.)
    font: Roost.theme.uiFont

    // The colours of every Qt Quick Controls item below, the dialogs and the
    // panels' Controls included: they inherit the palette of the window. The
    // Fusion style (roost::SelectControlsStyle) paints with it; the native
    // style of macOS would not. A panel with a dark background of its own
    // sets its own `palette` from the tokens.
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

    // Opens the bug report dialog; macOS calls it from the Help menu and
    // Cmd+Shift+B (the command shell.reportBug), other platforms from the
    // toolbar button.
    function reportBug() {
        bugReportDialog.open();
    }

    // Opens the command palette; macOS calls it from the View menu, other
    // platforms from the toolbar button, and both from the command
    // shell.palette.
    function openPalette() {
        commandPalette.open();
    }

    // Shows the settings window, or raises it; macOS calls it from the
    // "Settings…" item of the menu bar (the native menu puts it in the
    // application menu) and Cmd+, (the command shell.settings), other
    // platforms from the toolbar button.
    function openSettings() {
        settingsWindow.open();
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The menu bar only where Qt Quick Controls makes it native (macOS):
        // elsewhere MenuBar would draw a bar inside the window. Help → "Report a
        // Bug…" is there instead of the toolbar button. It is the first row of
        // the column, so that it takes room of its own and does not lie over
        // the banner. Where Qt makes the bar native it sets the height of the
        // MenuBar to 0 (QQuickMenuBarPrivate::createNativeMenuBar) and leaves
        // its implicit height above 0; Qt Quick Controls has no public flag
        // for it, so `nativeBar` is decided once, when the bar is created. The
        // row is the implicit height of the bar, so that it follows the bar
        // when the bar grows (a larger interface font), and 0 for the native
        // one: the row cannot follow `item.height`, which the Loader itself
        // sets from the row and which then keeps its first value.
        // The item "Settings…" has no role in Qt Quick Controls;
        // the Cocoa menu takes it for the application's settings by its text
        // ("Setting", "Preferences", ...) and moves it to the application menu.
        Loader {
            objectName: "menuBarLoader"
            active: Qt.platform.os === "osx"
            visible: active
            Layout.fillWidth: true
            Layout.preferredHeight: item === null || nativeBar ? 0 : item.implicitHeight
            property bool nativeBar: false
            // Decides `nativeBar` from the bar as Qt has just made it. A function
            // of its own so that the test can call it after setting the height.
            function detectNative() {
                nativeBar = item !== null && item.height === 0 && item.implicitHeight > 0;
            }
            onLoaded: detectNative()
            sourceComponent: MenuBar {
                Menu {
                    title: "roost"

                    MenuItem {
                        objectName: "settingsMenuItem"
                        text: "Settings…"
                        onTriggered: root.openSettings()
                    }
                }

                Menu {
                    title: "View"

                    MenuItem {
                        objectName: "paletteMenuItem"
                        text: "Command Palette…"
                        onTriggered: root.openPalette()
                    }
                }

                Menu {
                    title: "Help"

                    MenuItem {
                        objectName: "reportBugMenuItem"
                        text: "Report a Bug…"
                        onTriggered: root.reportBug()
                    }
                }
            }
        }

        // Not on macOS: there the action is in the native menu bar. A `surface`
        // bar with a hairline of `line` under it (derived, no mockup:
        // docs/design/18-style.md).
        ToolBar {
            objectName: "toolBar"
            visible: Qt.platform.os !== "osx"
            Layout.fillWidth: true
            // The room of the focus ring of a button, which lies outside it
            // with a gap of 2 px: the edge of the window and the next item of
            // the column must not cut it. The bottom has the hairline too.
            leftPadding: 2 + Roost.theme.cursorRingWidth
            rightPadding: 2 + Roost.theme.cursorRingWidth
            topPadding: 2 + Roost.theme.cursorRingWidth
            bottomPadding: 3 + Roost.theme.cursorRingWidth
            background: Item {
                Rectangle {
                    anchors.fill: parent
                    color: Roost.theme.surface
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Roost.theme.line
                }
            }

            RowLayout {
                anchors.fill: parent

                Item {
                    Layout.fillWidth: true
                }

                ToolbarButton {
                    objectName: "paletteButton"
                    text: "Command Palette…"
                    onClicked: root.openPalette()
                }

                ToolbarButton {
                    objectName: "settingsButton"
                    text: "Settings…"
                    onClicked: root.openSettings()
                }

                ToolbarButton {
                    objectName: "reportBugButton"
                    text: "Report a Bug…"
                    onClicked: root.reportBug()
                }
            }
        }

        Rectangle {
            id: banner

            objectName: "connectionBanner"
            Layout.fillWidth: true
            visible: root.connection !== "connected"
            implicitHeight: bannerRow.implicitHeight + 16
            color: root.connection === "refused" ? Roost.theme.bannerRefusedBackground : Roost.theme.bannerBackground

            RowLayout {
                id: bannerRow

                anchors.fill: parent
                anchors.margins: 8
                spacing: 12

                Label {
                    objectName: "connectionText"
                    Layout.fillWidth: true
                    wrapMode: Text.Wrap
                    // A failure is in the colour of the error, the rest waits
                    // for the user: `bannerText`, `attention` by default
                    // (derived, no mockup).
                    color: root.connection === "refused" || root.connection === "startFailed" ? Roost.theme.error : Roost.theme.bannerText
                    text: {
                        switch (root.connection) {
                        case "connecting":
                            return "Connecting to roostd...";
                        case "starting":
                            return "Starting roostd…";
                        case "restarting":
                            return "Restarting roostd…";
                        case "startFailed":
                        case "refused":
                            return root.connectionMessage;
                        default:
                            return "Disconnected from roostd, reconnecting. " + root.connectionMessage;
                        }
                    }
                }

                Button {
                    objectName: "retryButton"
                    visible: root.connection === "refused"
                    text: "Retry"
                    onClicked: root.retryRequested()
                }

                Button {
                    objectName: "restartButton"
                    visible: root.connection === "refused" && root.restartOffered
                    text: "Restart roostd"
                    onClicked: root.restartRequested()
                }
            }
        }

        LayoutView {
            id: layoutView

            objectName: "layoutView"
            // The margin of the window: the room of the cards and of the ring.
            Layout.margins: Roost.theme.windowMargin
            Layout.fillWidth: true
            Layout.fillHeight: true
            tree: root.layoutTree
            listTypes: root.listPanelTypes
            onSlotCreated: slot => root.slotCreated(slot)
            onSplitResized: (path, sizes) => root.splitResized(path, sizes)
            onTabChosen: (path, index) => root.tabChosen(path, index)
            panelTypes: root.panelTypes
            panelsWithInputs: root.panelsWithInputs
            onLayoutEdit: (action, args) => root.layoutEdit(action, args)
        }

        // A newer release is out: an offer, not a problem (design/17-updates.md).
        Rectangle {
            objectName: "updateStrip"
            Layout.fillWidth: true
            // An install in work keeps the strip, whatever the offer or the
            // setting says meanwhile; the setting is followed once it ends.
            visible: Roost.updates.available !== "" || Roost.updates.installBusy
            implicitHeight: updateRow.implicitHeight + 8
            color: Roost.theme.controlBackground

            RowLayout {
                id: updateRow

                anchors.fill: parent
                anchors.margins: 4
                spacing: 8

                Label {
                    objectName: "updateText"
                    // The text below is the error of the install: the same
                    // order of the cases.
                    readonly property bool failed: Roost.updates.installState !== "installing" && !Roost.updates.installBusy && Roost.updates.installError !== ""

                    Layout.fillWidth: true
                    text: {
                        const updates = Roost.updates;
                        if (updates.installState === "installing")
                            return "Installing roost v" + updates.installVersion + "...";
                        if (updates.installBusy)
                            return "Downloading roost v" + updates.installVersion + ": " + Math.round(updates.downloadProgress * 100) + "%";
                        if (updates.installError !== "")
                            return updates.installError;
                        return "roost v" + updates.available + " is out";
                    }
                    color: failed ? Roost.theme.error : Roost.theme.attention
                    wrapMode: Text.Wrap
                    maximumLineCount: 4
                    elide: Text.ElideRight
                }

                Button {
                    objectName: "updateInstallButton"
                    visible: Roost.updates.canInstall
                    enabled: !Roost.updates.installBusy
                    text: "Install and restart"
                    onClicked: Roost.updates.install()
                }

                Button {
                    objectName: "updateLinkButton"
                    visible: Roost.updates.url !== ""
                    flat: true
                    text: "Release page"
                    onClicked: {
                        const page = Roost.updates.url;
                        root.releaseOpened(page);
                        if (root.opensUrls) {
                            Qt.openUrlExternally(page);
                        }
                    }
                }

                Button {
                    objectName: "updateDismissButton"
                    flat: true
                    enabled: !Roost.updates.installBusy
                    text: "Dismiss"
                    onClicked: Roost.updates.dismiss()
                }
            }
        }

        Rectangle {
            objectName: "problemsStrip"
            Layout.fillWidth: true
            visible: root.shownProblems.length > 0
            implicitHeight: Math.min(problemsList.contentHeight, 96) + 8
            color: Roost.theme.problemsBackground

            ListView {
                id: problemsList

                anchors.fill: parent
                anchors.margins: 4
                clip: true
                model: root.shownProblems
                delegate: Label {
                    required property string modelData

                    width: ListView.view.width
                    text: modelData
                    color: Roost.theme.problemsText
                    elide: Text.ElideRight
                }
            }
        }
    }

    // The commands of the window (the manifest of the built-in plugin `shell`
    // declares them and gives them keys; the matcher runs them, key and
    // palette alike). Not Shortcuts while the table is there: Qt tries a
    // Shortcut before the key press reaches the matcher, so a key that is
    // both would never be looked up in the table (the two Shortcuts below
    // are on only without a table).
    //
    // shell.focusList, Ctrl+\ (the physical Control key) by default: moves the
    // keyboard to the list from anywhere, the terminal included: the list that
    // feeds the panel that has it, or the first list. Not a toggle: in the list
    // it does nothing. shell.reportBug, Cmd+Shift+B on macOS only:
    // on Linux Ctrl+Shift+B must reach the program in the terminal.
    // shell.palette, Ctrl+Shift+P (Cmd+Shift+P on macOS): the command palette.
    // `Roost.commands` is null in a window that has no API (some tests).
    Component.onCompleted: {
        if (Roost.commands !== null) {
            Roost.commands.registerCommand("shell.focusList", () => layoutView.focusList());
            Roost.commands.registerCommand("shell.reportBug", () => root.reportBug());
            Roost.commands.registerCommand("shell.palette", () => root.openPalette());
            Roost.commands.registerCommand("shell.settings", () => root.openSettings());
        }
    }

    // The same two keys while the window has no keymap table (before roostd
    // has answered keymap.get, roostd down or crashed): the bug report must be
    // reachable exactly when roostd is broken, and a key that moves the
    // keyboard to the list is the way out of a panel. While the table is
    // there, they are off: the key is then an entry of the table, and an
    // enabled Shortcut would take it before the matcher (a double run, and no
    // way for keymap.json to change it). Ctrl is the physical Control key;
    // Qt's Ctrl is Command on macOS.
    Shortcut {
        enabled: !Roost.keymapLoaded
        context: Qt.WindowShortcut
        sequence: Qt.platform.os === "osx" ? "Meta+\\" : "Ctrl+\\"
        onActivated: layoutView.focusList()
    }

    Shortcut {
        enabled: Qt.platform.os === "osx" && !Roost.keymapLoaded
        context: Qt.WindowShortcut
        sequence: "Ctrl+Shift+B"
        onActivated: root.reportBug()
    }

    BugReportDialog {
        id: bugReportDialog

        window: root
    }

    CommandPalette {
        id: commandPalette
    }

    SettingsWindow {
        id: settingsWindow
    }
}
