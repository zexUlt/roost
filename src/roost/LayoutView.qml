import QtQuick
import Roost.Api

// The layout of the window (design.md §6): builds the tree that roost::Shell
// publishes in `tree` out of splits, stacks of tabs and PanelSlots. Shell
// fills the slots with panels and writes their links; this item only shows
// them, keeps the slots by instance id and reports the sizes the user sets.
Item {
    id: view

    // The tree: {kind: "split", horizontal, children, sizes}, {kind: "tabs",
    // active, children} or {kind: "leaf", instance, panel}. A size is a
    // number of pixels, 0 for the rest. null: nothing is built.
    property var tree: null
    // Instance id -> its PanelSlot; replaced as a whole when a slot comes or
    // goes, so that a binding that reads it follows.
    property var slots: ({})

    // A slot is built. Emitted before the slot is registered: Shell hands it
    // its panel first, so that a link never sees a slot without one.
    signal slotCreated(var slot)
    // The user finished resizing a split: `path` is the child indices from the
    // root to the split, `sizes` one number per child, 0 for the rest.
    signal splitResized(var path, var sizes)
    // An edit of the layout, for the shell to apply: the action (splitRight,
    // splitDown, intoTabs, outOfTabs, close, editLinks, setLink) and its
    // arguments, `instance` among them. See roost::Shell::OnLayoutEdit.
    signal layoutEdit(string action, var args)

    // The panel types that "Split with…" offers; written by the shell.
    property var panelTypes: []
    // The instance id of the panel that had the keyboard last, "" if none.
    property string lastFocused: ""
    // The panel that takes the keyboard when its slot is there, "" if none.
    property string pendingFocus: ""

    function slotOf(instance) {
        return view.slots[instance] ?? null;
    }

    function registerSlot(slot) {
        const copy = Object.assign({}, view.slots);
        copy[slot.instanceId] = slot;
        view.slots = copy;
        if (view.pendingFocus === slot.instanceId) {
            Qt.callLater(view.applyPendingFocus);
        }
    }

    function unregisterSlot(slot) {
        if (view.slots[slot.instanceId] !== slot) {
            return;
        }
        const copy = Object.assign({}, view.slots);
        delete copy[slot.instanceId];
        view.slots = copy;
    }

    // The panel the layout commands act on: the one that has the keyboard,
    // else the one that had it last, else the first one. "" without a layout.
    function targetInstance() {
        for (const id of Object.keys(view.slots)) {
            if (view.slots[id].activeFocus) {
                return id;
            }
        }
        if (view.lastFocused !== "" && view.slotOf(view.lastFocused) !== null) {
            return view.lastFocused;
        }
        const leaf = view.firstLeaf(view.tree, "");
        return leaf !== null ? leaf.instance : "";
    }

    // What can be done with the panel: it is in a stack of tabs, it has a
    // sibling in a split to be put in tabs with, and it is not the last panel.
    function describe(instance) {
        const info = {
            inTabs: false,
            canIntoTabs: false,
            canClose: false
        };
        let leaves = 0;
        const walk = (node, parent) => {
            if (node.kind === "leaf") {
                leaves += 1;
                if (node.instance === instance && parent !== null) {
                    info.inTabs = parent.kind === "tabs";
                    info.canIntoTabs = parent.kind === "split" && parent.children.length > 1;
                }
                return;
            }
            for (const child of node.children) {
                walk(child, node);
            }
        };
        if (view.tree !== null) {
            walk(view.tree, null);
        }
        info.canClose = leaves > 1;
        return info;
    }

    // Asks the shell for the edit on `instance`, the target of the commands
    // if empty.
    function edit(action, args, instance) {
        const target = instance ?? "";
        view.layoutEdit(action, Object.assign({}, args, {
            instance: target !== "" ? target : view.targetInstance()
        }));
    }

    // The keyboard goes to the panel as soon as its slot is there: an edit
    // makes slots, and the panel it names may not have one yet.
    function focusInstance(instance) {
        view.pendingFocus = instance;
        Qt.callLater(view.applyPendingFocus);
    }

    function applyPendingFocus() {
        const slot = view.slotOf(view.pendingFocus);
        if (slot !== null) {
            view.pendingFocus = "";
            slot.forceActiveFocus();
        }
    }

    // The link editor shows the state the shell worked out for a panel.
    function openLinkEditor(state) {
        linkEditor.show(state);
    }

    // The slots whose panels are fed by the panel of `instance`.
    function fedBy(instance) {
        const fed = [];
        for (const id of Object.keys(view.slots)) {
            const slot = view.slots[id];
            if (slot.links.some(link => link.from === instance)) {
                fed.push(slot);
            }
        }
        return fed;
    }

    // The keyboard goes to the panel fed by the list `instance`, if it has one.
    function focusFed(instance) {
        for (const slot of fedBy(instance)) {
            if (slot.panel !== null) {
                slot.forceActiveFocus();
                return;
            }
        }
    }

    function reopenFed(instance, session) {
        for (const slot of fedBy(instance)) {
            const panel = slot.panel;
            if (panel !== null && typeof panel.reopenWhenEnded === "function") {
                panel.reopenWhenEnded(session);
            }
        }
    }

    // The first leaf of the type (any type if empty) whose slot is shown: a
    // leaf on a tab that is not shown cannot take the keyboard.
    function firstLeaf(node, type) {
        if (node === null) {
            return null;
        }
        if (node.kind === "leaf") {
            if (type !== "" && node.panel !== type) {
                return null;
            }
            const slot = view.slotOf(node.instance);
            return slot !== null && slot.visible ? node : null;
        }
        for (const child of node.children) {
            const found = firstLeaf(child, type);
            if (found !== null) {
                return found;
            }
        }
        return null;
    }

    // Moves the keyboard to the list: the list that feeds the panel that has
    // it, or the first list of the layout. In a list it does nothing.
    function focusList() {
        for (const id of Object.keys(view.slots)) {
            const slot = view.slots[id];
            if (!slot.activeFocus) {
                continue;
            }
            if (slot.panelType === "sessions.list") {
                return;
            }
            for (const link of slot.links) {
                const source = view.slotOf(link.from);
                if (source !== null && source.panel !== null) {
                    source.forceActiveFocus();
                    return;
                }
            }
        }
        const leaf = view.firstLeaf(view.tree, "sessions.list");
        const slot = leaf === null ? null : view.slotOf(leaf.instance);
        if (slot !== null) {
            slot.forceActiveFocus();
        }
    }

    // The layout commands (the manifest of the built-in plugin `shell`
    // declares them) act on the panel that has the keyboard, else on the one
    // that had it last; a panel menu does not go through them but names its
    // panel. `Roost.commands` is null in a window that has no API.
    Component.onCompleted: {
        if (Roost.commands === null) {
            return;
        }
        const actions = {
            "layout.splitRight": "splitRight",
            "layout.splitDown": "splitDown",
            "layout.intoTabs": "intoTabs",
            "layout.outOfTabs": "outOfTabs",
            "layout.close": "close",
            "layout.editLinks": "editLinks"
        };
        for (const id of Object.keys(actions)) {
            Roost.commands.registerCommand(id, args => view.edit(actions[id], args ?? {}, ""));
        }
    }

    LayoutNode {
        anchors.fill: parent
        node: view.tree
        view: view
        path: []
    }

    PanelMenu {
        id: panelMenu

        view: view
        panelTypes: view.panelTypes
    }

    LinkEditor {
        id: linkEditor

        onLinkChosen: (instance, input, from, output) => view.layoutEdit("setLink", {
                instance: instance,
                input: input,
                from: from,
                output: output
            })
    }

    // Opens the menu of the panel of `slot` at the point (x, y) of the slot.
    function openMenu(slot, x, y) {
        panelMenu.openOn(slot, x, y);
    }
}
