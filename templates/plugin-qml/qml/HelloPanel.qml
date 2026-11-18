import QtQuick
import Roost.Api

// The panel of the template: shows the id of its plugin and whether the window
// is connected to roostd. Replace the content; keep the root an Item.
Item {
    id: root

    // The window writes the id of the plugin here before the panel is shown,
    // so the panel never names its plugin itself.
    property string pluginId

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            objectName: "pluginIdText"
            anchors.horizontalCenter: parent.horizontalCenter
            text: "Plugin " + root.pluginId
            font.pixelSize: 20
            color: "#e0e0e0"
        }

        Text {
            objectName: "connectionText"
            anchors.horizontalCenter: parent.horizontalCenter
            text: Roost.connected ? "connected to roostd" : "not connected"
            color: "#a0a0a0"
        }
    }
}
