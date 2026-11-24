import QtQuick
import Roost.Api

// A card of the window (docs/design/18-style.md): `surface` with a 1 px `line`
// outline and rounded corners, on the canvas of the window. What is put in it
// (`content`) fills the card, and the outline is drawn over it. A panel that
// paints a background of its own rounds its corners with `radiusCard` too; a
// panel that paints a square (the terminal) keeps `cornerInset` from the
// edge, where the square lies inside the arc: Qt Quick cannot clip to a round
// shape without drawing the panel into a texture first. The focus ring is
// drawn outside the card, `focusRingWidth` wide; the room for it is the gap
// between the cards and the margin of the window.
//
// A card with `bare` set draws nothing: the panels of a tab stack sit in the
// card of the stack.
Item {
    id: card

    // Whether the ring of the focus is drawn around the card.
    property bool focused: false
    // A card that draws nothing (see above).
    property bool bare: false
    default property alias content: contentArea.data

    Rectangle {
        objectName: "panelCardFace"
        anchors.fill: parent
        visible: !card.bare
        radius: Roost.theme.radiusCard
        color: Roost.theme.surface
    }

    Item {
        id: contentArea

        anchors.fill: parent
    }

    Rectangle {
        objectName: "panelCardOutline"
        anchors.fill: parent
        visible: !card.bare
        radius: Roost.theme.radiusCard
        color: "transparent"
        border.width: 1
        border.color: Roost.theme.line
    }

    // Outside the card: the ring lies on the 3 px around it.
    Rectangle {
        objectName: "panelCardRing"
        anchors.fill: parent
        anchors.margins: -Roost.theme.focusRingWidth
        visible: card.focused && !card.bare
        radius: Roost.theme.radiusCard + Roost.theme.focusRingWidth
        color: "transparent"
        border.width: Roost.theme.focusRingWidth
        border.color: Roost.theme.focus
    }
}
