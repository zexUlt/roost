import QtQuick
import QtQuick.Controls
import Roost.Api

// One node of the layout: a split, a stack of tabs or a panel slot, as its
// `kind` says. The nodes build each other, so a tree of any depth is shown.
Item {
    id: root

    required property var node
    required property var view
    // The child indices from the root to this node.
    required property var path

    // The node is the first leaf of the layout: it takes the keyboard at start.
    readonly property bool first: root.path.every(index => index === 0)

    // A split or a stack of tabs is loaded by file name: SplitNode and TabNode
    // use this type for their children, and a type that used them back would
    // be a cycle the QML engine refuses.
    Loader {
        anchors.fill: parent
        source: {
            switch (root.node === null ? "" : root.node.kind) {
            case "split":
                return "SplitNode.qml";
            case "tabs":
                return "TabNode.qml";
            default:
                return "";
            }
        }
        onLoaded: {
            // The node last: it makes the children.
            item.view = Qt.binding(() => root.view);
            item.path = Qt.binding(() => root.path);
            item.node = Qt.binding(() => root.node);
        }
    }

    Loader {
        anchors.fill: parent
        sourceComponent: root.node !== null && root.node.kind === "leaf" ? leafComponent : null
    }

    // The width of the panel is stepped by the panel itself (`<`, `>`): within
    // these bounds, in pixels.
    readonly property int widthStep: 20
    readonly property int widthMin: 200
    readonly property int widthMax: 560

    // The split that holds this node resizes it by `step` steps along the
    // split, and the new size is reported once the split has laid out.
    function stepWidth(step) {
        const split = root.SplitView.view;
        if (split === null || split.orientation !== Qt.Horizontal) {
            return;
        }
        const current = root.SplitView.preferredWidth > 0 ? root.SplitView.preferredWidth : root.width;
        root.SplitView.preferredWidth = Math.max(root.widthMin, Math.min(root.widthMax, current + step * root.widthStep));
        Qt.callLater(() => split.reportSizes());
    }

    Component {
        id: leafComponent

        PanelSlot {
            id: slot

            anchors.fill: parent
            objectName: "panelSlot"
            instanceId: root.node.instance
            panelType: root.node.panel
                resolveSource: instance => root.view.slotOf(instance)

            // The first leaf takes the keyboard at start, once the window has
            // none in it. (Not by `focus`: a Loader is a focus scope, and the
            // leaf and the nodes above it are made one after another.)
            function takeKeyboardAtStart() {
                const focused = slot.Window.activeFocusItem;
                if (root.first && (focused === null || focused === slot.Window.contentItem)) {
                    slot.forceActiveFocus();
                }
            }

            Component.onCompleted: {
                root.view.slotCreated(slot);
                root.view.registerSlot(slot);
                Qt.callLater(slot.takeKeyboardAtStart);
            }
            Component.onDestruction: root.view.unregisterSlot(slot)

            // The list panel's optional signals. `activated()`: the user chose
            // a row, so the keyboard goes to the panel the list feeds.
            // `widthStepRequested(step)`: `<` or `>`, a narrower (-1) or a
            // wider (+1) list, within bounds. `respawned(session)`: the agent
            // respawned a session of the list; a panel it feeds that shows it
            // and has the optional function `reopenWhenEnded(session)` opens
            // it again once the old attach has ended. A panel without them is
            // fine.
            Connections {
                target: slot.panel
                ignoreUnknownSignals: true

                function onActivated() {
                    root.view.focusFed(slot.instanceId);
                }

                function onRespawned(session) {
                    root.view.reopenFed(slot.instanceId, session);
                }

                function onWidthStepRequested(step) {
                    root.stepWidth(step);
                }
            }
        }
    }
}
