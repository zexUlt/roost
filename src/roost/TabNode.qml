import QtQuick
import QtQuick.Controls
import Roost.Api

// A stack of tabs of the layout: every child is built, one is shown. A stack
// comes from "Move into tabs" or from a saved layout. A tab the user clicks is
// reported to the shell (`tabChosen`) and takes the keyboard. The bar itself
// never holds the keyboard, so the tab changes only by a click or by the layout.
PanelCard {
    id: stack

    objectName: "tabsCard"

    // Set by the LayoutNode that loads this by file name.
    property var node: null
    property var view: null
    property var path: []

    // The card of the stack: the tabs on top, the shown panel under them.
    focused: stack.view !== null && stack.view.holdsLastFocused(stack.node)

    // The bar goes to the first tab when it gets one; the tab of the document
    // is set after the buttons are there (delayed), and the user's own choice
    // goes on from it.
    Binding {
        target: bar
        property: "currentIndex"
        value: stack.node !== null ? stack.node.active : 0
        delayed: true
        restoreMode: Binding.RestoreNone
    }

    TabBar {
        id: bar

        // The bar is square: off the rounded corners of the card.
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: Math.ceil(Roost.theme.radiusCard * (1 - Math.SQRT1_2)) + 1
        focusPolicy: Qt.NoFocus
        Repeater {
            model: stack.node !== null ? stack.node.children : []

            delegate: TabButton {
                required property var modelData
                required property int index

                focusPolicy: Qt.NoFocus
                text: modelData.kind === "leaf" ? modelData.instance : "Tab " + (index + 1)
                onClicked: stack.choose(index)
            }
        }
    }

    // The user chose a tab: the shell stores it, and the keyboard goes to the
    // first panel shown on it, as the layout commands act on the panel that
    // has the keyboard. Late: the shown panel is known when the page has
    // become visible. What the late part needs is taken now; if the layout was
    // replaced or the stack is gone by then, it does nothing.
    function choose(index) {
        const view = stack.view;
        const chosen = stack.node !== null ? (stack.node.children[index] ?? null) : null;
        view.tabChosen(stack.path, index);
        if (chosen === null) {
            return;
        }
        Qt.callLater(() => {
            if (!view || !stack || stack.node === null || stack.node.children[index] !== chosen || bar.currentIndex !== index) {
                return;
            }
            const leaf = view.firstLeaf(chosen, null);
            if (leaf !== null) {
                view.focusInstance(leaf.instance);
            }
        });
    }

    // The pages lie one over the other; the tab that is chosen is visible.
    Item {
        id: pages

        anchors.top: bar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom

        // An Instantiator for the QObject parent, as in SplitNode.
        Instantiator {
            model: stack.node !== null ? stack.node.children : []

            delegate: LayoutNode {
                required property int index
                required property var modelData

                parent: pages
                stacked: true
                width: pages.width
                height: pages.height
                visible: index === bar.currentIndex
                node: modelData
                view: stack.view
                path: stack.path.concat([index])
            }
        }
    }
}
