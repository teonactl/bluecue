import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Colonna Input come playlist stile QLab/Cuelab: solo le tracce aggiunte
// dall'utente (mai sorgenti hardware), in ordine. Più tracce possono essere
// "in riproduzione" in contemporanea (avviarne una con Play/barra
// spaziatrice NON ferma le altre già partite, vedi PatchManager::playCueAt):
// ogni traccia partita ha un proprio nodo PipeWire live (nodeId > 0) e
// quindi un connettore trascinabile per instradarla verso uno o più output;
// le altre sono in coda in attesa del loro turno.
ColumnLayout {
    id: column
    property var cueModel: null
    property int armedIndex: -1
    property int dragSourceNodeId: -1
    property Item mapTarget: null
    // Stesso filtro "solo traccia selezionata" di Main.qml/cableCanvas: senza
    // questo, le etichette di rotazione qui sotto restavano sempre visibili
    // anche per una traccia nascosta dal filtro, mostrando un routing
    // "voluto" (bluez_output_...) come se fosse collegato mentre il cavo
    // corrispondente non veniva disegnato affatto — bug segnalato
    // dall'utente ("non è collegata a nessun sink ma appare la scritta
    // sotto che è collegata").
    property bool showAllPatches: true

    signal addRequested()
    // Aggiungi una sorgente app (Firefox, ecc.) invece di un file — vedi
    // PatchManager::addAppStreamCue. Distinto da addRequested() perché
    // richiede scegliere da un elenco di stream in esecuzione, non un
    // FileDialog.
    signal addAppStreamRequested()
    signal removeRequested(int index)
    signal configureRequested(int index)
    signal renameRequested(int index, string currentName)
    signal anchorChanged(int nodeId, int rowIndex, bool isInput, real cx, real cy, real rx, real ry, real rw, real rh)
    signal dragStarted(int rowIndex, int nodeId, real gx, real gy)
    signal dragMoved(real gx, real gy)
    signal dragEnded(real gx, real gy)

    spacing: 8

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: qsTr("Playlist")
            font.pixelSize: 16
            font.bold: true
            Layout.fillWidth: true
        }

        Button {
            id: addTrackButton
            text: "+"
            implicitWidth: 32
            implicitHeight: 32
            // Contentitem esplicito: lo stile piattaforma di default ha già
            // causato più volte testo invisibile/poco leggibile in questo
            // progetto. Colore BIANCO: questo pulsante usa lo sfondo nativo
            // scuro del tema (come il resto della barra strumenti), un
            // testo scuro forzato era scuro-su-scuro e restava invisibile.
            font.pixelSize: 18
            font.bold: true
            contentItem: Text {
                text: addTrackButton.text
                font: addTrackButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: column.addRequested()
        }

        // Aggiunge una sorgente app in esecuzione (Firefox, ecc.) invece di
        // un file — richiesto esplicitamente dall'utente. Icona diversa
        // (cuffie) per non confonderlo con "+" (che apre un FileDialog).
        Button {
            id: addAppStreamButton
            text: "🎧+"
            implicitWidth: 40
            implicitHeight: 32
            font.pixelSize: 14
            contentItem: Text {
                text: addAppStreamButton.text
                font: addAppStreamButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: column.addAppStreamRequested()
        }
    }

    ListView {
        id: cueListView
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 6
        clip: true
        model: column.cueModel

        delegate: ColumnLayout {
            id: cueDelegate
            width: ListView.view.width
            spacing: 3

            // column.cueModel è un QVariantList (JS array) esposto da
            // PatchManager, non un QAbstractListModel: i ruoli automatici
            // (model.xxx) non sono affidabili per una lista di questo tipo
            // e risultavano "undefined" in pratica. Si indicizza l'array
            // direttamente con "index" (sempre disponibile nei delegate,
            // indipendentemente dal tipo di modello), con un guard contro
            // gli indici transitori fuori range durante rimozioni/riciclo.
            readonly property var cue: (column.cueModel && index >= 0 && index < column.cueModel.length)
                                        ? column.cueModel[index] : null
            // "index" qui sotto è quello del ListView esterno (la traccia);
            // nel Repeater delle etichette di rotazione più in basso lo
            // stesso nome "index" viene rioscurato dall'indice interno
            // (l'output all'interno della traccia) — serve una copia con un
            // nome diverso per poterlo ancora usare lì dentro.
            readonly property int trackIndex: index

            // PortRow (sotto) riporta la propria posizione globale
            // (reportAnchor) solo quando cambia RISPETTO AL PROPRIO
            // GENITORE IMMEDIATO — che da quando le etichette di rotazione
            // hanno reso il delegate un ColumnLayout (PortRow + etichette),
            // non è più la ListView ma QUESTO wrapper. La posizione DENTRO
            // il wrapper resta invariata (PortRow è sempre in cima); a
            // muoversi è il wrapper stesso quando la ListView lo riposiziona
            // (un'altra riga sopra cambia altezza — es. le etichette di
            // un'altra traccia appaiono/scompaiono — o il caricamento di un
            // progetto crea molte righe in una volta). Senza questo, quel
            // riposizionamento non veniva mai notificato: l'ancoraggio
            // registrato restava quello vecchio e i cavi finivano disegnati
            // nel punto sbagliato, a volte sovrapposti a un'altra traccia
            // (bug segnalato dall'utente: "vedo la prima traccia patchata a
            // tutti i sink"). Un cambio di LARGHEZZA invece arriva già a
            // cueRow direttamente (Layout.fillWidth), il che spiega perché
            // aprire/chiudere il pannello Trasforma "sistemava" tutto per
            // caso: cambiava la larghezza dell'intera colonna, non l'altezza
            // di una riga sopra.
            onXChanged: cueRow.reportAnchor()
            onYChanged: cueRow.reportAnchor()

            PortRow {
                id: cueRow
                Layout.fillWidth: true

                nodeId: cueDelegate.cue ? cueDelegate.cue.nodeId : 0
                rowIndex: index
                mapTarget: column.mapTarget
                isInput: true
                isBluetooth: false
                // Riordino manuale con i pulsanti ▲/▼ (vedi PortRow.qml):
                // i limiti dipendono dalla posizione nella ListView, non
                // dal contenuto della cue.
                canMoveUp: index > 0
                canMoveDown: index < cueListView.count - 1
                // Più tracce possono essere "in riproduzione" in
                // contemporanea (avviarne una non ferma le altre): lo stato
                // è per-riga (cue.nodeId/paused/waitingToStart/inPostWait),
                // non più un singolo indice/flag globali.
                portName: (cueDelegate.cue && cueDelegate.cue.waitingToStart ? "⏳  "
                           : cueDelegate.cue && cueDelegate.cue.inPostWait ? "…  "
                           : cueDelegate.cue && cueDelegate.cue.nodeId > 0 && cueDelegate.cue.paused ? "⏸  "
                           : cueDelegate.cue && cueDelegate.cue.nodeId > 0 ? "▶  "
                           : "")
                          + (cueDelegate.cue && cueDelegate.cue.isAppStream ? "🎧 " : "")
                          + (cueDelegate.cue ? cueDelegate.cue.displayName : "")
                selected: cueDelegate.cue ? (cueDelegate.cue.nodeId > 0 || cueDelegate.cue.waitingToStart) : false
                armed: index === column.armedIndex && !(cueDelegate.cue && (cueDelegate.cue.nodeId > 0 || cueDelegate.cue.waitingToStart))
                connected: {
                    if (!cueDelegate.cue)
                        return false
                    var list = patchManager.connectionsModel
                    for (var i = 0; i < list.length; i++) {
                        if (list[i].inputNodeId === cueDelegate.cue.nodeId)
                            return true
                    }
                    return false
                }

                onRemoveRequested: column.removeRequested(index)
                onConfigureRequested: column.configureRequested(index)
                onRenameRequested: column.renameRequested(index, cueDelegate.cue ? cueDelegate.cue.displayName : "")
                onMoveUpRequested: patchManager.moveCue(index, index - 1)
                onMoveDownRequested: patchManager.moveCue(index, index + 1)
                onAnchorChanged: (nid, rIdx, isInp, cx, cy, rx, ry, rw, rh) => column.anchorChanged(nid, rIdx, isInp, cx, cy, rx, ry, rw, rh)
                onDragStarted: (rIndex, nid, gx, gy) => column.dragStarted(rIndex, nid, gx, gy)
                onDragMoved: (gx, gy) => column.dragMoved(gx, gy)
                onDragEnded: (gx, gy) => column.dragEnded(gx, gy)
            }

            // Etichette di rotazione: una per ogni output collegato quando
            // la traccia ha "un output alla volta" attivo (vedi
            // PatchManager::setCueRotateOutputs). La barra spaziatrice, per
            // la traccia in riproduzione, avanza tra queste (fa scattare la
            // successiva evidenziata) invece di partire con la traccia
            // dopo — vedi PatchManager::advanceCue.
            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 22
                Layout.bottomMargin: 2
                visible: cueDelegate.cue && cueDelegate.cue.rotateOutputs && cueDelegate.cue.desiredOutputLabels.length > 0
                         && (column.showAllPatches || index === column.armedIndex)
                spacing: 4

                Repeater {
                    model: cueDelegate.cue ? cueDelegate.cue.desiredOutputLabels : []

                    delegate: Rectangle {
                        readonly property bool active: cueDelegate.cue
                            && cueDelegate.cue.desiredOutputNodeIds[index] !== 0
                            && cueDelegate.cue.desiredOutputNodeIds[index] === cueDelegate.cue.activeOutputNodeId

                        radius: 8
                        color: active ? "#7F77DD" : "#E4E1D6"
                        implicitHeight: rotationLabel.implicitHeight + 4
                        implicitWidth: rotationLabel.implicitWidth + 12

                        Text {
                            id: rotationLabel
                            anchors.centerIn: parent
                            text: modelData
                            font.pixelSize: 10
                            color: active ? "white" : "#5A584F"
                        }

                        // Click per rimuovere questo singolo output dal
                        // routing voluto della traccia — l'unico modo,
                        // prima, era cliccare il cavo disegnato sul Canvas,
                        // ma per un output non al momento connesso (nodeId
                        // 0) nessun cavo viene mai disegnato (nessun
                        // ancoraggio a cui agganciarlo), quindi non c'era
                        // modo di toglierlo dall'elenco. Segnalato
                        // dall'utente: un dispositivo Bluetooth non più
                        // funzionante restava agganciato per sempre a una
                        // traccia, sempre visibile, senza via di rimozione.
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (cueDelegate.cue && index < cueDelegate.cue.desiredOutputNames.length)
                                    patchManager.removeCueDesiredOutputByName(
                                        cueDelegate.trackIndex, cueDelegate.cue.desiredOutputNames[index])
                            }
                        }
                    }
                }

                Item { Layout.fillWidth: true }
            }
        }
    }
}
