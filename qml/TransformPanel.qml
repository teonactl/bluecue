import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Pannello opzionale "Trasforma" (colonna centrale, mostrata solo a
// richiesta): permette di modificare come ogni traccia della playlist viene
// riprodotta PRIMA di arrivare ai sink, senza toccare il routing (cavi) che
// resta gestito dalle colonne Playlist/Output. Un pannello, non una terza
// colonna di trascinamento: ogni riga corrisponde 1:1 a una riga della
// playlist (stesso indice), letta dallo stesso cueModel.
ColumnLayout {
    id: panel
    property var cueModel: null

    spacing: 8

    // Il fix precedente (vedi PROJECT_STATUS punto 27) copriva solo il testo
    // dei Control nello stato normale (palette.windowText/text/buttonText su
    // ApplicationWindow); lo stato "disabled" (es. il campo "volte" quando
    // Loop infinito è spuntato) non era coperto e ricadeva sul grigio chiaro
    // di default dello stile piattaforma — leggibilità comunque scarsa.
    // Impostare qui il gruppo "disabled" della palette si propaga a tutti i
    // Control figli, sovrascrivibile localmente dove serve.
    palette.disabled.windowText: "#6B6A62"
    palette.disabled.text: "#6B6A62"
    palette.disabled.buttonText: "#6B6A62"

    Label {
        text: qsTr("Trasforma")
        font.pixelSize: 16
        font.bold: true
        color: "#2C2C2A"
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Per ogni traccia: quante volte ripeterla, se suonarla al contrario, e se farla girare tra gli output collegati invece che su tutti insieme. I cavi sono nascosti qui: il routing di ogni traccia è riassunto sotto, nella riga corrispondente.")
        wrapMode: Text.WordWrap
        font.pixelSize: 11
        color: "#5C5A52"
    }

    ListView {
        Layout.fillWidth: true
        Layout.fillHeight: true
        spacing: 6
        clip: true
        model: panel.cueModel

        delegate: Rectangle {
            id: row
            width: ListView.view.width
            height: content.implicitHeight + 16
            radius: 6
            color: "#F1EFE8"
            border.width: 1
            border.color: "#D3D1C7"

            // panel.cueModel è un QVariantList (JS array), non un
            // QAbstractListModel: si indicizza direttamente con "index",
            // sempre disponibile nei delegate — stesso pattern di
            // CueList.qml (i ruoli automatici model.xxx non sono affidabili
            // su una lista di questo tipo).
            readonly property var cue: (panel.cueModel && index >= 0 && index < panel.cueModel.length)
                                        ? panel.cueModel[index] : null
            // "index" qui è quello della ListView esterna (la traccia); nel
            // Repeater delle "pillole" più in basso lo stesso nome "index"
            // viene rioscurato dall'indice interno (l'output all'interno
            // della traccia) — serve una copia con un nome diverso.
            readonly property int trackIndex: index
            readonly property int outputCount: cue ? cue.desiredOutputNodeIds.length : 0

            ColumnLayout {
                id: content
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.margins: 8
                spacing: 4

                Text {
                    Layout.fillWidth: true
                    text: row.cue ? row.cue.displayName : ""
                    color: "#2C2C2A"
                    font.bold: true
                    elide: Text.ElideRight
                }

                RowLayout {
                    spacing: 10

                    CheckBox {
                        id: infiniteCheck
                        text: qsTr("Loop infinito")
                        checked: row.cue ? row.cue.loopCount < 0 : true
                        // palette.windowText da solo non basta: lo stile
                        // nativo di questa piattaforma ignora l'override di
                        // palette su CheckBox/SpinBox (a differenza di
                        // Label/Text/TextField, che espongono "color"
                        // direttamente e infatti funzionano) — segnalato
                        // dall'utente come testo del pannello Trasforma
                        // ancora invisibile anche dopo il fix sulla
                        // palette.disabled. Un contentItem esplicito, stesso
                        // principio già usato per i pulsanti "+"/"↻", è
                        // l'unico modo affidabile di garantire il colore.
                        contentItem: Text {
                            text: infiniteCheck.text
                            font: infiniteCheck.font
                            color: infiniteCheck.enabled ? "#2C2C2A" : "#6B6A62"
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: infiniteCheck.indicator.width + infiniteCheck.spacing
                        }
                        onToggled: patchManager.setCueLoopCount(index, checked ? -1 : Math.max(1, loopSpin.value))
                    }

                    SpinBox {
                        id: loopSpin
                        from: 1
                        to: 999
                        editable: true
                        enabled: !infiniteCheck.checked
                        contentItem: TextInput {
                            text: loopSpin.textFromValue(loopSpin.value, loopSpin.locale)
                            font: loopSpin.font
                            // Bianco, non scuro: il campo numerico interno
                            // di un SpinBox ha uno sfondo nativo scuro
                            // (diverso dalla card chiara dietro di lui),
                            // testo scuro qui era scuro-su-scuro e restava
                            // illeggibile.
                            color: loopSpin.enabled ? "white" : "#9A9A95"
                            selectionColor: "#7F77DD"
                            selectedTextColor: "white"
                            horizontalAlignment: Qt.AlignHCenter
                            verticalAlignment: Qt.AlignVCenter
                            readOnly: !loopSpin.editable
                            validator: loopSpin.validator
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                        }
                        // Non un semplice "value: row.cue.loopCount": ogni
                        // valueModified riscrive subito il backend, che
                        // riemette cuesChanged, che ricalcola row.cue — con
                        // un binding diretto questo riassegnava value() ad
                        // OGNI tasto premuto durante la digitazione,
                        // interrompendo l'editing in corso ("non sempre
                        // editabile" segnalato dall'utente: bastava scrivere
                        // un numero a più cifre per vederlo tornare
                        // all'ultima cifra confermata). Il Binding si
                        // applica solo quando il campo non ha il focus, così
                        // la sincronizzazione dal backend non compete con la
                        // digitazione in corso — stesso principio del
                        // pattern "assegna una volta sola" già usato per i
                        // campi pre/durata/post wait in cueConfigDialog
                        // (Main.qml), qui adattato a un delegate sempre
                        // vivo invece che a un dialog aperto on-demand.
                        Binding {
                            target: loopSpin
                            property: "value"
                            value: (row.cue && row.cue.loopCount > 0) ? row.cue.loopCount : 1
                            when: !loopSpin.activeFocus
                        }
                        onValueModified: patchManager.setCueLoopCount(index, value)
                    }

                    Label {
                        text: qsTr("volte")
                        enabled: !infiniteCheck.checked
                        color: "#2C2C2A"
                    }
                }

                RowLayout {
                    spacing: 10

                    CheckBox {
                        id: reverseCheck
                        text: qsTr("Riproduci al contrario")
                        checked: row.cue ? row.cue.reverse : false
                        contentItem: Text {
                            text: reverseCheck.text
                            font: reverseCheck.font
                            color: "#2C2C2A"
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: reverseCheck.indicator.width + reverseCheck.spacing
                        }
                        onToggled: patchManager.setCueReverse(index, checked)
                    }
                }

                RowLayout {
                    spacing: 10

                    CheckBox {
                        id: rotateCheck
                        // La rotazione avanza a comando (barra spaziatrice),
                        // non più automaticamente ad ogni giro del file —
                        // vedi le etichette nella Playlist e
                        // PatchManager::advanceCue.
                        text: qsTr("Un output alla volta (avanza con la barra spaziatrice)")
                        enabled: row.outputCount > 1
                        checked: row.cue ? row.cue.rotateOutputs : false
                        contentItem: Text {
                            text: rotateCheck.text
                            font: rotateCheck.font
                            color: rotateCheck.enabled ? "#2C2C2A" : "#6B6A62"
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: rotateCheck.indicator.width + rotateCheck.spacing
                        }
                        onToggled: patchManager.setCueRotateOutputs(index, checked)
                    }
                }

                RowLayout {
                    spacing: 10
                    visible: rotateCheck.checked && row.outputCount > 1

                    CheckBox {
                        id: infiniteCyclesCheck
                        // Comportamento storico (nessun limite): la
                        // rotazione avanza solo a comando finché l'utente
                        // non ferma la traccia a mano. Richiesto
                        // esplicitamente dall'utente un modo di impostare
                        // quanti giri completi fare prima di passare da
                        // sola alla prossima traccia in coda.
                        text: qsTr("Cicli infiniti")
                        checked: row.cue ? row.cue.rotationCycleCount < 0 : true
                        contentItem: Text {
                            text: infiniteCyclesCheck.text
                            font: infiniteCyclesCheck.font
                            color: "#2C2C2A"
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: infiniteCyclesCheck.indicator.width + infiniteCyclesCheck.spacing
                        }
                        onToggled: patchManager.setCueRotationCycleCount(index, checked ? -1 : Math.max(1, cycleSpin.value))
                    }

                    SpinBox {
                        id: cycleSpin
                        from: 1
                        to: 999
                        editable: true
                        enabled: !infiniteCyclesCheck.checked
                        contentItem: TextInput {
                            text: cycleSpin.textFromValue(cycleSpin.value, cycleSpin.locale)
                            font: cycleSpin.font
                            color: cycleSpin.enabled ? "white" : "#9A9A95"
                            selectionColor: "#7F77DD"
                            selectedTextColor: "white"
                            horizontalAlignment: Qt.AlignHCenter
                            verticalAlignment: Qt.AlignVCenter
                            readOnly: !cycleSpin.editable
                            validator: cycleSpin.validator
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                        }
                        // Stesso motivo del Binding su loopSpin qui sopra:
                        // un binding diretto a row.cue.rotationCycleCount
                        // interromperebbe la digitazione di un numero a più
                        // cifre ad ogni tasto premuto.
                        Binding {
                            target: cycleSpin
                            property: "value"
                            value: (row.cue && row.cue.rotationCycleCount > 0) ? row.cue.rotationCycleCount : 1
                            when: !cycleSpin.activeFocus
                        }
                        onValueModified: patchManager.setCueRotationCycleCount(index, value)
                    }

                    Label {
                        text: qsTr("giri, poi passa alla prossima traccia")
                        enabled: !infiniteCyclesCheck.checked
                        color: "#2C2C2A"
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: row.outputCount <= 1
                    text: qsTr("Collega la traccia ad almeno due output per poter ruotare.")
                    wrapMode: Text.WordWrap
                    font.pixelSize: 10
                    font.italic: true
                    color: "#5C5A52"
                }

                // Riepilogo del routing di questa traccia, al posto dei
                // cavi (nascosti mentre questo pannello è aperto, vedi
                // Main.qml/cableCanvas): un'etichetta per ogni output
                // collegato, con quella davvero attiva evidenziata quando
                // la traccia è in rotazione.
                RowLayout {
                    Layout.fillWidth: true
                    visible: row.outputCount > 0
                    spacing: 4

                    Label {
                        text: qsTr("Collegata a:")
                        font.pixelSize: 10
                        color: "#5C5A52"
                    }

                    Repeater {
                        model: row.cue ? row.cue.desiredOutputLabels : []

                        delegate: Rectangle {
                            // "Connesso adesso" richiede SEMPRE che l'output
                            // sia al momento risolto (nodeId diverso da 0,
                            // cioè il dispositivo è davvero scoperto/
                            // connesso ora) — prima "!row.cue.rotateOutputs"
                            // da solo bastava a colorare la pillola come
                            // "attiva" per QUALUNQUE traccia non in
                            // rotazione, anche con un output desiderato ma
                            // al momento disconnesso, mostrando un colore
                            // "collegato" fuorviante (segnalato
                            // dall'utente: "questo non è collegato eppure
                            // vedo questo").
                            readonly property bool resolvedNow: row.cue && row.cue.desiredOutputNodeIds[index] !== 0
                            readonly property bool active: resolvedNow
                                && (!row.cue.rotateOutputs || row.cue.desiredOutputNodeIds[index] === row.cue.activeOutputNodeId)

                            radius: 8
                            color: active ? "#7F77DD" : "#E4E1D6"
                            implicitHeight: connLabel.implicitHeight + 4
                            implicitWidth: connLabel.implicitWidth + 12

                            Text {
                                id: connLabel
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 10
                                color: active ? "white" : "#5A584F"
                            }

                            // Click per rimuovere questo output dal routing
                            // voluto della traccia, anche se non al momento
                            // connesso (nessun cavo disegnato/cliccabile in
                            // quel caso) — stesso motivo di CueList.qml.
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    if (row.cue && index < row.cue.desiredOutputNames.length)
                                        patchManager.removeCueDesiredOutputByName(
                                            row.trackIndex, row.cue.desiredOutputNames[index])
                                }
                            }
                        }
                    }

                    Item { Layout.fillWidth: true }
                }
            }
        }
    }
}
