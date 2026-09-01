import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Campo di durata in stile timecode HH:MM:SS:CC (ore:minuti:secondi:
// centesimi), richiesto esplicitamente dall'utente al posto di un singolo
// SpinBox in secondi interi nella configurazione pre/durata/post wait di
// una traccia (Main.qml, cueConfigDialog) — permette di scrivere durate
// precise senza dover calcolare a mano i secondi totali.
//
// NOTA: la prima versione usava QtQuick.Controls TextField con "color"
// impostato direttamente — risultava in campi completamente vuoti
// (segnalato dall'utente con uno screenshot), lo stile nativo di questa
// piattaforma evidentemente ignora anche quella proprietà. Un secondo
// tentativo con TextField + contentItem custom introduceva un problema
// diverso: legare il contentItem a "control.text" in modo dichiarativo E
// riscriverlo in modo imperativo in onTextEdited rompe il binding
// dichiarativo al primo carattere digitato (regola QML: un'assegnazione
// imperativa sostituisce sempre un binding). Fix definitivo: niente
// TextField/Control, solo un TextInput grezzo dentro un Rectangle disegnato
// a mano — stesso principio già usato con successo per il contentItem dello
// SpinBox altrove nel progetto, qui portato all'estremo evitando anche il
// Control stesso.
RowLayout {
    id: root
    spacing: 2

    // Valore in secondi (con frazione fino al centesimo), letto/scritto
    // dall'esterno — stessa unità già usata dal backend
    // (Cue::preWaitSeconds/durationSeconds/postWaitSeconds, double).
    property real seconds: 0

    // Vero mentre uno dei quattro campi ha il focus: usato per sospendere
    // il ricalcolo automatico da "seconds" mentre l'utente sta scrivendo
    // (altrimenti un binding diretto sovrascriverebbe la digitazione in
    // corso ad ogni cambiamento intermedio — stesso principio già usato per
    // loopSpin/cycleSpin in TransformPanel.qml).
    readonly property bool editing: hhField.focused || mmField.focused
        || ssField.focused || ccField.focused

    function pad2(n) {
        n = Math.max(0, Math.min(99, Math.round(n)))
        return (n < 10 ? "0" : "") + n
    }

    function commit() {
        const hh = parseInt(hhField.text, 10) || 0
        const mm = Math.min(59, parseInt(mmField.text, 10) || 0)
        const ss = Math.min(59, parseInt(ssField.text, 10) || 0)
        const cc = Math.min(99, parseInt(ccField.text, 10) || 0)
        root.seconds = hh * 3600 + mm * 60 + ss + cc / 100
    }

    Binding {
        target: hhField; property: "text"
        value: root.pad2(Math.floor(root.seconds / 3600))
        when: !root.editing
    }
    Binding {
        target: mmField; property: "text"
        value: root.pad2(Math.floor(root.seconds / 60) % 60)
        when: !root.editing
    }
    Binding {
        target: ssField; property: "text"
        value: root.pad2(Math.floor(root.seconds) % 60)
        when: !root.editing
    }
    Binding {
        target: ccField; property: "text"
        value: root.pad2(Math.round((root.seconds - Math.floor(root.seconds)) * 100))
        when: !root.editing
    }

    component DigitBox: Rectangle {
        id: box
        property alias text: input.text
        // "activeFocus" è FINAL su Item (ereditata da Rectangle): non si
        // può creare un alias con lo stesso nome, serve un nome diverso.
        property alias focused: input.activeFocus
        property alias validator: input.validator
        signal committed()

        implicitWidth: 36
        implicitHeight: 28
        radius: 3
        color: "#1B1B1B"
        border.width: 1
        border.color: input.activeFocus ? "#7F77DD" : "#5A584F"

        TextInput {
            id: input
            anchors.fill: parent
            anchors.margins: 4
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: "white"
            selectionColor: "#7F77DD"
            selectedTextColor: "white"
            inputMethodHints: Qt.ImhFormattedNumbersOnly
            onEditingFinished: box.committed()
            Keys.onReturnPressed: box.committed()
        }
    }

    DigitBox {
        id: hhField
        validator: IntValidator { bottom: 0; top: 99 }
        onCommitted: root.commit()
    }
    Label { text: ":"; color: "white" }
    DigitBox {
        id: mmField
        validator: IntValidator { bottom: 0; top: 59 }
        onCommitted: root.commit()
    }
    Label { text: ":"; color: "white" }
    DigitBox {
        id: ssField
        validator: IntValidator { bottom: 0; top: 59 }
        onCommitted: root.commit()
    }
    Label { text: ":"; color: "white" }
    DigitBox {
        id: ccField
        validator: IntValidator { bottom: 0; top: 99 }
        onCommitted: root.commit()
    }
}
