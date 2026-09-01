import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ColumnLayout {
    id: column
    property string title: ""
    property var portModel: null
    property string addButtonText: "+"
    // true per la colonna Input (i connettori trascinabili stanno a destra),
    // false per quella Output (connettori bersaglio, a sinistra).
    property bool isInputColumn: false

    // Passati da Main.qml per evidenziare la riga sorgente durante il
    // trascinamento (lato Input) e la riga bersaglio sotto il cursore
    // (lato Output).
    property int dragSourceNodeId: -1
    property bool dragging: false
    property point dragCurrentPoint: Qt.point(0, 0)
    property var hitTestOutputFn: null
    property Item mapTarget: null

    signal addRequested()
    signal portRemoveRequested(int nodeId)
    signal identifyRequested(int nodeId)
    signal renameRequested(int nodeId, string currentName)
    signal anchorChanged(int nodeId, int rowIndex, bool isInput, real cx, real cy, real rx, real ry, real rw, real rh)
    signal dragStarted(int rowIndex, int nodeId, real gx, real gy)
    signal dragMoved(real gx, real gy)
    signal dragEnded(real gx, real gy)

    spacing: 8

    RowLayout {
        Layout.fillWidth: true

        Label {
            text: column.title
            font.pixelSize: 16
            font.bold: true
            Layout.fillWidth: true
        }

        Button {
            id: addButton
            text: column.addButtonText
            implicitWidth: 32
            implicitHeight: 32
            font.pixelSize: 18
            // Non ci si affida al contentItem/palette di default dello
            // stile piattaforma: questo progetto ha già avuto più bug di
            // testo invisibile/illeggibile per colori di default. Il colore
            // qui è BIANCO, non scuro: a differenza delle card chiare del
            // pannello Trasforma, questo pulsante usa lo sfondo nativo
            // scuro del tema (come il resto del chrome dell'app — barra
            // strumenti, dialog), quindi un testo scuro forzato era
            // scuro-su-scuro e restava invisibile esattamente come prima
            // del fix (segnalato dall'utente: "continuo a non vedere il +
            // e il refresh nei pulsanti").
            contentItem: Text {
                text: addButton.text
                font: addButton.font
                color: "white"
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            onClicked: column.addRequested()
        }
    }

    ListView {
        id: portListView
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 6
        clip: true
        model: column.portModel

        delegate: PortRow {
            width: ListView.view.width
            nodeId: model.nodeId
            mapTarget: column.mapTarget
            isInput: column.isInputColumn
            portName: model.description
            isBluetooth: model.isBluetooth
            batteryPercentage: model.batteryPercentage
            muted: model.muted
            // Riordino manuale con i pulsanti ▲/▼ (vedi PortRow.qml):
            // column.portModel è un PortModel (QAbstractListModel) C++, con
            // moveNode() invocabile direttamente da QML — a differenza della
            // Playlist non serve passare da PatchManager, non c'è nessuno
            // stato aggiuntivo (undo/persistenza) da coordinare qui.
            canMoveUp: index > 0
            canMoveDown: index < portListView.count - 1
            connected: {
                var list = patchManager.connectionsModel
                for (var i = 0; i < list.length; i++) {
                    if (column.isInputColumn ? list[i].inputNodeId === model.nodeId
                                              : list[i].outputNodeId === model.nodeId)
                        return true
                }
                return false
            }
            selected: column.isInputColumn
                ? (column.dragSourceNodeId === model.nodeId)
                : (column.dragging && column.hitTestOutputFn
                   && column.hitTestOutputFn(column.dragCurrentPoint.x, column.dragCurrentPoint.y) === model.nodeId)
            identifying: !column.isInputColumn && patchManager.identifyingSinkId === model.nodeId

            onRemoveRequested: column.portRemoveRequested(model.nodeId)
            onIdentifyRequested: column.identifyRequested(model.nodeId)
            onRenameRequested: column.renameRequested(model.nodeId, model.description)
            onMuteToggleRequested: patchManager.setOutputMuted(model.nodeId, !model.muted)
            onMoveUpRequested: column.portModel.moveNode(index, index - 1)
            onMoveDownRequested: column.portModel.moveNode(index, index + 1)
            onAnchorChanged: (nid, rIdx, isInp, cx, cy, rx, ry, rw, rh) => column.anchorChanged(nid, rIdx, isInp, cx, cy, rx, ry, rw, rh)
            onDragStarted: (rIdx, nid, gx, gy) => column.dragStarted(rIdx, nid, gx, gy)
            onDragMoved: (gx, gy) => column.dragMoved(gx, gy)
            onDragEnded: (gx, gy) => column.dragEnded(gx, gy)
        }
    }
}
