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
    // The stack is on a page of another one: it has no card of its own.
    property bool stacked: false
    bare: stack.stacked

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

        // The bar is square: off the rounded corners of the card, if there is one.
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: stack.bare ? 0 : Roost.theme.cornerInset
        focusPolicy: Qt.NoFocus
        // A bigger font makes the bar higher, and the stack needs more.
        onHeightChanged: stack.recountMinimum()
        // The strip the tabs lie on: the `canvas`, so that the current tab,
        // a `surface` one, stands out (derived, no mockup: docs/design/18-style.md).
        background: Rectangle {
            color: Roost.theme.canvas
        }

        Repeater {
            model: stack.node !== null ? stack.node.children : []

            delegate: TabButton {
                id: tab

                required property var modelData
                required property int index
                // The tab the bar is on (the same index the pages follow).
                readonly property bool current: bar.currentIndex === index

                focusPolicy: Qt.NoFocus
                // A tab with nothing to show (a hidden list) has no button.
                // The bar still puts its spacing after it, so it is as wide
                // as minus the spacing: the tabs beside it keep the space
                // they have between two tabs.
                visible: modelData.hidden !== true
                width: visible ? implicitWidth : -bar.spacing
                text: modelData.kind === "leaf" ? modelData.instance : "Tab " + (index + 1)
                font: Roost.theme.rowFont
                // The plate has room around the text (18-style.md, sizes
                // without tokens).
                horizontalPadding: 12
                verticalPadding: 6
                onClicked: stack.choose(index)

                // The Fusion bevel is replaced: the current tab is a `surface`
                // plate with its text in `focus`, the others are
                // `textSecondary`, `hoverBackground` under the mouse.
                contentItem: Text {
                    objectName: "tabText"
                    text: tab.text
                    font: tab.font
                    elide: Text.ElideRight
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    color: tab.current ? Roost.theme.focus : Roost.theme.textSecondary
                }

                background: Rectangle {
                    objectName: "tabPlate"
                    radius: Roost.theme.radiusControl
                    color: tab.current ? Roost.theme.surface : tab.hovered ? Roost.theme.hoverBackground : "transparent"
                }
            }
        }
    }

    // What the stack needs: the largest minimum of its pages, and the bar
    // above the page. A page with nothing to show (a hidden list) needs none.
    property real minimumWidth: 40
    property real minimumHeight: 40

    function recountMinimum() {
        let width = 40;
        let height = 40;
        let shown = false;
        for (let i = 0; i < made.count; ++i) {
            const page = made.objectAt(i);
            if (page === null || page.node === null || page.node.hidden === true) {
                continue;
            }
            shown = true;
            width = Math.max(width, page.minWidth);
            height = Math.max(height, page.minHeight);
        }
        // A stack with no page shown has no bar to keep either.
        stack.minimumWidth = width;
        stack.minimumHeight = shown ? bar.height + height : height;
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
            id: made

            onObjectAdded: stack.recountMinimum()
            onObjectRemoved: stack.recountMinimum()

            model: stack.node !== null ? stack.node.children : []

            delegate: LayoutNode {
                required property int index
                required property var modelData

                parent: pages
                stacked: true
                width: pages.width
                height: pages.height
                visible: index === bar.currentIndex && modelData.hidden !== true
                onMinWidthChanged: stack.recountMinimum()
                onMinHeightChanged: stack.recountMinimum()
                node: modelData
                view: stack.view
                path: stack.path.concat([index])
            }
        }
    }
}
