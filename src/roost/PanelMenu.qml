import QtQuick
import QtQuick.Controls
import Roost.Api

// The context menu of a panel (design.md §6): the same edits as the commands
// `layout.*`, on the panel it was opened on. It is opened by a right click
// that reaches the PanelSlot (see there), and does nothing itself: every item
// asks the layout view for an edit, which the shell applies.
RoostMenu {
    id: root

    // The layout view that applies the edits.
    property var view: null
    // The panel types that "Split with…" offers: ids.
    property var panelTypes: []
    // The panel the menu is open on, and what can be done with it.
    property string instance
    property var info: ({
            inTabs: false,
            canIntoTabs: false,
            canTabLeft: false,
            canTabRight: false,
            canClose: false,
            canMoveLeft: false,
            canMoveRight: false,
            canMoveUp: false,
            canMoveDown: false,
            canRotate: false,
            hasInputs: false,
            others: []
        })

    // Opens the menu on the panel of `slot` at the point (x, y) of the slot.
    function openOn(slot, x, y) {
        root.instance = slot.instanceId;
        root.info = root.view.describe(root.instance);
        root.popup(slot, x, y);
    }

    function run(action, args) {
        root.view.edit(action, args, root.instance);
    }

    objectName: "panelMenu"

    MenuRow {
        objectName: "menuSplitRight"
        text: "Split right"
        onTriggered: root.run("splitRight", {})
    }

    MenuRow {
        objectName: "menuSplitDown"
        text: "Split down"
        onTriggered: root.run("splitDown", {})
    }

    RoostMenu {
        id: splitWith

        objectName: "menuSplitWith"
        title: "Split with…"

        // Two items per panel type: to the right of the panel, or below it.
        Instantiator {
            model: root.panelTypes.length * 2

            delegate: MenuRow {
                required property int index

                readonly property string type: root.panelTypes[Math.floor(index / 2)]
                readonly property bool toRight: index % 2 === 0

                objectName: "menuSplitWith_" + type + (toRight ? "_right" : "_down")
                text: (toRight ? "Right: " : "Down: ") + type
                onTriggered: root.run(toRight ? "splitRight" : "splitDown", {
                    panel: type
                })
            }
            onObjectAdded: (index, object) => splitWith.insertItem(index, object)
            onObjectRemoved: (index, object) => splitWith.removeItem(object)
        }
    }

    MenuSeparatorRow {}

    MenuRow {
        objectName: "menuIntoTabs"
        text: "Move into tabs"
        enabled: root.info.canIntoTabs
        onTriggered: root.run("intoTabs", {})
    }

    MenuRow {
        objectName: "menuOutOfTabs"
        text: "Move out of tabs"
        enabled: root.info.inTabs
        onTriggered: root.run("outOfTabs", {})
    }

    MenuSeparatorRow {}

    MenuRow {
        objectName: "menuMoveLeft"
        text: "Move left"
        enabled: root.info.canMoveLeft
        onTriggered: root.run("moveLeft", {})
    }

    MenuRow {
        objectName: "menuMoveRight"
        text: "Move right"
        enabled: root.info.canMoveRight
        onTriggered: root.run("moveRight", {})
    }

    MenuRow {
        objectName: "menuMoveUp"
        text: "Move up"
        enabled: root.info.canMoveUp
        onTriggered: root.run("moveUp", {})
    }

    MenuRow {
        objectName: "menuMoveDown"
        text: "Move down"
        enabled: root.info.canMoveDown
        onTriggered: root.run("moveDown", {})
    }

    MenuRow {
        objectName: "menuRotate"
        text: "Turn split"
        enabled: root.info.canRotate
        onTriggered: root.run("rotate", {})
    }

    MenuSeparatorRow {}

    MenuRow {
        objectName: "menuTabLeft"
        text: "Move tab left"
        enabled: root.info.canTabLeft
        onTriggered: root.run("tabLeft", {})
    }

    MenuRow {
        objectName: "menuTabRight"
        text: "Move tab right"
        enabled: root.info.canTabRight
        onTriggered: root.run("tabRight", {})
    }

    RoostMenu {
        id: swapWith

        objectName: "menuSwapWith"
        title: "Swap with…"
        enabled: root.info.others.length > 0

        // One item per other panel of the window: its instance and type.
        Instantiator {
            model: root.info.others

            delegate: MenuRow {
                required property var modelData
                required property int index

                objectName: "menuSwapWith_" + modelData.instance
                text: modelData.instance + " (" + modelData.panel + ")"
                onTriggered: root.run("swap", {
                    with: modelData.instance
                })
            }
            onObjectAdded: (index, object) => swapWith.insertItem(index, object)
            onObjectRemoved: (index, object) => swapWith.removeItem(object)
        }
    }

    MenuRow {
        objectName: "menuEditLinks"
        text: "Edit links…"
        enabled: root.info.hasInputs
        onTriggered: root.run("editLinks", {})
    }

    MenuSeparatorRow {}

    MenuRow {
        objectName: "menuClose"
        text: "Close"
        enabled: root.info.canClose
        onTriggered: root.run("close", {})
    }

    MenuSeparatorRow {}

    MenuRow {
        objectName: "menuUndo"
        text: "Undo layout edit"
        enabled: root.view.canUndo
        onTriggered: root.run("undo", {})
    }

    MenuRow {
        objectName: "menuReset"
        text: "Reset layout…"
        onTriggered: root.view.askReset()
    }
}
