import QtQuick
import QtQuick.Controls
import Roost.Api

// A split of the layout: its children side by side or stacked. The divider is
// as thin as the gap of the fixed row it replaces; dragging it changes the
// sizes, and the new sizes are reported when the drag ends.
SplitView {
    id: split

    // Set by the LayoutNode that loads this by file name.
    property var node: null
    property var view: null
    property var path: []

    orientation: split.node !== null && !split.node.horizontal ? Qt.Vertical : Qt.Horizontal

    handle: Item {
        implicitWidth: 1
        implicitHeight: 1
    }

    // The sizes as they are now: pixels, 0 for a child that takes the rest.
    function reportSizes() {
        const sizes = [];
        for (let i = 0; i < children.count; ++i) {
            const child = children.objectAt(i);
            sizes.push(child.fills ? 0 : Math.round(split.node.horizontal ? child.width : child.height));
        }
        split.view.splitResized(split.path, sizes);
    }

    onResizingChanged: {
        if (!split.resizing) {
            split.reportSizes();
        }
    }

    // An Instantiator, not a Repeater: the objects of a Repeater's delegate
    // have no QObject parent, and the window's objects are found from its
    // root (findChildren) by tools and tests; the Instantiator is the parent
    // of what it makes. The visual parent is the content item of the split.
    Instantiator {
        id: children

        model: split.node !== null ? split.node.children : []

        delegate: LayoutNode {
            required property int index
            required property var modelData

            parent: split.contentItem

            readonly property int size: split.node.sizes[index] ?? 0
            readonly property bool fills: size <= 0

            node: modelData
            view: split.view
            path: split.path.concat([index])

            SplitView.fillWidth: split.node.horizontal && fills
            SplitView.fillHeight: !split.node.horizontal && fills
            SplitView.preferredWidth: split.node.horizontal && !fills ? size : -1
            SplitView.preferredHeight: !split.node.horizontal && !fills ? size : -1
            SplitView.minimumWidth: 40
            SplitView.minimumHeight: 40
        }
    }
}
