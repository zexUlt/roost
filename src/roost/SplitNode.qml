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
// On a report, if no "fill" child but the filling one is off its share, all
// of them are "fill" again, so that a drag of a fixed divider does not turn
// the shares into pixels; if one is, every "fill" child but the filling one is
// reported in pixels, so that the ones the user did not touch stay as they are.
SplitView {
    id: split

    // Set by the LayoutNode that loads this by file name.
    property var node: null
    property var view: null
    property var path: []

    orientation: split.node !== null && !split.node.horizontal ? Qt.Vertical : Qt.Horizontal

    // The handle lies in the gap between two cards and draws nothing.
    readonly property int handleSize: Roost.theme.panelGap
    // How far from the line a press still takes the divider.
    readonly property int handleGrab: 2
    readonly property bool horizontal: split.node !== null && split.node.horizontal

    handle: Item {
        implicitWidth: split.handleSize
        implicitHeight: split.handleSize

        // The item is as wide as the gap; the mask widens the area that takes
        // a press by `handleGrab` to either side (5 px or more, as before) (SplitView finds the divider with contains()).
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
            const child = children.objectAt(i);
            // Not created yet (the Instantiator builds several delegates), or
            // removed and waiting to be deleted.
            if (child === null || child.gone) {
                continue;
            }
            total += child.fixedPx;
        }
        split.fixedTotal = total;
    }

    // The sizes as they are now: pixels, 0 for a child that takes the rest.
    function reportSizes() {
        const pixelsOf = i => {
            const child = children.objectAt(i);
            return Math.round(split.horizontal ? child.width : child.height);
        };
        // A "fill" child other than the filling one is off its share: the
        // user dragged a divider between "fill" children.
        let moved = false;
        for (let i = 0; i < children.count; ++i) {
            if (children.objectAt(i).fills && i !== split.lastFill && Math.abs(pixelsOf(i) - split.fillShare) > 1) {
                moved = true;
            }
        }
        const sizes = [];
        for (let i = 0; i < children.count; ++i) {
            const fillsAsBefore = children.objectAt(i).fills && (i === split.lastFill || !moved);
            sizes.push(fillsAsBefore ? 0 : pixelsOf(i));
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
            object.gone = true;
            object.visible = false;
            split.recount();
        }
        onObjectAdded: split.recount()

        delegate: LayoutNode {
            required property int index
            required property var modelData

            parent: split.contentItem

            // Set when the Instantiator drops this node.
            property bool gone: false

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
