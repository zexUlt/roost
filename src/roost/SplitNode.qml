import QtQuick
import QtQuick.Controls
import Roost.Api

// A split of the layout: its children side by side or stacked. The divider is
// as thin as the gap of the fixed row it replaces, but is grabbed a few pixels
// to either side of the line; dragging it changes the sizes, and the new sizes
// are reported when the drag ends.
//
// Children of size "fill" share what the fixed ones leave, equally. A
// SplitView lets only one item fill, the last "fill" child: the others are
// given their share as a preferred size, which follows the size of the split.
// On a report, the filling child and a child still at its share are "fill"
// again, so that a drag of another divider does not turn the shares into
// pixels; a child the user resized is reported in pixels.
SplitView {
    id: split

    // Set by the LayoutNode that loads this by file name.
    property var node: null
    property var view: null
    property var path: []

    orientation: split.node !== null && !split.node.horizontal ? Qt.Vertical : Qt.Horizontal

    readonly property int handleSize: 1
    // How far from the line a press still takes the divider.
    readonly property int handleGrab: 2
    readonly property bool horizontal: split.node !== null && split.node.horizontal

    handle: Item {
        implicitWidth: split.handleSize
        implicitHeight: split.handleSize

        // The item stays 1 px, so does the gap; the mask only widens the area
        // that takes a press (SplitView finds the divider with contains()).
        containmentMask: QtObject {
            function contains(point: point): bool {
                const along = split.horizontal ? point.x : point.y;
                const across = split.horizontal ? point.y : point.x;
                const length = split.horizontal ? split.height : split.width;
                return along >= -split.handleGrab && along < split.handleSize + split.handleGrab && across >= 0 && across < length;
            }
        }
    }

    // The children of size "fill": how many, and which one fills the rest.
    readonly property int fillCount: {
        let count = 0;
        const total = split.node !== null ? split.node.children.length : 0;
        for (let i = 0; i < total; ++i) {
            if ((split.node.sizes[i] ?? 0) <= 0) {
                ++count;
            }
        }
        return count;
    }
    readonly property int lastFill: {
        const total = split.node !== null ? split.node.children.length : 0;
        for (let i = total - 1; i >= 0; --i) {
            if ((split.node.sizes[i] ?? 0) <= 0) {
                return i;
            }
        }
        return -1;
    }

    // The pixels of the children that are not "fill" (set by recount), and
    // the share of each "fill" child in what is left.
    property real fixedTotal: 0
    readonly property real fillShare: {
        if (split.fillCount < 2 || children.count < 1) {
            return -1;
        }
        const room = (split.horizontal ? split.width : split.height) - split.fixedTotal - (children.count - 1) * split.handleSize;
        return Math.max(40, Math.floor(room / split.fillCount));
    }

    function recount() {
        let total = 0;
        for (let i = 0; i < children.count; ++i) {
            total += children.objectAt(i).fixedPx;
        }
        split.fixedTotal = total;
    }

    // The sizes as they are now: pixels, 0 for a child that takes the rest.
    function reportSizes() {
        const sizes = [];
        for (let i = 0; i < children.count; ++i) {
            const child = children.objectAt(i);
            const pixels = Math.round(split.horizontal ? child.width : child.height);
            const atShare = child.fills && (i === split.lastFill || Math.abs(pixels - split.fillShare) <= 1);
            sizes.push(atShare ? 0 : pixels);
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

        // A new layout replaces the nodes, and the old ones are deleted only
        // when the event loop gets to them. Until then the split lays them out
        // beside the new ones, which are squeezed to their minimum, and a
        // terminal in one of them opens a session of a few columns.
        onObjectRemoved: (index, object) => {
            object.visible = false;
            split.recount();
        }
        onObjectAdded: split.recount()

        delegate: LayoutNode {
            required property int index
            required property var modelData

            parent: split.contentItem

            readonly property int size: split.node.sizes[index] ?? 0
            readonly property bool fills: size <= 0
            // The one "fill" child that the SplitView lets fill; the others
            // of its kind have a share.
            readonly property bool filler: fills && index === split.lastFill
            readonly property real fixedPx: fills ? 0 : (stepped > 0 ? stepped : size)
            onFixedPxChanged: split.recount()

            node: modelData
            view: split.view
            path: split.path.concat([index])

            SplitView.fillWidth: split.node.horizontal && filler
            SplitView.fillHeight: !split.node.horizontal && filler
            // A step of the panel is kept in `stepped`, and goes when the layout
            // brings another node or another size.
            onSizeChanged: stepped = 0
            onNodeChanged: stepped = 0

            readonly property real preferred: filler ? -1 : (fills ? split.fillShare : fixedPx)

            SplitView.preferredWidth: split.node.horizontal ? preferred : -1
            SplitView.preferredHeight: !split.node.horizontal ? preferred : -1
            SplitView.minimumWidth: 40
            SplitView.minimumHeight: 40
        }
    }
}
