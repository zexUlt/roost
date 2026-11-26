import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Roost.Api

// "About roost": the icon, the name, the version, the licence and the project
// page, and Copy for the version lines. One top-level window, made once by
// Main.qml and shown again by open(); closing it only hides it. The look is the
// one of the settings window (docs/design/18-style.md): no mockup.
ApplicationWindow {
    id: root

    // Whether "Project page" opens the browser. A test turns it off; linkOpened
    // follows either way.
    property bool opensUrls: true
    // The page of the project.
    readonly property url projectPage: "https://github.com/zexUlt/roost"
    // The C++ side; a test reads it.
    readonly property alias info: info

    // The user pressed the button of the project page.
    signal linkOpened(url page)

    function openProjectPage() {
        root.linkOpened(root.projectPage);
        if (root.opensUrls) {
            Qt.openUrlExternally(root.projectPage);
        }
    }

    // Shows the window, or raises the one that is shown.
    function open() {
        const shown = visible;
        show();
        if (shown) {
            raise();
        }
        requestActivate();
    }

    objectName: "aboutWindow"
    title: "About roost"
    // Wide enough for the row of buttons at any window font; the lines wrap.
    width: Math.max(360, buttons.implicitWidth + 2 * Roost.theme.panelPadding)
    height: content.implicitHeight + 2 * Roost.theme.panelPadding
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

    AboutInfo {
        id: info
    }

    ColumnLayout {
        id: content

        // Escape closes the window, from here or from a button that has the
        // focus. Not a Shortcut: the window of the shell has none but the two
        // that stand in for the table (design/16-keymap.md).
        focus: true
        Keys.onEscapePressed: root.close()
        anchors.fill: parent
        anchors.margins: Roost.theme.panelPadding
        spacing: 6

        Image {
            objectName: "aboutIcon"
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 96
            Layout.preferredHeight: 96
            source: "qrc:/roost/icons/128.png"
            fillMode: Image.PreserveAspectFit
            mipmap: true
        }

        Label {
            objectName: "aboutName"
            Layout.alignment: Qt.AlignHCenter
            text: "roost"
            font: Roost.theme.titleFont
            color: Roost.theme.textStrong
        }

        Label {
            objectName: "aboutVersion"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: info.version
            color: Roost.theme.text
        }

        Label {
            objectName: "aboutLibraries"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: "libghostty-vt " + info.ghosttyVt + ", Qt " + info.qtVersion
            font: Roost.theme.smallFont
            color: Roost.theme.textMuted
        }

        Label {
            objectName: "aboutLicense"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            Layout.topMargin: 8
            text: "Apache-2.0"
            color: Roost.theme.textSecondary
        }

        // The address in words; the button below opens it, so the keyboard
        // reaches it as any other button.
        Label {
            objectName: "aboutLink"
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WrapAnywhere
            text: String(root.projectPage)
            color: Roost.theme.textSecondary
        }

        RowLayout {
            id: buttons

            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: 12
            spacing: 8

            FormButton {
                objectName: "aboutPageButton"
                text: "Project page"
                onClicked: root.openProjectPage()
            }

            FormButton {
                objectName: "aboutCopyButton"
                text: "Copy"
                onClicked: info.copyVersion()
            }

            FormButton {
                objectName: "aboutCloseButton"
                text: "Close"
                onClicked: root.close()
            }
        }
    }
}
