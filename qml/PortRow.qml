import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Riga di una colonna Input o Output. Espone un "connettore" (il pallino sul
// bordo interno della riga) che è il punto di aggancio dei cavi disegnati da
// Main.qml: sulle righe Input è trascinabile (avvia un collegamento), sulle
// righe Output è solo un bersaglio visivo. La riga notifica in continuazione
// la propria posizione globale (anchorChanged) così Main.qml può disegnare i
// cavi anche quando la finestra viene ridimensionata o la lista scorre.
Rectangle {
    id: row
    property int nodeId: -1
    property bool isInput: true
    property string portName: ""
    property bool isBluetooth: false
    property bool selected: false
    // Evidenziata (bordo colorato) ma non riempita come "selected": usata
    // per la traccia scelta con un click (armedCueIndex) mentre NON sta
    // ancora suonando — distinta dalla traccia effettivamente in
    // riproduzione (selected, sfondo pieno), per non confondere le due cose.
    property bool armed: false
    property bool connected: false
    // Solo lato Output: true mentre questo sink sta suonando la sequenza di
    // bip del pulsante "identifica" (patchManager.identifyingSinkId).
    property bool identifying: false
    // Solo lato Output, solo per sink Bluetooth: percentuale batteria da
    // BlueZ (org.bluez.Battery1), -1 = sconosciuta/non esposta dal
    // dispositivo — in quel caso non si mostra nulla, richiesto
    // esplicitamente "se arrivano informazioni sulla batteria".
    property int batteryPercentage: -1
    // Solo lato Output: muto/non muto (SPA_PROP_mute), letto da PipeWire.
    // Sostituisce un precedente slider di volume: rimosso su richiesta
    // esplicita dell'utente dopo aver verificato che il volume software
    // del nodo agisce solo dentro il range già limitato a monte dal mixer
    // di sistema (non controllabile da qui) — il muto è un interruttore
    // netto, non soggetto allo stesso problema, il volume vero resta al
    // mixer di sistema.
    property bool muted: false
    // Indice della riga nella sua lista (ListView "index"), passato dal
    // delegate — CueList lo usa per identificare la cue trascinata anche
    // quando non ha ancora un nodeId live (nodeId <= 0), dato che il
    // routing "voluto" si registra per indice/nome, non più solo per nodeId
    // live (vedi PatchManager::toggleCueOutput). Non usato lato Output.
    property int rowIndex: -1
    // Riordino manuale con pulsanti ▲/▼ (invece di un trascinamento: dentro
    // una ListView un trascinamento verticale viene facilmente "rubato" dal
    // gesto di scorrimento del Flickable, segnalato dall'utente come
    // riordino che non si muove nonostante il click funzioni) — usato sia
    // dalla Playlist (CueList) sia dalla colonna Output (PortColumn), che
    // decidono i limiti (canMoveUp/canMoveDown) in base alla propria lista.
    property bool canMoveUp: true
    property bool canMoveDown: true
    signal moveUpRequested()
    signal moveDownRequested()
    // Item rispetto a cui calcolare le coordinate globali (mapToItem) per
    // anchor/drag. DEVE essere lo stesso item a cui è ancorato il Canvas dei
    // cavi in Main.qml (root.contentItem, non "null"/scena): con un
    // menuBar impostato, l'ApplicationWindow.contentItem parte più in basso
    // dell'inizio reale della finestra, quindi "mappare alla scena" (null)
    // e disegnare dentro contentItem userebbero due origini diverse,
    // sfalsando cavi e aree di rilascio esattamente dell'altezza del menuBar.
    property Item mapTarget: null

    signal removeRequested()
    // Solo lato Output: richiesta di riprodurre due bip udibili su questo
    // sink per identificarlo fisicamente.
    signal identifyRequested()
    // Solo lato Output: doppio click sul nome, per assegnargli un nickname
    // (PatchManager::setOutputNickname) mostrato al posto della descrizione
    // PipeWire grezza sia qui sia nelle etichette di rotazione/pannello
    // Trasforma.
    signal renameRequested()
    // Solo lato Output: l'utente ha premuto il pulsante muto/smuta — vedi
    // PatchManager::setOutputMuted.
    signal muteToggleRequested()
    // Solo lato Input (playlist): tasto destro sulla riga, per aprire il
    // modal di configurazione pre wait/durata/post wait stile QLab.
    signal configureRequested()
    // Emesso di continuo (resize, scroll, cambio nodo per riciclo delegate)
    // con la posizione globale del connettore (cx,cy) e il rettangolo della
    // riga stessa (rx,ry,rw,rh), usato da Main.qml come bersaglio di drop.
    // rowIndex è incluso perché lato Input è la chiave usata per registrare
    // il punto di aggancio anche quando nodeId <= 0 (traccia non ancora in
    // riproduzione): più righe non live condividerebbero altrimenti lo
    // stesso nodeId (0).
    signal anchorChanged(int nodeId, int rowIndex, bool isInput, real cx, real cy, real rx, real ry, real rw, real rh)
    // Solo dal lato Input: inizio/movimento/fine trascinamento di un cavo.
    // Funziona anche quando nodeId <= 0 (traccia non ancora in
    // riproduzione): il routing voluto viene comunque registrato per indice
    // di riga (vedi PatchManager::toggleCueOutput) e applicato in automatico
    // non appena la traccia parte.
    signal dragStarted(int rowIndex, int nodeId, real gx, real gy)
    signal dragMoved(real gx, real gy)
    signal dragEnded(real gx, real gy)

    height: 40
    radius: 6
    color: selected ? "#7F77DD" : "#F1EFE8"
    border.width: selected ? 0 : (armed ? 2 : 1)
    border.color: selected ? "#D3D1C7" : (armed ? "#7F77DD" : "#D3D1C7")

    function reportAnchor() {
        var c = connector.mapToItem(row.mapTarget, connector.width / 2, connector.height / 2)
        var r = row.mapToItem(row.mapTarget, 0, 0)
        row.anchorChanged(row.nodeId, row.rowIndex, row.isInput, c.x, c.y, r.x, r.y, row.width, row.height)
    }

    onXChanged: reportAnchor()
    onYChanged: reportAnchor()
    onWidthChanged: reportAnchor()
    onHeightChanged: reportAnchor()
    onNodeIdChanged: reportAnchor()
    onRowIndexChanged: reportAnchor()
    Component.onCompleted: reportAnchor()

    // Area di interazione per l'intera riga (solo lato Input): il pallino da
    // solo era troppo piccolo/preciso da colpire col mouse. Sta "dietro"
    // (z: -1) al contenuto della riga così il tasto ✕, che ha una sua
    // MouseArea più piccola dentro la RowLayout, mantiene la priorità sulla
    // propria area. Il trascinamento (per collegare/scollegare un output)
    // funziona sempre, anche se la traccia non è ancora in riproduzione: la
    // riproduzione è ormai gestita separatamente dai controlli di trasporto
    // globali (Play/Pausa/Stop in alto, o barra spaziatrice), non dal click
    // sulla riga.
    MouseArea {
        id: rowArea
        anchors.fill: parent
        z: -1
        enabled: row.isInput
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        preventStealing: true

        // Tasto destro: apre il modal di configurazione (pre wait/durata/
        // post wait), non trascina nessun cavo.
        onClicked: (mouse) => {
            if (mouse.button === Qt.RightButton)
                row.configureRequested()
        }

        // Doppio click per rinominare una traccia della playlist (analogo al
        // doppio click sul nome usato lato Output, ma lì la riga non è
        // trascinabile quindi basta una MouseArea sul solo testo — qui invece
        // l'intera riga è già presa dal trascinamento del cavo, quindi si
        // aggancia allo stesso rowArea invece di sovrapporre un'altra
        // MouseArea che ruberebbe la pressione iniziale del drag).
        onDoubleClicked: (mouse) => {
            if (mouse.button === Qt.LeftButton && row.isInput)
                row.renameRequested()
        }

        onPressed: (mouse) => {
            if (mouse.button === Qt.RightButton)
                return
            var p = mapToItem(row.mapTarget, mouse.x, mouse.y)
            row.dragStarted(row.rowIndex, row.nodeId, p.x, p.y)
        }
        onPositionChanged: (mouse) => {
            if (pressed && (mouse.buttons & Qt.RightButton) === 0) {
                var p = mapToItem(row.mapTarget, mouse.x, mouse.y)
                row.dragMoved(p.x, p.y)
            }
        }
        onReleased: (mouse) => {
            if (mouse.button === Qt.RightButton)
                return
            var p = mapToItem(row.mapTarget, mouse.x, mouse.y)
            row.dragEnded(p.x, p.y)
        }
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: row.isInput ? 10 : 16
        anchors.rightMargin: row.isInput ? 16 : 10
        spacing: 8

            // Riordino manuale: due freccette impilate invece di una
            // maniglia da trascinare (vedi nota su canMoveUp/canMoveDown
            // più in alto). z sopra rowArea (che sta dietro a z:-1) così
            // il click qui non fa anche partire un trascinamento di cavo.
            ColumnLayout {
                spacing: 0
                Layout.alignment: Qt.AlignVCenter

                Text {
                    text: "▲"
                    font.pixelSize: 9
                    opacity: row.canMoveUp ? 1 : 0.25
                    color: row.selected ? "white" : "#5C5A52"

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
                        enabled: row.canMoveUp
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: row.moveUpRequested()
                    }
                }

                Text {
                    text: "▼"
                    font.pixelSize: 9
                    opacity: row.canMoveDown ? 1 : 0.25
                    color: row.selected ? "white" : "#5C5A52"

                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -4
                        enabled: row.canMoveDown
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: row.moveDownRequested()
                    }
                }
            }

            // Etichetta testuale invece di un glifo da icon-font: quel
            // placeholder (un codepoint di un font di icone mai incluso nel
            // progetto) veniva mostrato come un quadrato "tofu" -- carattere
            // mancante.
            Rectangle {
                visible: row.isBluetooth
                radius: 3
                color: row.selected ? "#FFFFFF33" : "#4A90D922"
                implicitWidth: btLabel.implicitWidth + 6
                implicitHeight: btLabel.implicitHeight + 2

                Text {
                    id: btLabel
                    anchors.centerIn: parent
                    text: "BT"
                    font.pixelSize: 10
                    font.bold: true
                    color: row.selected ? "white" : "#2980B9"
                }
            }

            Text {
                text: row.portName
                color: row.selected ? "white" : "#2C2C2A"
                elide: Text.ElideRight
                Layout.fillWidth: true

                MouseArea {
                    anchors.fill: parent
                    enabled: !row.isInput
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onDoubleClicked: row.renameRequested()
                }
            }

            // Solo lato Output, solo se BlueZ espone la batteria per questo
            // dispositivo (non tutte le casse/cuffie lo fanno) — richiesto
            // esplicitamente dall'utente. Icona 🔋 per distinguerla a colpo
            // d'occhio dalla percentuale del volume qui accanto (segnalato
            // dall'utente: le due percentuali sembravano la stessa cosa,
            // "non si capisce cosa sia"). Colore d'allarme sotto il 20%.
            Text {
                visible: !row.isInput && row.batteryPercentage >= 0
                text: "🔋 " + row.batteryPercentage + "%"
                font.pixelSize: 11
                color: row.selected
                    ? "white"
                    : (row.batteryPercentage < 20 ? "#C0392B" : "#5C5A52")
            }

            // Solo lato Output: due bip chiaramente udibili su questo
            // altoparlante, per capire fisicamente quale sia tra più casse
            // collegate. Evidenziato (sfondo pieno) mentre row.identifying è
            // vero, così è chiaro quale sink si sta testando.
            Rectangle {
                visible: !row.isInput
                radius: 3
                color: row.identifying ? "#F39C12" : "transparent"
                implicitWidth: identifyLabel.implicitWidth + 4
                implicitHeight: identifyLabel.implicitHeight + 2

                Text {
                    id: identifyLabel
                    anchors.centerIn: parent
                    text: "🔔"
                    font.pixelSize: 13
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: row.identifyRequested()
                }
            }

            // Solo lato Output: muto/smuta. Sostituisce un precedente
            // slider di volume — vedi la nota su Cue::muted/PortRow.muted
            // più in alto: il volume software del nodo agisce solo dentro
            // il range già limitato a monte dal mixer di sistema, un vero
            // controllo del volume da qui non era affidabile.
            Rectangle {
                visible: !row.isInput
                radius: 3
                color: row.muted ? "#C0392B22" : "transparent"
                implicitWidth: muteLabel.implicitWidth + 4
                implicitHeight: muteLabel.implicitHeight + 2

                Text {
                    id: muteLabel
                    anchors.centerIn: parent
                    text: row.muted ? "🔇" : "🔊"
                    font.pixelSize: 13
                }

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: row.muteToggleRequested()
                }
            }

            Text {
                text: "✕"
                color: row.selected ? "white" : "#888780"
                font.pixelSize: 12

                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    onClicked: row.removeRequested()
                }
            }
    }

    // Connettore: pallino sul bordo interno della riga (destro per gli
    // Input, sinistro per gli Output), puramente visivo — l'interazione
    // (click/trascinamento) è gestita da rowArea sopra, su tutta la riga.
    // Su un Input senza nodo live ancora (nodeId <= 0, in coda nella
    // playlist) è blu; una volta live (nodeId > 0) diventa verde/grigio
    // come bersaglio normale. Sugli Output è solo un bersaglio visivo, mai
    // un punto di partenza.
    Rectangle {
        id: connector
        width: 14
        height: 14
        radius: 7
        anchors.verticalCenter: parent.verticalCenter
        anchors.right: row.isInput ? parent.right : undefined
        anchors.left: row.isInput ? undefined : parent.left
        anchors.rightMargin: row.isInput ? -5 : 0
        anchors.leftMargin: row.isInput ? 0 : -5
        color: (row.isInput && row.nodeId <= 0) ? "#4A90D9" : (row.connected ? "#4CAF50" : "#B4B2A9")
        border.color: "white"
        border.width: 1.5

        onXChanged: row.reportAnchor()
        onYChanged: row.reportAnchor()
    }
}
