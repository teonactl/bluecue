import QtQuick
import QtQuick.Controls

// Placeholder: in uno step successivo mostrerà la lista dei dispositivi
// Bluetooth accoppiati (da blueZManager.devices()) come ComboBox/ListView
// selezionabile per assegnare un dispositivo a una zona.
ComboBox {
    id: selector
    model: []
    displayText: count === 0 ? qsTr("Nessun dispositivo") : currentText
}
