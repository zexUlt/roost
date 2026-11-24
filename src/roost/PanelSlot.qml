import QtQuick
import QtQuick.Controls
import QtQml.Models
import Roost.Api

// A place of the window's layout that shows one panel of a plugin. The shell
// (roost::Shell) writes `panel`, `error`, `links`, `linkInput` and
// `linkOutput`; this item only shows the panel, or says why there is none, and
// keeps the links of the panel as bindings. The panel is owned by
// the loader and never deleted from here.
FocusScope {
    id: root

    // The type of panel this slot asks for, e.g. "sessions.list".
    property string panelType
    // The id of the panel in the window's layout; empty for a slot of a window
    // that has no layout.
    property string instanceId
    // The live panel, or null.
    property Item panel: null
    // Why there is no panel; empty when there is one.
    property string error
    // The property of the panel that a link writes to, and the one it reads
    // from; empty when the panel has none, so the link is off.
    property string linkInput
    property string linkOutput
    // The links that feed the inputs of this panel and work: [{input, from,
    // output}], `from` the instance id of the slot of the source panel.
    property var links: []
    // The slot of an instance id, or null; given by the layout view.
    property var resolveSource: instance => null

    // The user pressed the right button at (x, y) of the slot, and the panel
    // did not take the press: a terminal whose program tracks the mouse owns
    // it (Shift takes it back), any other panel lets it through. The layout
    // view opens the panel menu.
    signal menuRequested(real x, real y)

    onPanelChanged: {
        if (panel === null) {
            return;
        }
        panel.parent = root;
        panel.anchors.fill = root;
        // Focusing the slot focuses the panel.
        panel.focus = true;
    }

    // One binding per link: the output of the source panel to the input of
    // this one. Off while a panel is missing; an output that is undefined (an
    // uninitialised `property var`) is written as null.
    Instantiator {
        model: root.links

        delegate: Binding {
            required property var modelData
            readonly property var source: root.resolveSource(modelData.from)

            target: root.panel
            property: modelData.input
            value: source && source.panel ? (source.panel[modelData.output] ?? null) : null
            when: root.panel !== null && source !== null && source.panel !== null
        }
    }

    // Under the panel, which lies over it: the right button reaches this only
    // where the panel gave it up.
    MouseArea {
        objectName: "menuArea"
        anchors.fill: parent
        acceptedButtons: Qt.RightButton
        onPressed: mouse => root.menuRequested(mouse.x, mouse.y)
    }

    Label {
        objectName: "placeholderText"
        anchors.fill: parent
        anchors.margins: 12
        visible: root.panel === null
        text: root.error !== "" ? root.error : root.panelType
        color: Roost.theme.textSecondary
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }
}
