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
    // The node is on a page of a stack of tabs, or in a split or a stack that
    // is: the card is the stack's, and nothing under it has a card or a ring
    // of its own.
    property bool stacked: false

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
            item.stacked = Qt.binding(() => root.stacked);
            item.path = Qt.binding(() => root.path);
            item.node = Qt.binding(() => root.node);
        }
    }

    Loader {
        anchors.fill: parent
        sourceComponent: root.node !== null && root.node.kind === "leaf" ? leafComponent : null
    }

    // The width of the panel is stepped by the panel itself (`<`, `>`): by
    // `widthStep` pixels, and not past these bounds.
    readonly property int widthStep: 20
    readonly property int widthMin: 200
    readonly property int widthMax: 560
    // The width the user stepped to, 0 if none. SplitNode takes it before the
    // size of the layout, and drops it when the layout gives a new size; it is
    // a property of its own, so that the binding of SplitNode to the layout
    // is not replaced by an assignment.
    property int stepped: 0

    // The split that holds this node resizes it by `step` steps along the
    // split, and the new size is reported once the split has laid out. A step
    // goes from the width the node has, and never against its direction: a
    // width saved over the bound gets narrower by a step, not cut to the
    // bound.
    function stepWidth(step) {
        const split = root.SplitView.view;
        if (split === null || split.orientation !== Qt.Horizontal) {
            return;
        }
        // The step before may not be laid out yet, a held key steps faster.
        const current = root.stepped > 0 ? root.stepped : Math.round(root.width);
        const target = current + step * root.widthStep;
        root.stepped = step > 0 ? Math.min(target, Math.max(current, root.widthMax)) : Math.max(target, Math.min(current, root.widthMin));
        Qt.callLater(() => split.reportSizes());
    }

    Component {
        id: leafComponent

        PanelCard {
            id: card

            objectName: "panelCard"
            bare: root.stacked
            focused: root.view.holdsLastFocused(root.node)

            PanelSlot {
                id: slot

                anchors.fill: parent
                objectName: "panelSlot"
                instanceId: root.node.instance
                panelType: root.node.panel
                resolveSource: instance => root.view.slotOf(instance)

                // The keyboard is where the layout commands act, and the menu
                // takes it first: a right click chooses the panel.
                onActiveFocusChanged: {
                    if (slot.activeFocus) {
                        root.view.lastFocused = slot.instanceId;
                    }
                }
                onMenuRequested: (x, y) => {
                    slot.forceActiveFocus();
                    root.view.openMenu(slot, x, y);
                }

                Component.onCompleted: {
                    root.view.slotCreated(slot);
                    root.view.registerSlot(slot);
                    // Late, and by the view: the layout may be replaced before it
                    // runs, and the slot and `root` with it.
                    if (root.first) {
                        root.view.takeKeyboardAtStart(slot.instanceId);
                    }
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
}
