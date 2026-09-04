import QtQuick
import QtQml
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    width: 900
    height: 640
    visible: true
    title: qsTr("BlueCue")
    // Niente "icon:" qui: QtQuick.Controls ApplicationWindow (a differenza
    // di un semplice QtQuick.Window) non espone questa proprietà in questa
    // versione di Qt — provarla rompeva il caricamento dell'intera UI
    // ("Cannot assign to non-existent property"). L'icona della finestra è
    // già impostata lato C++ (app.setWindowIcon in main.cpp), che copre
    // taskbar/switcher indipendentemente da QML.

    // L'app non imposta uno stile QtQuick.Controls esplicito, quindi i
    // Control (CheckBox, Label, SpinBox, ecc.) prendono i colori di default
    // dello stile della piattaforma — su alcuni ambienti risultava testo
    // chiaro su sfondo chiaro (es. nel pannello "Trasforma") e quindi
    // illeggibile. Impostare la palette qui la propaga a tutti i Control
    // della finestra, sovrascrivibile puntualmente dove serve (es. il testo
    // bianco esplicito sulle righe "selected" in PortRow.qml, invariato).
    palette.windowText: "#2C2C2A"
    palette.text: "#2C2C2A"
    palette.buttonText: "#2C2C2A"

    menuBar: MenuBar {
        Menu {
            title: qsTr("File")

            MenuItem {
                text: qsTr("Nuovo progetto")
                onTriggered: patchManager.newProject()
            }
            MenuItem {
                text: qsTr("Apri progetto...")
                onTriggered: openProjectDialog.open()
            }
            Menu {
                id: recentMenu
                title: qsTr("Apri recenti")
                enabled: patchManager.recentProjects.length > 0

                Instantiator {
                    model: patchManager.recentProjects
                    delegate: MenuItem {
                        text: modelData
                        onTriggered: patchManager.loadProjectFromPath(modelData)
                    }
                    onObjectAdded: (index, object) => recentMenu.insertItem(index, object)
                    onObjectRemoved: (index, object) => recentMenu.removeItem(object)
                }
            }
            MenuSeparator {}
            MenuItem {
                text: qsTr("Salva")
                onTriggered: root.saveProjectOrPromptPath()
            }
            MenuItem {
                text: qsTr("Salva con nome...")
                onTriggered: saveProjectDialog.open()
            }
        }

        Menu {
            title: qsTr("Modifica")

            MenuItem {
                text: qsTr("Annulla")
                enabled: patchManager.canUndo
                onTriggered: patchManager.undo()
            }
            MenuItem {
                text: qsTr("Ripeti")
                enabled: patchManager.canRedo
                onTriggered: patchManager.redo()
            }
        }

        Menu {
            title: qsTr("Impostazioni")

            MenuItem {
                text: qsTr("Mantieni sveglie le casse Bluetooth")
                checkable: true
                checked: patchManager.keepAliveEnabled
                onTriggered: patchManager.keepAliveEnabled = checked
            }
            MenuItem {
                text: qsTr("Ping keepalive...")
                enabled: patchManager.keepAliveEnabled
                onTriggered: keepAlivePingDialog.open()
            }
        }
    }

    // --- Stato del trascinamento cavi (colonna Input -> colonna Output) ---
    property bool dragging: false
    property int dragCueIndex: -1
    property int dragSourceNodeId: -1
    property point dragStartPoint: Qt.point(0, 0)
    property point dragCurrentPoint: Qt.point(0, 0)

    onDragCurrentPointChanged: cableCanvas.requestPaint()
    onDraggingChanged: cableCanvas.requestPaint()

    // Se false, disegna/testa solo i cavi della traccia "in armo"
    // (quella che si sta effettivamente editando), nascondendo quelli di
    // tutte le altre — utile con playlist lunghe dove i cavi di ogni
    // traccia altrimenti si sovrappongono.
    property bool showAllPatches: true
    onShowAllPatchesChanged: cableCanvas.requestPaint()

    // Colore diverso per i cavi di ogni traccia, per distinguerli a colpo
    // d'occhio quando sono visibili insieme (showAllPatches: true).
    readonly property var cableColors: [
        "#7F77DD", "#E67E22", "#16A085", "#C0392B",
        "#2980B9", "#8E44AD", "#D35400", "#27AE60",
        "#2C3E50", "#F39C12"
    ]
    function colorForCue(index) {
        return index >= 0 ? cableColors[index % cableColors.length] : "#7F77DD"
    }

    // Pannello opzionale "Trasforma" (loop/reverse/rotazione output per
    // traccia), nascosto di default: sostituisce il placeholder PatchGrid
    // al centro quando attivato dal pulsante in barra.
    property bool showTransformPanel: false
    onShowTransformPanelChanged: cableCanvas.requestPaint()

    // Più tracce possono essere in riproduzione insieme (vedi
    // PatchManager::playCueAt): i pulsanti di trasporto globali non possono
    // più abilitarsi/disabilitarsi in base a un singolo indice/flag, vanno
    // calcolati scorrendo cueModel.
    function anyCuePlaying() {
        var list = patchManager.cueModel
        for (var i = 0; i < list.length; i++)
            if (list[i].nodeId > 0 || list[i].waitingToStart)
                return true
        return false
    }
    function anyCuePausable() {
        var list = patchManager.cueModel
        for (var i = 0; i < list.length; i++)
            if (list[i].nodeId > 0 && !list[i].paused && !list[i].waitingToStart)
                return true
        return false
    }
    function anyCueResumable() {
        var list = patchManager.cueModel
        for (var i = 0; i < list.length; i++)
            if (list[i].paused)
                return true
        return false
    }

    // Registri posizione (in coordinate globali/finestra) aggiornati di
    // continuo dalle righe tramite anchorChanged. Lato Input si indicizza
    // per rowIndex (non per nodeId): una traccia in coda non ancora in
    // riproduzione ha nodeId 0, valore che più righe diverse
    // condividerebbero contemporaneamente in una mappa per-nodeId — e il
    // routing "voluto" (vedi PatchManager::desiredOutputNames) deve poter
    // essere disegnato/testato anche per tracce non live. Lato Output i
    // nodi sono sempre live, quindi restano indicizzati per nodeId; in più
    // si registra il rettangolo della riga (per il rilascio: si può
    // lasciare il cavo in un punto qualsiasi della riga, non solo
    // esattamente sul pallino).
    property var inputAnchorsByIndex: ({})
    property var outputAnchors: ({})
    property var outputRects: ({})

    function registerAnchor(nodeId, rowIndex, isInput, cx, cy, rx, ry, rw, rh) {
        if (isInput) {
            inputAnchorsByIndex[rowIndex] = Qt.point(cx, cy)
        } else {
            outputAnchors[nodeId] = Qt.point(cx, cy)
            outputRects[nodeId] = { x: rx, y: ry, w: rw, h: rh }
        }
        cableCanvas.requestPaint()
    }

    function hitTestOutput(px, py) {
        for (var key in outputRects) {
            var r = outputRects[key]
            if (px >= r.x && px <= r.x + r.w && py >= r.y && py <= r.y + r.h)
                return parseInt(key)
        }
        return -1
    }

    // Distanza minima da un punto a un segmento (px,py) - (x1,y1)-(x2,y2).
    function distanceToSegment(px, py, x1, y1, x2, y2) {
        var dx = x2 - x1
        var dy = y2 - y1
        var lenSq = dx * dx + dy * dy
        var t = lenSq > 0 ? ((px - x1) * dx + (py - y1) * dy) / lenSq : 0
        t = Math.max(0, Math.min(1, t))
        var cx = x1 + t * dx
        var cy = y1 + t * dy
        var ddx = px - cx
        var ddy = py - cy
        return Math.sqrt(ddx * ddx + ddy * ddy)
    }

    // Distanza minima da un punto alla curva di Bezier disegnata da drawCable
    // (stessi punti di controllo), campionata a segmenti.
    function distanceToCable(px, py, x1, y1, x2, y2) {
        var midX = x1 + (x2 - x1) * 0.5
        var steps = 24
        var prevX = x1
        var prevY = y1
        var minDist = Infinity
        for (var i = 1; i <= steps; i++) {
            var t = i / steps
            var mt = 1 - t
            var x = mt * mt * mt * x1 + 3 * mt * mt * t * midX + 3 * mt * t * t * midX + t * t * t * x2
            var y = mt * mt * mt * y1 + 3 * mt * mt * t * y1 + 3 * mt * t * t * y2 + t * t * t * y2
            var d = distanceToSegment(px, py, prevX, prevY, x, y)
            if (d < minDist)
                minDist = d
            prevX = x
            prevY = y
        }
        return minDist
    }

    // Trova il cavo di routing "voluto" (vedi PatchManager::cueModel /
    // desiredOutputNodeIds — disegnato indipendentemente dal fatto che la
    // traccia sia effettivamente in riproduzione) che passa più vicino a
    // (px,py), entro una soglia di qualche pixel — usata per rimuovere una
    // singola connessione cliccando direttamente sul suo cavo. Esclude i
    // punti troppo vicini a uno dei due connettori (endpointExclusion): un
    // cavo parte esattamente dal connettore di origine, quindi senza questa
    // esclusione premere di nuovo su un connettore già collegato (per
    // trascinare un SECONDO cavo verso un altro output) veniva scambiato
    // per un click di rimozione sul cavo esistente, e il trascinamento non
    // partiva mai.
    function findConnectionNear(px, py) {
        var threshold = 8
        var endpointExclusion = 16
        var cues = patchManager.cueModel
        for (var i = 0; i < cues.length; i++) {
            if (!showAllPatches && i !== patchManager.armedCueIndex)
                continue
            var start = inputAnchorsByIndex[i]
            if (!start)
                continue
            var outputs = cues[i].desiredOutputNodeIds
            for (var j = 0; j < outputs.length; j++) {
                var end = outputAnchors[outputs[j]]
                if (!end)
                    continue
                if (Math.hypot(px - start.x, py - start.y) < endpointExclusion
                    || Math.hypot(px - end.x, py - end.y) < endpointExclusion)
                    continue
                if (distanceToCable(px, py, start.x, start.y, end.x, end.y) <= threshold)
                    return { cueIndex: i, outputNodeId: outputs[j] }
            }
        }
        return null
    }

    function beginDrag(cueIndex, nodeId, gx, gy) {
        dragging = true
        dragCueIndex = cueIndex
        dragSourceNodeId = nodeId
        dragStartPoint = Qt.point(gx, gy)
        dragCurrentPoint = Qt.point(gx, gy)
    }

    function updateDrag(gx, gy) {
        if (!dragging)
            return
        dragCurrentPoint = Qt.point(gx, gy)
    }

    function endDrag(gx, gy) {
        if (!dragging)
            return

        const movedDist = Math.hypot(gx - dragStartPoint.x, gy - dragStartPoint.y)
        if (movedDist < 8) {
            // Click, non trascinamento: sceglie questa traccia come "in
            // armo" (partirà con Play/barra spaziatrice), senza avviarla
            // subito — è l'unico modo per scegliere liberamente quale
            // traccia riprodurre, dato che il resto della riga serve solo a
            // definire il routing.
            patchManager.armCue(dragCueIndex)
        } else {
            var targetNodeId = hitTestOutput(gx, gy)
            if (targetNodeId !== -1)
                patchManager.toggleCueOutput(dragCueIndex, targetNodeId)
        }

        dragging = false
        dragCueIndex = -1
        dragSourceNodeId = -1
    }

    Connections {
        target: patchManager
        function onPatchError(message) {
            errorLabel.text = message
            errorLabel.visible = true
            errorHideTimer.restart()
        }
        function onConnectionsChanged() {
            cableCanvas.requestPaint()
        }
        function onCuesChanged() {
            cableCanvas.requestPaint()
        }
    }

    Timer {
        id: errorHideTimer
        interval: 5000
        onTriggered: errorLabel.visible = false
    }

    FileDialog {
        id: fileDialog
        title: qsTr("Scegli un file audio")
        nameFilters: [qsTr("File audio (*.wav *.mp3 *.flac *.ogg)"), qsTr("Tutti i file (*)")]
        onAccepted: patchManager.addCueFile(selectedFile)
    }

    FileDialog {
        id: saveProjectDialog
        title: qsTr("Salva progetto")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "btmzproj"
        nameFilters: [qsTr("Progetti BlueCue (*.btmzproj)")]
        onAccepted: patchManager.saveProject(selectedFile)
    }

    FileDialog {
        id: openProjectDialog
        title: qsTr("Apri progetto")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("Progetti BlueCue (*.btmzproj)"), qsTr("Tutti i file (*)")]
        onAccepted: patchManager.loadProject(selectedFile)
    }

    // Barra spaziatrice: avanza la playlist (avvia la traccia "in armo",
    // SENZA fermare quelle già in riproduzione), come in QLab/Cuelab.
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        onActivated: patchManager.advanceCue()
    }

    // Esc: "panic" in stile QLab, ferma subito tutto quello che sta
    // suonando (senza toccare la playlist né la coda), come il pulsante
    // "Stop" qui sotto.
    Shortcut {
        sequence: "Escape"
        context: Qt.ApplicationShortcut
        onActivated: patchManager.stopAllCues()
    }

    // Ctrl+S: salva sul percorso già noto (progetto aperto/già salvato in
    // precedenza) senza chiedere nulla; se il progetto è nuovo e non ha
    // ancora un percorso, si comporta come "Salva con nome" — stesso
    // comportamento standard di qualunque editor.
    function saveProjectOrPromptPath() {
        if (!patchManager.saveProjectToCurrentPath())
            saveProjectDialog.open()
    }

    Shortcut {
        sequence: StandardKey.Save
        context: Qt.ApplicationShortcut
        onActivated: root.saveProjectOrPromptPath()
    }

    // Annulla/ripeti (richiesto esplicitamente dall'utente) — copre solo
    // le operazioni strutturali sulla playlist, vedi
    // PatchManager::pushUndoSnapshot.
    Shortcut {
        sequence: StandardKey.Undo
        context: Qt.ApplicationShortcut
        enabled: patchManager.canUndo
        onActivated: patchManager.undo()
    }
    Shortcut {
        sequence: StandardKey.Redo
        context: Qt.ApplicationShortcut
        enabled: patchManager.canRedo
        onActivated: patchManager.redo()
    }

    // Conferma per le azioni distruttive (rimuovi traccia, rimuovi output,
    // scollega tutto) — richiesto esplicitamente dall'utente. Un'unica
    // Dialog riusabile invece di tre separate: "pendingAction" tiene la
    // funzione da eseguire se l'utente conferma.
    Dialog {
        id: confirmDialog
        property var pendingAction: null
        property string message: ""

        title: qsTr("Conferma")
        modal: true
        standardButtons: Dialog.Yes | Dialog.No
        anchors.centerIn: parent

        function ask(msg, action) {
            confirmDialog.message = msg
            confirmDialog.pendingAction = action
            confirmDialog.open()
        }

        onAccepted: {
            if (confirmDialog.pendingAction)
                confirmDialog.pendingAction()
            confirmDialog.pendingAction = null
        }
        onRejected: confirmDialog.pendingAction = null

        Label {
            width: 280
            wrapMode: Text.WordWrap
            color: "white"
            text: confirmDialog.message
        }
    }

    // Rinomina di un sink Output (doppio click sul nome della riga, vedi
    // PortRow.qml/PortColumn.qml) o di una traccia della Playlist (doppio
    // click sulla riga, vedi PortRow.qml/CueList.qml): stesso dialog per
    // entrambi i casi ("kind" seleziona a chi si applica il nome), il
    // nickname/nome sostituisce la descrizione grezza ovunque venga
    // mostrato (colonna Output/Playlist, etichette di rotazione e
    // riepilogo nel pannello Trasforma).
    Dialog {
        id: renameDialog
        property string kind: "output" // "output" oppure "cue"
        property int nodeId: -1
        property int cueIndex: -1

        title: kind === "cue" ? qsTr("Rinomina traccia") : qsTr("Rinomina output")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent

        function openForOutput(nodeId, currentName) {
            renameDialog.kind = "output"
            renameDialog.nodeId = nodeId
            nicknameField.text = currentName
            renameDialog.open()
        }
        function openForCue(cueIndex, currentName) {
            renameDialog.kind = "cue"
            renameDialog.cueIndex = cueIndex
            nicknameField.text = currentName
            renameDialog.open()
        }

        onAccepted: {
            if (renameDialog.kind === "cue") {
                if (renameDialog.cueIndex >= 0)
                    patchManager.setCueDisplayName(renameDialog.cueIndex, nicknameField.text)
            } else if (renameDialog.nodeId >= 0) {
                patchManager.setOutputNickname(renameDialog.nodeId, nicknameField.text)
            }
        }
        onOpened: {
            // Il Dialog non passa automaticamente il focus tastiera al
            // TextField figlio: senza forceActiveFocus(), selectAll() da
            // solo non bastava a poter scrivere subito — serviva prima un
            // click manuale sul campo, segnalato dall'utente come attrito
            // inutile ("deve essere subito editabile senza ulteriore click").
            nicknameField.forceActiveFocus()
            nicknameField.selectAll()
        }

        TextField {
            id: nicknameField
            width: 260
            placeholderText: renameDialog.kind === "cue" ? qsTr("Nome per questa traccia") : qsTr("Nome per questo output")
            // NIENTE "color" forzato qui: a differenza del pannello
            // Trasforma (sfondi custom chiari con testo scuro forzato), il
            // riquadro di un TextField in questo dialog usa lo sfondo
            // NATIVO del tema (scuro), quindi un testo scuro forzato
            // sarebbe scuro-su-scuro — visibile solo mentre selezionato
            // (grazie a selectedTextColor, non a "color"), invisibile
            // appena deselezionato. Il colore di default dello stile
            // nativo va già bene qui (testo chiaro su sfondo scuro, come il
            // resto del chrome dell'app) — segnalato dall'utente con uno
            // screenshot del testo "JBL tre" visibile SOLO da selezionato.
            Keys.onReturnPressed: renameDialog.accept()
        }
    }

    // Ritardo di output (pulsante "⏱" sulla riga Output, vedi PortRow.qml)
    // — richiesto esplicitamente dall'utente: l'audio interno arriva prima
    // di quello trasmesso via Bluetooth (che aggiunge trasporto+decodifica
    // A2DP), quindi ritardare l'output più veloce li fa suonare di nuovo
    // insieme. PatchManager::setOutputDelayMs applica il valore subito e lo
    // persiste per nome stabile del sink (sopravvive a riavvii/
    // riconnessioni, come il nickname).
    Dialog {
        id: delayDialog
        property int nodeId: -1

        title: qsTr("Ritardo output")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent

        function openForOutput(nodeId, currentDelayMs) {
            delayDialog.nodeId = nodeId
            delaySpinBox.value = currentDelayMs
            delayDialog.open()
        }

        onAccepted: {
            if (delayDialog.nodeId >= 0)
                patchManager.setOutputDelayMs(delayDialog.nodeId, delaySpinBox.value)
        }
        onOpened: delaySpinBox.forceActiveFocus()

        ColumnLayout {
            spacing: 8

            Label {
                text: qsTr("Ritarda l'audio verso questo output per farlo suonare in sincrono con un altro output più lento (tipicamente una cassa Bluetooth).")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 280
            }

            SpinBox {
                id: delaySpinBox
                from: 0
                to: 2000
                stepSize: 10
                editable: true
                textFromValue: (value) => value + " ms"
                valueFromText: (text) => parseInt(text, 10) || 0
                Keys.onReturnPressed: delayDialog.accept()
            }

            Rectangle { Layout.fillWidth: true; height: 1; color: "#5C5A52" }

            // Calibrazione automatica (richiesta esplicitamente
            // dall'utente: non riusciva a trovare a orecchio il valore
            // giusto) — riproduce un breve click su questo output e su un
            // secondo a scelta, lo registra col microfono e calcola da
            // solo il ritardo giusto invece di doverlo indovinare a mano.
            Label {
                text: qsTr("Oppure calibra automaticamente confrontando questo output con un altro (un breve click di test su entrambi, registrato con un microfono).")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 280
            }

            Label { text: qsTr("Confronta con:") }
            ComboBox {
                id: otherOutputCombo
                Layout.fillWidth: true
                model: patchManager.outputs
                textRole: "description"
                valueRole: "nodeId"
            }

            Label {
                text: patchManager.microphonesModel.length > 0
                      ? qsTr("Microfono: %1").arg(patchManager.microphonesModel[0].description)
                      : qsTr("Nessun microfono rilevato — impossibile calibrare automaticamente")
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 280
                color: patchManager.microphonesModel.length > 0 ? "#CFCFC9" : "#C0392B"
            }

            Button {
                id: calibrateButton
                Layout.fillWidth: true
                text: patchManager.calibrationInProgress ? qsTr("Calibrazione in corso… (~2s)") : qsTr("🎯 Calibra automaticamente")
                enabled: !patchManager.calibrationInProgress
                         && patchManager.microphonesModel.length > 0
                         && otherOutputCombo.currentValue !== delayDialog.nodeId
                background: Rectangle {
                    radius: 4
                    color: calibrateButton.enabled ? (calibrateButton.pressed ? "#6961C9" : "#7F77DD") : "#9A98A6"
                }
                contentItem: Text {
                    text: calibrateButton.text
                    font: calibrateButton.font
                    color: "white"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: {
                    calibrationResultLabel.text = ""
                    patchManager.calibrateOutputDelay(delayDialog.nodeId, otherOutputCombo.currentValue,
                                                       patchManager.microphonesModel[0].nodeId)
                }
            }

            Label {
                id: calibrationResultLabel
                visible: text.length > 0
                wrapMode: Text.WordWrap
                Layout.preferredWidth: 280
            }
        }

        // Chiude da solo il dialog dopo una calibrazione riuscita —
        // richiesto esplicitamente dall'utente ("vorrei che si inserisse
        // automaticamente il valore trovato e confermasse da solo"): il
        // valore è già applicato e persistito lato PatchManager appena
        // arriva calibrationResult, questo timer serve solo a lasciare
        // un attimo per leggere il messaggio prima di chiudere.
        Timer {
            id: autoConfirmTimer
            interval: 900
            onTriggered: delayDialog.close()
        }

        Connections {
            target: patchManager
            function onCalibrationResult(success, message, nodeIdA, delayMsA, nodeIdB, delayMsB) {
                calibrationResultLabel.text = message
                calibrationResultLabel.color = success ? "#7FD37F" : "#E57373"
                if (!success)
                    return
                if (delayDialog.nodeId === nodeIdA)
                    delaySpinBox.value = delayMsA
                else if (delayDialog.nodeId === nodeIdB)
                    delaySpinBox.value = delayMsB
                autoConfirmTimer.restart()
            }
        }
    }

    // Elenco degli stream audio applicativi in esecuzione (Firefox, ecc.,
    // patchManager.appStreamsModel) selezionabili come sorgente in playlist
    // — richiesto esplicitamente dall'utente. Sceglierne uno aggiunge una
    // cue "sposta audio app qui" (PatchManager::addAppStreamCue): quando
    // avviata (come una cue normale), reindirizza quello stream in un sink
    // virtuale nostro invece di lasciarlo sull'uscita di sistema di
    // default — "sposta", non "duplica", scelto esplicitamente
    // dall'utente. Stesso stile del dialog dispositivi Bluetooth qui sopra.
    Dialog {
        id: appStreamPickerDialog
        title: qsTr("Aggiungi sorgente app")
        standardButtons: Dialog.Close
        modal: true
        anchors.centerIn: parent
        width: 380

        ColumnLayout {
            width: parent.width
            spacing: 8

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Sposta l'audio di un'app già in riproduzione nella patch bay (smette di sentirsi sull'uscita di sistema).")
                color: "#CFCFC9"
            }

            Label {
                visible: patchManager.appStreamsModel.length === 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Nessuno stream audio applicativo rilevato al momento. Avvia la riproduzione nell'app (es. un video su Firefox), poi riapri questo elenco.")
                color: "#CFCFC9"
            }

            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 260)
                clip: true
                model: patchManager.appStreamsModel
                spacing: 4

                delegate: Rectangle {
                    width: ListView.view.width
                    height: appStreamRow.implicitHeight + 12
                    color: "#F0F0EE"
                    border.color: "#C9C9C4"
                    radius: 4

                    RowLayout {
                        id: appStreamRow
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: modelData.description
                            color: "#2C2C2A"
                        }

                        Button {
                            id: addAppStreamPickButton
                            text: qsTr("Aggiungi")
                            background: Rectangle {
                                radius: 4
                                color: addAppStreamPickButton.pressed ? "#6961C9" : "#7F77DD"
                            }
                            contentItem: Text {
                                text: addAppStreamPickButton.text
                                font: addAppStreamPickButton.font
                                color: "white"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: {
                                patchManager.addAppStreamCue(modelData.nodeId)
                                appStreamPickerDialog.close()
                            }
                        }
                    }
                }
            }
        }
    }

    // Elenco dei dispositivi Bluetooth accoppiati (blueZManager.deviceModel,
    // un QVariantList aggiornato reattivamente da BlueZManager::refreshDevices
    // e dagli esiti di connectDevice/disconnectDevice). "Connetti" avvia
    // patchManager.addBluetoothOutput (BlueZ Connect() + il sink A2DP compare
    // poi da solo in colonna Output via il discovery PipeWire esistente,
    // nessun collegamento manuale qui). "Disconnetti" chiama BlueZ
    // direttamente: non serve passare da PatchManager, la rimozione del sink
    // dalla colonna Output è già gestita da nodeRemoved.
    Dialog {
        id: bluetoothDialog
        title: qsTr("Dispositivi Bluetooth")
        standardButtons: Dialog.Close
        modal: true
        anchors.centerIn: parent
        width: 380

        onOpened: blueZManager.refreshDevices()

        ColumnLayout {
            width: parent.width
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Dispositivi accoppiati")
                    font.bold: true
                    // Nessun colore esplicito qui prima: erediva il
                    // palette.windowText scuro impostato sull'
                    // ApplicationWindow (pensato per gli sfondi chiari
                    // custom come le righe Output/Input), ma questo dialog
                    // ha uno sfondo nativo scuro come gli altri — testo
                    // scuro su scuro, segnalato dall'utente ("stesso
                    // problema di visualizzazione ancora sul menu
                    // bluetooth") dopo che i soli pulsanti erano già stati
                    // sistemati.
                    color: "white"
                }
                Button {
                    id: refreshDevicesButton
                    text: qsTr("Aggiorna")
                    // Uno sfondo scuro forzato con testo bianco appena
                    // provato in questo stesso pulsante era diventato
                    // bianco su bianco altrove nella stessa finestra
                    // (Connetti/Disconnetti qui sotto) — evidentemente lo
                    // sfondo di un Button "nudo" con questo stile nativo
                    // non è affidabile/prevedibile a seconda del contesto.
                    // Fix robusto: sfondo E testo entrambi espliciti, così
                    // il contrasto non dipende più da cosa fa lo stile di
                    // default in quel punto specifico.
                    background: Rectangle {
                        radius: 4
                        color: refreshDevicesButton.pressed ? "#6961C9" : "#7F77DD"
                    }
                    contentItem: Text {
                        text: refreshDevicesButton.text
                        font: refreshDevicesButton.font
                        color: "white"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: blueZManager.refreshDevices()
                }
            }

            Label {
                visible: blueZManager.deviceModel.length === 0
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("Nessun dispositivo accoppiato. Accoppia una cassa/cuffia con bluetoothctl o le impostazioni di sistema, poi premi Aggiorna.")
                color: "#CFCFC9"
            }

            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(contentHeight, 260)
                clip: true
                model: blueZManager.deviceModel
                spacing: 4

                delegate: Rectangle {
                    width: ListView.view.width
                    height: deviceRow.implicitHeight + 12
                    color: modelData.connected ? "#DCEEDC" : "#F0F0EE"
                    border.color: "#C9C9C4"
                    radius: 4

                    RowLayout {
                        id: deviceRow
                        anchors.fill: parent
                        anchors.margins: 6
                        spacing: 8

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0
                            Label {
                                text: (modelData.isAudioSink ? "🔊 " : "") + modelData.name
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                                // Colore esplicito: queste righe hanno uno
                                // sfondo chiaro personalizzato
                                // (#DCEEDC/#F0F0EE), ma il colore ereditato
                                // di default risultava troppo chiaro per
                                // essere leggibile — segnalato dall'utente
                                // con uno screenshot.
                                color: "#2C2C2A"
                            }
                            Label {
                                text: modelData.address + (modelData.connected ? qsTr(" · connesso") : "")
                                font.pixelSize: 11
                                color: "#5C5A52"
                            }
                        }

                        Button {
                            id: disconnectDeviceButton
                            text: qsTr("Disconnetti")
                            visible: modelData.connected
                            // Sfondo esplicito, non solo il testo — vedi
                            // spiegazione sul pulsante "Aggiorna" qui sopra:
                            // segnalato dall'utente proprio come bianco su
                            // bianco in questo dialog.
                            background: Rectangle {
                                radius: 4
                                color: disconnectDeviceButton.pressed ? "#6961C9" : "#7F77DD"
                            }
                            contentItem: Text {
                                text: disconnectDeviceButton.text
                                font: disconnectDeviceButton.font
                                color: "white"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: blueZManager.disconnectDevice(modelData.objectPath)
                        }
                        Button {
                            id: connectDeviceButton
                            text: qsTr("Connetti")
                            visible: !modelData.connected
                            background: Rectangle {
                                radius: 4
                                color: connectDeviceButton.pressed ? "#6961C9" : "#7F77DD"
                            }
                            contentItem: Text {
                                text: connectDeviceButton.text
                                font: connectDeviceButton.font
                                color: "white"
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            onClicked: patchManager.addBluetoothOutput(modelData.objectPath)
                        }
                    }
                }
            }
        }
    }

    // Parametri del ping keepalive, regolabili senza ricompilare —
    // richiesto esplicitamente dall'utente per poter sperimentare quanto
    // breve/silenzioso può essere restando comunque efficace, incluso
    // provare una frequenza ultrasonica (inaudibile all'orecchio umano)
    // invece di un rumore udibile a basso volume. Persistiti da
    // PatchManager tra un avvio e l'altro.
    Dialog {
        id: keepAlivePingDialog
        title: qsTr("Ping keepalive")
        modal: true
        standardButtons: Dialog.Close
        anchors.centerIn: parent

        ColumnLayout {
            width: 340
            spacing: 10

            Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: "#CFCFC9"
                text: qsTr("Un tono brevissimo inviato periodicamente alle casse Bluetooth per evitare che vadano in stand-by per inattività. Più è ultrasonico/breve/silenzioso, meno si sente — ma se una cassa si spegne comunque, prova ad aumentare ampiezza o durata, o a ridurre l'intervallo.")
            }

            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8

                Label { text: qsTr("Frequenza (Hz)"); color: "white" }
                SpinBox {
                    id: pingFrequencyField
                    from: 20
                    to: 24000
                    stepSize: 500
                    editable: true
                    value: patchManager.keepAlivePingFrequencyHz
                    onValueModified: patchManager.keepAlivePingFrequencyHz = value
                    contentItem: TextInput {
                        text: pingFrequencyField.textFromValue(pingFrequencyField.value, pingFrequencyField.locale)
                        font: pingFrequencyField.font
                        color: "white"
                        selectionColor: "#7F77DD"
                        selectedTextColor: "white"
                        horizontalAlignment: Qt.AlignHCenter
                        verticalAlignment: Qt.AlignVCenter
                        readOnly: !pingFrequencyField.editable
                        validator: pingFrequencyField.validator
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                    }
                }

                Label {
                    Layout.columnSpan: 2
                    font.pixelSize: 10
                    font.italic: true
                    color: "#9A9A95"
                    text: qsTr("Sopra ~17-18kHz è tipicamente inaudibile agli adulti (ma non a bambini/animali) — oltre ~22-24kHz rischia di essere tagliato dal codec Bluetooth stesso.")
                }

                Label { text: qsTr("Ampiezza (‰₀₀, 0-100000)"); color: "white" }
                SpinBox {
                    id: pingAmplitudeField
                    from: 0
                    to: 100000
                    stepSize: 50
                    editable: true
                    value: patchManager.keepAlivePingAmplitudeUnits
                    onValueModified: patchManager.keepAlivePingAmplitudeUnits = value
                    contentItem: TextInput {
                        text: pingAmplitudeField.textFromValue(pingAmplitudeField.value, pingAmplitudeField.locale)
                        font: pingAmplitudeField.font
                        color: "white"
                        selectionColor: "#7F77DD"
                        selectedTextColor: "white"
                        horizontalAlignment: Qt.AlignHCenter
                        verticalAlignment: Qt.AlignVCenter
                        readOnly: !pingAmplitudeField.editable
                        validator: pingAmplitudeField.validator
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                    }
                }

                Label {
                    Layout.columnSpan: 2
                    font.pixelSize: 10
                    font.italic: true
                    color: "#9A9A95"
                    text: qsTr("≈ %1% del fondo scala").arg((patchManager.keepAlivePingAmplitudeUnits / 1000).toFixed(3))
                }

                Label { text: qsTr("Durata (ms)"); color: "white" }
                SpinBox {
                    id: pingDurationField
                    from: 0
                    to: 1000
                    stepSize: 10
                    editable: true
                    value: patchManager.keepAlivePingDurationMs
                    onValueModified: patchManager.keepAlivePingDurationMs = value
                    contentItem: TextInput {
                        text: pingDurationField.textFromValue(pingDurationField.value, pingDurationField.locale)
                        font: pingDurationField.font
                        color: "white"
                        selectionColor: "#7F77DD"
                        selectedTextColor: "white"
                        horizontalAlignment: Qt.AlignHCenter
                        verticalAlignment: Qt.AlignVCenter
                        readOnly: !pingDurationField.editable
                        validator: pingDurationField.validator
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                    }
                }

                Label { text: qsTr("Intervallo (s)"); color: "white" }
                SpinBox {
                    id: pingPeriodField
                    from: 1
                    to: 120
                    editable: true
                    value: patchManager.keepAlivePingPeriodSeconds
                    onValueModified: patchManager.keepAlivePingPeriodSeconds = value
                    contentItem: TextInput {
                        text: pingPeriodField.textFromValue(pingPeriodField.value, pingPeriodField.locale)
                        font: pingPeriodField.font
                        color: "white"
                        selectionColor: "#7F77DD"
                        selectedTextColor: "white"
                        horizontalAlignment: Qt.AlignHCenter
                        verticalAlignment: Qt.AlignVCenter
                        readOnly: !pingPeriodField.editable
                        validator: pingPeriodField.validator
                        inputMethodHints: Qt.ImhFormattedNumbersOnly
                    }
                }
            }
        }
    }

    // Modal di configurazione stile QLab per una singola traccia della
    // playlist (tasto destro sulla riga, vedi PortRow::configureRequested):
    // pre wait, durata, post wait. I tre campi sono in secondi interi;
    // 0 in ognuno = comportamento storico (parte subito, dura finché i loop
    // non finiscono o viene fermata a mano, si scollega subito alla fine).
    Dialog {
        id: cueConfigDialog
        property int cueIndex: -1

        title: qsTr("Configura traccia")
        modal: true
        standardButtons: Dialog.Ok | Dialog.Cancel
        anchors.centerIn: parent

        function openFor(index) {
            cueConfigDialog.cueIndex = index
            var list = patchManager.cueModel
            if (index >= 0 && index < list.length) {
                preWaitField.seconds = list[index].preWaitSeconds || 0
                durationField.seconds = list[index].durationSeconds || 0
                postWaitField.seconds = list[index].postWaitSeconds || 0
            }
            cueConfigDialog.open()
        }

        onAccepted: {
            if (cueConfigDialog.cueIndex < 0)
                return
            // Un campo può avere il focus (quindi il Binding interno
            // sospeso, vedi DurationField.qml) proprio nel momento in cui si
            // preme OK: senza un commit esplicito, l'ultima cifra digitata
            // ma non ancora confermata (niente editingFinished) andrebbe
            // persa.
            preWaitField.commit()
            durationField.commit()
            postWaitField.commit()
            patchManager.setCuePreWait(cueConfigDialog.cueIndex, preWaitField.seconds)
            patchManager.setCueDuration(cueConfigDialog.cueIndex, durationField.seconds)
            patchManager.setCuePostWait(cueConfigDialog.cueIndex, postWaitField.seconds)
        }

        ColumnLayout {
            spacing: 10

            Label {
                Layout.fillWidth: true
                text: (cueConfigDialog.cueIndex >= 0 && cueConfigDialog.cueIndex < patchManager.cueModel.length)
                      ? patchManager.cueModel[cueConfigDialog.cueIndex].displayName : ""
                font.bold: true
                elide: Text.ElideRight
            }

            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8

                // Timecode HH:MM:SS:CC invece di un singolo numero di
                // secondi, richiesto esplicitamente dall'utente per poter
                // scrivere durate precise senza calcolare i secondi totali
                // a mano.
                Label { text: qsTr("Pre wait") }
                DurationField { id: preWaitField }

                Label { text: qsTr("Durata (00:00:00:00 = nessun limite)") }
                DurationField { id: durationField }

                Label { text: qsTr("Post wait") }
                DurationField { id: postWaitField }
            }

            Label {
                Layout.preferredWidth: 320
                wrapMode: Text.WordWrap
                font.pixelSize: 11
                color: "#888780"
                text: qsTr("Pre wait: attesa prima che l'audio parta davvero dopo Play. Durata: la traccia si ferma da sola dopo tot secondi (0 = usa i loop o la lunghezza naturale del file). Post wait: quanto resta collegata (in silenzio) dopo la fine naturale prima di scollegarsi del tutto.")
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 8

        Label {
            id: errorLabel
            visible: false
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            color: "#c0392b"
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            // Trasporto globale: slegato dal click/trascinamento sulle righe
            // della playlist, che ora serve solo a definire il routing. Play
            // riprende tutte le tracce in pausa, altrimenti avvia quella in
            // armo SENZA fermare le altre già in riproduzione (lo stesso fa
            // la barra spaziatrice) — più tracce possono suonare insieme.
            // Pausa mette in pausa tutte le tracce in riproduzione (nodo e
            // collegamenti restano vivi, silenzio invece di distruzione).
            // Stop ferma tutto (anche con Esc).
            Button {
                text: qsTr("▶ Play")
                enabled: root.anyCueResumable() || patchManager.armedCueIndex >= 0
                onClicked: patchManager.play()
            }

            Button {
                text: qsTr("⏸ Pausa")
                enabled: root.anyCuePausable()
                onClicked: patchManager.pause()
            }

            Button {
                text: qsTr("⏹ Stop (Esc)")
                enabled: root.anyCuePlaying()
                onClicked: patchManager.stopAllCues()
            }

            Item { Layout.fillWidth: true }

            Button {
                text: qsTr("🎛 Trasforma")
                checkable: true
                checked: root.showTransformPanel
                onToggled: root.showTransformPanel = checked
            }

            // Con playlist lunghe i cavi di ogni traccia si accavallano:
            // questi due radio button permettono di vedere solo il routing
            // della traccia "in armo" (quella che si sta editando) invece
            // di tutte insieme.
            RadioButton {
                text: qsTr("Tutte le patch")
                checked: root.showAllPatches
                onCheckedChanged: if (checked) root.showAllPatches = true
            }

            RadioButton {
                text: qsTr("Solo selezionata")
                checked: !root.showAllPatches
                onCheckedChanged: if (checked) root.showAllPatches = false
            }

            Button {
                text: qsTr("Scollega tutto")
                onClicked: confirmDialog.ask(
                    qsTr("Scollegare tutti gli output? La riproduzione in corso non si ferma, solo il routing."),
                    function() { patchManager.clearAllConnections() })
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            CueList {
                id: inputColumn
                Layout.fillWidth: true
                Layout.fillHeight: true
                cueModel: patchManager.cueModel
                armedIndex: patchManager.armedCueIndex
                dragSourceNodeId: root.dragSourceNodeId
                mapTarget: root.contentItem
                showAllPatches: root.showAllPatches

                onAddRequested: fileDialog.open()
                onAddAppStreamRequested: appStreamPickerDialog.open()
                onRemoveRequested: (index) => confirmDialog.ask(
                    qsTr("Rimuovere questa traccia dalla playlist?"),
                    function() { patchManager.removeCue(index) })
                onConfigureRequested: (index) => cueConfigDialog.openFor(index)
                onRenameRequested: (index, currentName) => renameDialog.openForCue(index, currentName)
                onAnchorChanged: (nodeId, rowIndex, isInput, cx, cy, rx, ry, rw, rh) =>
                    root.registerAnchor(nodeId, rowIndex, isInput, cx, cy, rx, ry, rw, rh)
                onDragStarted: (rowIndex, nodeId, gx, gy) => root.beginDrag(rowIndex, nodeId, gx, gy)
                onDragMoved: (gx, gy) => root.updateDrag(gx, gy)
                onDragEnded: (gx, gy) => root.endDrag(gx, gy)
            }

            PatchGrid {
                visible: !root.showTransformPanel
                Layout.fillHeight: true
                linkingActive: root.dragging
            }

            TransformPanel {
                id: transformPanel
                // Stretto apposta (non 300 come prima): lasciato più largo
                // sarebbe stato attraversato dai cavi che vanno dalla
                // Playlist all'Output. I cavi restano comunque disegnati a
                // piena finestra (Canvas sopra tutto): quando questo
                // pannello è visibile, cableCanvas ritaglia il disegno di
                // ogni cavo per non attraversarlo (vedi
                // drawCableAroundTransformPanel), quindi il cavo si vede
                // "entrare" a sinistra del pannello e "uscire" a destra,
                // mai sopra i controlli.
                visible: root.showTransformPanel
                Layout.preferredWidth: 210
                Layout.fillHeight: true
                cueModel: patchManager.cueModel
            }

            PortColumn {
                id: outputColumn
                Layout.fillWidth: true
                Layout.fillHeight: true
                title: qsTr("Output")
                portModel: patchManager.outputs
                // Non "+" (i sink non si "aggiungono" a mano: PipeWire li
                // scopre e li mostra da solo appena una cassa Bluetooth è
                // connessa a livello di sistema). Il simbolo di refresh apre
                // lo stesso dialog, che ora serve solo a forzare un nuovo
                // giro di BlueZManager::refreshDevices — utile per vedere
                // subito una cassa appena accoppiata via bluetoothctl/
                // impostazioni di sistema senza dover riavviare l'app.
                // Stesso quadrato 32x32 del "+" della Playlist (vedi
                // PortColumn.qml), non un pulsante largo quanto il testo.
                addButtonText: "↻"
                isInputColumn: false
                dragging: root.dragging
                dragCurrentPoint: root.dragCurrentPoint
                hitTestOutputFn: root.hitTestOutput
                mapTarget: root.contentItem

                onAddRequested: bluetoothDialog.open()
                onPortRemoveRequested: (nodeId) => confirmDialog.ask(
                    qsTr("Rimuovere questo output dalla colonna?"),
                    function() { patchManager.removeOutput(nodeId) })
                onIdentifyRequested: (nodeId) => patchManager.identifySink(nodeId)
                onRenameRequested: (nodeId, currentName) => renameDialog.openForOutput(nodeId, currentName)
                onDelayRequested: (nodeId, currentDelayMs) => delayDialog.openForOutput(nodeId, currentDelayMs)
                onAnchorChanged: (nodeId, rowIndex, isInput, cx, cy, rx, ry, rw, rh) =>
                    root.registerAnchor(nodeId, rowIndex, isInput, cx, cy, rx, ry, rw, rh)
            }
        }

        // Barra di suggerimenti fissa in fondo, SEMPRE presente (mai
        // nascosta/mostrata condizionalmente): prima questo testo viveva
        // dentro CueList, visibile solo con playlist non vuota — comparire
        // dopo il caricamento di un progetto spostava la ListView sotto di
        // sé, ma le righe già posizionate non aggiornavano il loro punto di
        // aggancio globale (cambia solo la posizione di un antenato, non la
        // loro stessa y relativa), quindi il primo cavo restava disegnato
        // più in alto di dove appariva davvero la riga. Tenerla sempre
        // presente evita che l'altezza della colonna cambi quando la
        // playlist si popola.
        Label {
            Layout.fillWidth: true
            text: qsTr("Crea un cavo virtuale trascinando una traccia dalla playlist (tabella a sinistra) verso un output (tabella a destra). Click su un cavo per scollegarlo.")
            wrapMode: Text.WordWrap
            font.pixelSize: 11
            color: "#888780"
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // Overlay di disegno dei cavi: sovrapposto a tutta la finestra, senza
    // MouseArea propria (gli eventi passano attraverso verso le colonne
    // sottostanti). Le curve collegano il connettore di ogni input
    // collegato a quello di ogni output collegato (patchManager.connectionsModel,
    // aggiornato reattivamente), più un cavo "in volo" che segue il cursore
    // durante un trascinamento.
    Canvas {
        id: cableCanvas
        anchors.fill: parent
        z: 10
        renderTarget: Canvas.FramebufferObject

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        // Permette di cliccare direttamente su un cavo per rimuovere quella
        // connessione, senza dover ritrascinare dal connettore di origine.
        // Se il click non cade abbastanza vicino a nessun cavo, l'evento
        // viene esplicitamente rifiutato (mouse.accepted = false) in
        // onPressed, così passa agli item sottostanti (connettori, righe,
        // pulsanti "+") esattamente come se questa MouseArea non esistesse.
        MouseArea {
            anchors.fill: parent

            onPressed: (mouse) => {
                if (!root.findConnectionNear(mouse.x, mouse.y))
                    mouse.accepted = false
            }
            onClicked: (mouse) => {
                var hit = root.findConnectionNear(mouse.x, mouse.y)
                if (hit)
                    patchManager.toggleCueOutput(hit.cueIndex, hit.outputNodeId)
            }
        }

        function drawCable(ctx, x1, y1, x2, y2, color, lineWidth, dashed) {
            ctx.save()
            ctx.strokeStyle = color
            ctx.lineWidth = lineWidth
            ctx.lineCap = "round"
            if (dashed)
                ctx.setLineDash([6, 5])
            var midX = x1 + (x2 - x1) * 0.5
            ctx.beginPath()
            ctx.moveTo(x1, y1)
            ctx.bezierCurveTo(midX, y1, midX, y2, x2, y2)
            ctx.stroke()
            ctx.restore()
        }

        // Come drawCable, ma quando il pannello "Trasforma" è visibile
        // ritaglia il disegno per non farlo passare sopra i suoi controlli:
        // la STESSA curva viene disegnata due volte, una volta ritagliata
        // alla zona a sinistra del pannello e una a destra — il tratto
        // centrale (quello che l'attraverserebbe) semplicemente non viene
        // disegnato, dando l'impressione che il cavo "entri" a sinistra del
        // pannello ed "esca" a destra, invece di stare sopra ai controlli.
        function drawCableAroundTransformPanel(ctx, x1, y1, x2, y2, color, lineWidth, dashed) {
            if (!root.showTransformPanel || !transformPanel.width) {
                drawCable(ctx, x1, y1, x2, y2, color, lineWidth, dashed)
                return
            }

            var topLeft = transformPanel.mapToItem(root.contentItem, 0, 0)
            var bottomRight = transformPanel.mapToItem(root.contentItem, transformPanel.width, transformPanel.height)

            ctx.save()
            ctx.beginPath()
            ctx.rect(0, 0, topLeft.x, height)
            ctx.clip()
            drawCable(ctx, x1, y1, x2, y2, color, lineWidth, dashed)
            ctx.restore()

            ctx.save()
            ctx.beginPath()
            ctx.rect(bottomRight.x, 0, width - bottomRight.x, height)
            ctx.clip()
            drawCable(ctx, x1, y1, x2, y2, color, lineWidth, dashed)
            ctx.restore()
        }

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()

            // Disegna il routing "voluto" di OGNI traccia (PatchManager::
            // cueModel / desiredOutputNodeIds), non solo quello della
            // traccia effettivamente in riproduzione: il cavo deve restare
            // visibile anche per una traccia solo armata/selezionata, non
            // ancora in play.
            var cues = patchManager.cueModel
            for (var i = 0; i < cues.length; i++) {
                // Con showAllPatches a false, si vede solo il routing della
                // traccia "in armo" (quella che si sta editando) — tutte le
                // altre restano nascoste per non affollare la vista con
                // playlist lunghe.
                if (!root.showAllPatches && i !== patchManager.armedCueIndex)
                    continue
                var start = root.inputAnchorsByIndex[i]
                if (!start)
                    continue
                var outputs = cues[i].desiredOutputNodeIds
                var cableColor = root.colorForCue(i)
                for (var j = 0; j < outputs.length; j++) {
                    var end = root.outputAnchors[outputs[j]]
                    if (end)
                        drawCableAroundTransformPanel(ctx, start.x, start.y, end.x, end.y, cableColor, 3, false)
                }
            }

            if (root.dragging) {
                // Usa il punto di partenza catturato al press (dragStartPoint),
                // non un lookup in inputAnchors per nodeId: una traccia non
                // ancora in riproduzione ha nodeId 0, valore che più righe
                // diverse condividerebbero contemporaneamente in quella mappa.
                drawCableAroundTransformPanel(ctx, root.dragStartPoint.x, root.dragStartPoint.y,
                          root.dragCurrentPoint.x, root.dragCurrentPoint.y,
                          root.colorForCue(root.dragCueIndex), 3, true)
            }
        }
    }

    // In un secondo passaggio, l'uscita jack va popolata automaticamente da
    // PipeWireEngine (sink hardware di default) invece che manualmente qui,
    // non appena il discovery iniziale dei nodi è completato.
    Component.onCompleted: {
        // patchManager.addJackOutput(defaultSinkId)
    }
}
