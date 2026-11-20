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
    // The panel types that are lists of sessions, as Shell works them out.
    property var listTypes: []
    // Instance id -> its PanelSlot; replaced as a whole when a slot comes or
    // goes, so that a binding that reads it follows.
    property var slots: ({})

    // A slot is built. Emitted before the slot is registered: Shell hands it
    // its panel first, so that a link never sees a slot without one.
    signal slotCreated(var slot)
    // The user finished resizing a split: `path` is the child indices from the
    // root to the split, `sizes` one number per child, 0 for the rest.
    signal splitResized(var path, var sizes)

    function slotOf(instance) {
        return view.slots[instance] ?? null;
    }

    function registerSlot(slot) {
        const copy = Object.assign({}, view.slots);
        copy[slot.instanceId] = slot;
        view.slots = copy;
    }

    function unregisterSlot(slot) {
        if (view.slots[slot.instanceId] !== slot) {
            return;
        }
        const copy = Object.assign({}, view.slots);
        delete copy[slot.instanceId];
        view.slots = copy;
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

    // The first leaf of one of the types whose slot is shown: a leaf on a tab
    // that is not shown cannot take the keyboard.
    function firstLeaf(node, types) {
        if (node === null) {
            return null;
        }
        if (node.kind === "leaf") {
            if (!types.includes(node.panel)) {
                return null;
            }
            const slot = view.slotOf(node.instance);
            return slot !== null && slot.visible ? node : null;
        }
        for (const child of node.children) {
            const found = firstLeaf(child, types);
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
            if (view.listTypes.includes(slot.panelType)) {
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
        const leaf = view.firstLeaf(view.tree, view.listTypes);
        const slot = leaf === null ? null : view.slotOf(leaf.instance);
        if (slot !== null) {
            slot.forceActiveFocus();
        }
    }

    LayoutNode {
        anchors.fill: parent
        node: view.tree
        view: view
        path: []
    }
}
