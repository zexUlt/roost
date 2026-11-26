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
    // How narrow and how low the layout may get: what the root node needs
    // (docs/design/06-panels.md); the window takes it as its own minimum.
    readonly property real minimumWidth: rootNode.minWidth
    readonly property real minimumHeight: rootNode.minHeight
    // The lists of sessions are hidden: collapsed, not closed. Read-only; the
    // command `layout.toggleLists` hides them and shows them again.
    readonly property bool listsHidden: view.tree !== null && view.tree.listsHidden === true
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
    // The user chose a tab of a stack: `path` is the child indices from the
    // root to the stack, `index` the tab.
    signal tabChosen(var path, int index)
    // An edit of the layout, for the shell to apply: the action (splitRight,
    // splitDown, intoTabs, outOfTabs, moveLeft, moveRight, moveUp, moveDown,
    // rotate, swap, tabLeft, tabRight, close, toggleLists, editLinks, setLink,
    // undo, reset) and its
    // arguments, `instance` among them. See roost::Shell::OnLayoutEdit.
    signal layoutEdit(string action, var args)

    // The panel types that "Split with…" offers; written by the shell.
    property var panelTypes: []
    // The panel types that have inputs: only they have links to edit.
    property var panelsWithInputs: []
    // There is a layout edit to undo; written by the shell.
    property bool canUndo: false
    // The instance id of the panel that had the keyboard last, "" if none.
    property string lastFocused: ""
    // The panel that takes the keyboard when its slot is there, "" if none.
    property string pendingFocus: ""
    // The first leaf that waits to take the keyboard at start, "" if none.
    property string startFocus: ""

    // A new layout may have no panel of `lastFocused`. A close of the shell
    // names the neighbour that takes the keyboard (`pendingFocus`), and the ring
    // goes with it. A layout that is replaced names none: the ring goes to its
    // first panel, as the keyboard does at start, and the keyboard follows when
    // the slot that had it is deleted and left it nowhere.
    onTreeChanged: Qt.callLater(view.keepRing)

    function keepRing() {
        if (view.lastFocused === "" || view.pendingFocus !== "" || view.holdsLastFocused(view.tree)) {
            return;
        }
        let leaf = view.tree;
        while (leaf !== null && leaf.kind !== "leaf") {
            leaf = leaf.children[leaf.kind === "tabs" ? Math.min(leaf.active ?? 0, leaf.children.length - 1) : 0] ?? null;
        }
        view.lastFocused = leaf !== null ? leaf.instance : "";
        Qt.callLater(view.returnKeyboard);
    }

    // The keyboard is nowhere (no item, the window itself, or an item of the
    // layout above the slots: the node that held the focus scope of a slot
    // that is gone) and the ring is on a panel of the layout: the panel takes
    // the keyboard. A popup that holds it keeps it.
    function returnKeyboard() {
        if (view.lastFocused === "" || view.pendingFocus !== "" || !view.holdsLastFocused(view.tree)) {
            return;
        }
        const focused = view.Window.activeFocusItem;
        let inView = false;
        let inSlot = false;
        for (let item = focused; item !== null && !inView; item = item.parent) {
            inSlot = inSlot || item.objectName === "panelSlot";
            inView = item === view;
        }
        if (focused === null || focused === view.Window.contentItem || (inView && !inSlot)) {
            view.focusInstance(view.lastFocused);
        }
    }

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
        Qt.callLater(view.returnKeyboard);
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
        const leaf = view.firstLeaf(view.tree, null);
        return leaf !== null ? leaf.instance : "";
    }

    // What can be done with the panel: it is in a stack of tabs (inTabs), it has
    // a sibling in a split to be put in tabs with (canIntoTabs), a tab with a
    // button on its left or right in the stack (canTabLeft, canTabRight), it is
    // not the last panel (canClose), it takes inputs (hasInputs); `others` are
    // the other panels of the window, for "Swap with…".
    function describe(instance) {
        const info = {
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
        };
        let leaves = 0;
        // The nodes from the root down to the parent of a node.
        const above = [];
        const walk = (node, parent) => {
            if (node.kind === "leaf") {
                leaves += 1;
                if (node.instance === instance) {
                    info.hasInputs = view.panelsWithInputs.includes(node.panel);
                    if (parent !== null) {
                        info.inTabs = parent.kind === "tabs";
                        info.canIntoTabs = parent.kind === "split" && parent.children.length > 1;
                        if (info.inTabs) {
                            const at = parent.children.findIndex(child => child.instance === instance);
                            // The tabs of hidden lists have no button: MoveTab steps over them.
                            const hasButton = child => child.hidden !== true;
                            info.canTabLeft = parent.children.slice(0, at).some(hasButton);
                            info.canTabRight = parent.children.slice(at + 1).some(hasButton);
                        }
                    }
                    view.describeMoves(info, node, above);
                } else {
                    info.others.push({
                        instance: node.instance,
                        panel: node.panel
                    });
                }
                return;
            }
            above.push(node);
            for (const child of node.children) {
                walk(child, node);
            }
            above.pop();
        };
        if (view.tree !== null) {
            walk(view.tree, null);
        }
        info.canClose = leaves > 1;
        return info;
    }

    // Fills the move and turn entries of `info` for the leaf `leaf` that has
    // the nodes `above` over it, the root first. As roost::MovePanel and
    // roost::RotateSplit decide: a leaf in a split moves where a neighbour is
    // in that split, or into a parent split that runs that way; a split is
    // turned when there is one above the leaf.
    function describeMoves(info, leaf, above) {
        info.canRotate = above.some(node => node.kind === "split");
        if (above.length === 0 || above[above.length - 1].kind !== "split") {
            return;
        }
        const parent = above[above.length - 1];
        const grand = above.length > 1 ? above[above.length - 2] : null;
        const index = parent.children.findIndex(child => child.kind === "leaf" && child.instance === leaf.instance);
        const can = (sideways, step) => {
            if (parent.horizontal === sideways && index + step >= 0 && index + step < parent.children.length) {
                return true;
            }
            return grand !== null && grand.kind === "split" && grand.horizontal === sideways;
        };
        info.canMoveLeft = can(true, -1);
        info.canMoveRight = can(true, 1);
        info.canMoveUp = can(false, -1);
        info.canMoveDown = can(false, 1);
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
        // The link editor is modal and keeps the keyboard while it is open:
        // the panel it edits takes it when the dialog closes.
        if (linkEditor.visible) {
            return;
        }
        const slot = view.slotOf(view.pendingFocus);
        if (slot !== null) {
            view.pendingFocus = "";
            slot.forceActiveFocus();
        }
    }

    // The first leaf of the layout takes the keyboard at start, once the window
    // has none in it. (Not by `focus`: a Loader is a focus scope, and the leaf
    // and the nodes above it are made one after another.) It runs late, and
    // here: the layout may be replaced before, and the slot that asked, still
    // waiting to be deleted, must not take it.
    function takeKeyboardAtStart(instance) {
        view.startFocus = instance;
        Qt.callLater(view.applyStartFocus);
    }

    function applyStartFocus() {
        const instance = view.startFocus;
        view.startFocus = "";
        // The first leaf by position, shown or not.
        let leaf = view.tree;
        while (leaf !== null && leaf.kind !== "leaf") {
            leaf = leaf.children[0] ?? null;
        }
        const slot = view.slotOf(instance);
        if (slot === null || leaf === null || leaf.instance !== instance) {
            return;
        }
        const focused = view.Window.activeFocusItem;
        if (focused === null || focused === view.Window.contentItem) {
            slot.forceActiveFocus();
        }
    }

    // Asks before the layout is replaced with the default one: the panels that
    // are not in the default go with their place in the layout.
    function askReset() {
        confirmReset.open();
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

    // Whether the panel that had the keyboard last (`lastFocused`) is in
    // `node`: the ring of the focus is on the card of that node.
    function holdsLastFocused(node) {
        if (node === null || view.lastFocused === "") {
            return false;
        }
        if (node.kind === "leaf") {
            return node.instance === view.lastFocused;
        }
        return node.children.some(child => view.holdsLastFocused(child));
    }

    // The first leaf of one of the types (any type for null) whose slot is
    // shown: a leaf on a tab that is not shown cannot take the keyboard.
    function firstLeaf(node, types) {
        if (node === null) {
            return null;
        }
        if (node.kind === "leaf") {
            if (types !== null && !types.includes(node.panel)) {
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

    // The list the keyboard belongs to: the list that has it, the list that
    // feeds the panel that has it, or the first list of the layout. A linked
    // list on a tab that is not shown is skipped (the keyboard cannot land in
    // it), as is a link from a panel that is not a list. null when there is no
    // list.
    function listOfKeyboard() {
        for (const id of Object.keys(view.slots)) {
            const slot = view.slots[id];
            if (!slot.activeFocus) {
                continue;
            }
            if (view.listTypes.includes(slot.panelType)) {
                return slot;
            }
            for (const link of slot.links) {
                const source = view.slotOf(link.from);
                if (source !== null && source.panel !== null && source.visible && view.listTypes.includes(source.panelType)) {
                    return source;
                }
            }
        }
        const leaf = view.firstLeaf(view.tree, view.listTypes);
        return leaf === null ? null : view.slotOf(leaf.instance);
    }

    // Moves the keyboard to the list: the list that feeds the panel that has
    // it, or the first list of the layout. In a list it does nothing.
    function focusList() {
        const slot = view.listOfKeyboard();
        if (slot !== null && !slot.activeFocus) {
            slot.forceActiveFocus();
        }
    }

    // Opens the form of a new session in the list of the keyboard (the same one
    // as focusList): the keyboard goes to its Task field. The lists hidden:
    // nothing happens, as with focusList there is no list to open it in. A list
    // panel without openNewForm (another plugin's list type) is skipped.
    function newSession() {
        if (view.listsHidden) {
            return;
        }
        const slot = view.listOfKeyboard();
        if (slot !== null && slot.panel !== null && typeof slot.panel.openNewForm === "function") {
            slot.panel.openNewForm();
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
            "layout.moveLeft": "moveLeft",
            "layout.moveRight": "moveRight",
            "layout.moveUp": "moveUp",
            "layout.moveDown": "moveDown",
            "layout.rotate": "rotate",
            "layout.swap": "swap",
            "layout.tabLeft": "tabLeft",
            "layout.tabRight": "tabRight",
            "layout.close": "close",
            "layout.toggleLists": "toggleLists",
            "layout.editLinks": "editLinks",
            "layout.undo": "undo"
        };
        for (const id of Object.keys(actions)) {
            Roost.commands.registerCommand(id, args => view.edit(actions[id], args ?? {}, ""));
        }
        Roost.commands.registerCommand("layout.reset", () => view.askReset());
        // sessions.newSession is declared by the manifest of the plugin
        // `sessions`; the window runs it, as it runs the layout commands, because
        // the list it names depends on the links of the panel that has the keyboard.
        Roost.commands.registerCommand("sessions.newSession", () => view.newSession());
    }

    LayoutNode {
        id: rootNode

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

        onClosed: Qt.callLater(view.applyPendingFocus)
        onLinkChosen: (instance, input, from, output) => view.layoutEdit("setLink", {
                instance: instance,
                input: input,
                from: from,
                output: output
            })
    }

    ConfirmDialog {
        id: confirmReset

        title: "Reset the layout to the default?"
        text: "Panels that are not in the default layout are closed. The others get their default places, sizes and links."
        confirmText: "Reset"
        onConfirmed: view.edit("reset", {})
    }

    // Opens the menu of the panel of `slot` at the point (x, y) of the slot.
    function openMenu(slot, x, y) {
        panelMenu.openOn(slot, x, y);
    }
}
