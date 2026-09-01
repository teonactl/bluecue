import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Rectangle {
    id: card
    property string zoneName: ""
    property bool connected: false

    height: 64
    radius: 8
    color: connected ? "#1D9E75" : "#B4B2A9"

    RowLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 12

        Label {
            text: card.zoneName
            font.pixelSize: 16
            color: "white"
            Layout.fillWidth: true
        }

        Label {
            text: card.connected ? qsTr("Connesso") : qsTr("Non assegnato")
            color: "white"
        }
    }
}
