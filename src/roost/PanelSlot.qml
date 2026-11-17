import QtQuick

// A place of the window's layout that shows one panel of a plugin. The shell
// (roost::Shell) writes `panel`, `error`, `linkInput` and `linkOutput`; this
// item only shows the panel, or says why there is none. The panel is owned by
// the loader and never deleted from here.
FocusScope {
    id: root

    // The type of panel this slot asks for, e.g. "sessions.list".
    property string panelType
    // The live panel, or null.
    property Item panel: null
    // Why there is no panel; empty when there is one.
    property string error
    // The property of the panel that a link writes to, and the one it reads
    // from; empty when the panel has none, so the link is off.
    property string linkInput
    property string linkOutput

    onPanelChanged: {
        if (panel === null) {
            return;
        }
        panel.parent = root;
        panel.anchors.fill = root;
    }

    Text {
        objectName: "placeholderText"
        anchors.fill: parent
        anchors.margins: 12
        visible: root.panel === null
        text: root.error !== "" ? root.error : root.panelType
        color: "#808080"
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
