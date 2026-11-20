import QtQuick
import QtQuick.Controls
import Roost.Api

// The context menu of a panel (design.md §6): the same edits as the commands
// `layout.*`, on the panel it was opened on. It is opened by a right click
// that reaches the PanelSlot (see there), and does nothing itself: every item
// asks the layout view for an edit, which the shell applies.
Menu {
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
            canClose: false
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

    MenuItem {
        objectName: "menuSplitRight"
        text: "Split right"
        onTriggered: root.run("splitRight", {})
    }

    MenuItem {
        objectName: "menuSplitDown"
        text: "Split down"
        onTriggered: root.run("splitDown", {})
    }

    Menu {
        id: splitWith

        objectName: "menuSplitWith"
        title: "Split with…"

        // Two items per panel type: to the right of the panel, or below it.
        Instantiator {
            model: root.panelTypes.length * 2

            delegate: MenuItem {
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

    MenuSeparator {}

    MenuItem {
        objectName: "menuIntoTabs"
        text: "Move into tabs"
        enabled: root.info.canIntoTabs
        onTriggered: root.run("intoTabs", {})
    }

    MenuItem {
        objectName: "menuOutOfTabs"
        text: "Move out of tabs"
        enabled: root.info.inTabs
        onTriggered: root.run("outOfTabs", {})
    }

    MenuItem {
        objectName: "menuEditLinks"
        text: "Edit links…"
        onTriggered: root.run("editLinks", {})
    }

    MenuSeparator {}

    MenuItem {
        objectName: "menuClose"
        text: "Close"
        enabled: root.info.canClose
        onTriggered: root.run("close", {})
    }
}
