import QtQuick
import QtQuick.Controls
import Roost.Api

// A stack of tabs of the layout: every child is built, one is shown. The
// layout has no way to make a stack yet; a stack in a saved layout is shown
// and can be switched here.
Item {
    id: stack

    // Set by the LayoutNode that loads this by file name.
    property var node: null
    property var view: null
    property var path: []

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

        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        Repeater {
            model: stack.node !== null ? stack.node.children : []

            delegate: TabButton {
                required property var modelData
                required property int index

                text: modelData.kind === "leaf" ? modelData.instance : "Tab " + (index + 1)
            }
        }
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
