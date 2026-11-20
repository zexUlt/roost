import QtQuick
import Roost.Api

// Every colour comes from the theme; "transparent" is no hue.
Window {
    color: Roost.theme.windowBackground

    Rectangle {
        color: "transparent"
        border.color: Roost.theme.accent
    }

    Text {
        text: "Redwood, whitespace and #hashtags are not colours"
    }

    Text {
        text: "Orange"
        objectName: "Red"
    }

    Text {
        text: "#123"
        property var label: "Tan"
    }
}
