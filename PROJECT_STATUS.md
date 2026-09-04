# BT Multizone Audio — stato del progetto

App C++/Qt QML per instradare audio (file, microfono) verso più dispositivi
di output (jack del computer, dispositivi Bluetooth) con un'interfaccia a
patch bay, su Linux (Arch Linux, target anche Raspberry Pi).

## Stack tecnico

- **GUI**: Qt6/QML (QtQuick, QtQuick.Controls, QtQuick.Layouts, QtQuick.Dialogs)
- **Audio**: libpipewire nativa (non subprocess/CLI verso pw-cli)
- **Decodifica file**: libsndfile (WAV/FLAC/OGG; no MP3 — libsndfile non lo supporta, vedi TODO)
- **Bluetooth**: BlueZ via Qt D-Bus (QDBusInterface, system bus)
- **Build**: CMake + pkg-config (libpipewire-0.3, sndfile)

## Architettura

```
src/
├── audio/
│   ├── AudioNode.h          — struct dati nodo PipeWire (id, nome, kind, bluetooth)
│   ├── PipeWireEngine.*     — wrapper libpipewire: discovery nodi+porte, link reali
│   └── PatchManager.*       — orchestratore: smista nodi in input/output, routing
├── bluetooth/
│   └── BlueZManager.*       — D-Bus verso org.bluez (scan, connect, disconnect)
├── models/
│   └── PortModel.*          — QAbstractListModel per colonna Input o Output
└── main.cpp                 — entry point, espone patchManager/blueZManager a QML

qml/
├── Main.qml                 — finestra: CueList | PatchGrid | colonna Output, overlay Canvas dei cavi
├── CueList.qml               — colonna Input come playlist stile QLab/Cuelab (vedi sotto)
├── PortColumn.qml           — colonna riusabile con header, tasto "+", ListView (solo Output ora)
├── PortRow.qml              — riga singola (nome, connettore trascinabile, tasto rimuovi)
├── PatchGrid.qml            — zona centrale, feedback visivo "trascinamento cavo in corso"
└── DeviceSelector.qml       — placeholder ComboBox (non ancora usato in Main.qml)
```

## Decisioni chiave prese in questa chat

- **Non multi-room sincronizzato**: audio diversi per canale/zona, non lo
  stesso stream ovunque.
- **UI patch bay**, non "zone": colonna Input a sinistra (file audio,
  microfono, aggiungibili con tasto +), zona di routing/patching al centro,
  colonna Output a destra (jack computer + N dispositivi Bluetooth,
  aggiungibili con tasto +). Questo ha sostituito un primo design a
  "zone" (ZoneManager/ZoneModel), rimosso.
- **Routing a trascinamento (2026-08-30, sostituisce il click)**: si
  trascina dal connettore di una riga Input (o della traccia in
  riproduzione nella playlist) verso una riga Output; al rilascio si crea
  il collegamento (`PatchManager::toggleConnection`). Trascinare di nuovo
  verso un output già collegato lo rimuove (stesso `toggleConnection`,
  simmetrico). I cavi attivi sono disegnati come curve su un `Canvas`
  overlay a tutta finestra (vedi "Cavi disegnati" sotto).
- **Threading PipeWire**: pw_thread_loop dedicato; ogni emit verso Qt passa
  da `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` perché le
  callback PipeWire arrivano su un thread diverso da quello Qt/QML.
- **Playback file in loop continuo, non "play once"**: alla fine del file
  `PipeWireEngine` torna automaticamente all'inizio (`sf_seek` a 0) invece
  di fermare lo stream. Scelto perché la patch bay è pensata per input
  persistenti (es. musica di sottofondo per una zona) che restano collegati
  finché l'utente non li rimuove esplicitamente col tasto rimuovi, non per
  una riproduzione one-shot.

## Cosa funziona adesso

1. **Discovery nodi reale**: `PipeWireEngine` si registra sul
   `pw_registry`, riceve callback per ogni nodo `Audio/Sink`/`Audio/Source`
   esistente e nuovo, li smista in `PatchManager` verso `inputs`/`outputs`
   in base al `Kind`. I sink Bluetooth vengono riconosciuti via
   `device.api == "bluez5"`.
2. **Routing reale**: `PipeWireEngine` traccia anche le **porte** di ogni
   nodo (`PW_TYPE_INTERFACE_Port`, via
   `PW_KEY_NODE_ID`/`PW_KEY_PORT_DIRECTION`). `linkNodes(outputNodeId,
   inputNodeId)` abbina le porte per indice di canale (assume ordine di
   scoperta = ordine canali, FL poi FR — non garantito dalla spec, punto
   debole noto, vedi TODO) e crea un link PipeWire reale per coppia con
   `pw_core_create_object(..., "link-factory", ...)`. Un collegamento
   stereo produce 2 pw_proxy raggruppati sotto un `linkId` locale unico
   (PipeWire non dà un id univoco per link stereo). Rollback automatico se
   un canale su due fallisce. **Robusto contro la race condition sulle
   porte (fix 2026-08-29)**: le porte di un nodo arrivano dal registry come
   eventi separati e asincroni rispetto al nodo stesso, quindi collegare
   subito dopo la comparsa di un nodo poteva fallire con "Porte non
   trovate". `linkNodes` ora ritenta fino a 5 volte con 40ms di attesa tra
   un tentativo e l'altro (rilasciando il `pw_thread_loop_lock` tra un
   tentativo e l'altro, altrimenti il thread PipeWire non può consegnare
   l'evento di registrazione della porta). Testato riproducendo
   esattamente la race (link tentato nello stesso istante della comparsa
   del nodo, JBL Go 3 come output): 4/4 esecuzioni riuscite, ciascuna
   risolta al primo retry (~40ms).
3. **UI**: colonne Input/Output popolate dai modelli reali, click per
   selezionare/collegare, tasto rimuovi per riga.
4. **Riproduzione file audio**: `PipeWireEngine::createFileStream` apre il
   file con libsndfile e crea un `pw_stream` in direzione OUTPUT con
   `media.class = Audio/Source` (niente `PW_STREAM_FLAG_AUTOCONNECT`: il
   routing resta manuale via `PatchManager::toggleConnection`, come per
   tutti gli altri nodi). Il nodo viene scoperto dal discovery esistente
   come un Source qualsiasi e appare in colonna Input via `nodeAdded`. La
   callback `process` legge da `sf_readf_float` direttamente nel buffer
   PipeWire (formato F32, canali/samplerate presi dal file) e riparte dello
   zero a fine file (loop continuo). `PatchManager::removeInput` chiama
   `PipeWireEngine::removeFileStream` per fermare e distruggere lo stream.
   Build verificata: compila senza errori (2026-08-29). **Testato con un
   file reale (2026-08-29)**: harness standalone che chiama
   `PipeWireEngine::createFileStream` direttamente (stesso codice di
   produzione, bypassando la UI QML) su un WAV reale
   (`eritnhut1992-burp-20581.wav`, stereo 16kHz). Il nodo
   `btmultizone.file.1` compare correttamente in `pw-dump` come
   `Audio/Source`; l'audio catturato dal nodo con `pw-record --target` ha
   RMS/picco praticamente identici alla decodifica di riferimento via
   ffmpeg (RMS 0.0220 vs 0.0217, picco 0.253 vs 0.257) — conferma che
   decodifica e streaming funzionano correttamente end-to-end, non solo la
   creazione del nodo. **Testato anche il routing verso un output
   Bluetooth reale (2026-08-29)**: `linkNodes(fileNodeId, jblSinkNodeId)`
   verso il JBL Go 3 connesso, link creato con successo e riprodotto per
   ~7s. **Testato lo scenario multizona (2026-08-29)**: lo stesso file
   collegato con `linkNodes` a TUTTI e 3 i sink disponibili
   contemporaneamente (audio interno + JBL Go 3 + JBL Xtreme 3) — tutti e
   3 i link creati con successo, un solo input verso più output
   simultanei, nessuna modifica necessaria a `PatchManager`/
   `PipeWireEngine` (il design a connessioni multiple per input era già
   corretto). **Testato il re-routing dinamico a runtime (2026-08-30)**:
   stream file continuo (loop) spostato ciclicamente tra i sink disponibili
   ogni 3s con `unlinkNodes` + `linkNodes` in sequenza, senza mai ricreare
   lo stream — 2 giri completi, tutti gli switch riusciti. Nel frattempo il
   JBL Xtreme 3 si era disconnesso (sospensione automatica per inattività,
   normale per diffusori BT a batteria): il test ha alternato sui 2 sink
   rimasti connessi, a conferma che il ciclo link/unlink regge anche
   quando la topologia disponibile cambia tra un test e l'altro. Non
   ancora testato: il comportamento del loop a fine file su una finestra
   di ascolto più lunga della durata del file.
5. **Keepalive Bluetooth (2026-08-30)**: proprio la disconnessione del JBL
   Xtreme 3 osservata durante il test precedente ha motivato
   `PipeWireEngine::setKeepAliveEnabled(sinkNodeId, enabled)`. Un
   generatore `pw_stream` unico e condiviso (`btmultizone.keepalive`,
   creato pigramente al primo utilizzo) emette silenzio continuo tranne un
   tono breve (150ms, 440Hz, ampiezza 0.02, circa -34dBFS, impercettibile)
   ogni 20s, sufficiente a evitare lo stand-by per inattività audio dei
   diffusori Bluetooth senza essere udibile. Non ha `PW_KEY_MEDIA_CLASS`
   impostato in creazione, quindi `onGlobal()` lo classifica
   `Kind::Unknown` e non emette mai `nodeAdded`: non compare come sorgente
   selezionabile in UI. Collegato/scollegato automaticamente da
   `PatchManager` a ogni sink Bluetooth (`node.isBluetooth`) che entra/esce
   dalla colonna Output (`nodeAdded`/`nodeRemoved`/`removeOutput`), su un
   link separato e indipendente dal routing utente in
   `PatchManager::m_connections` — attivare/disattivare il keepalive non
   tocca mai lo stato delle connessioni patch bay. **Testato (2026-08-30)**:
   harness standalone che chiama `setKeepAliveEnabled` direttamente (stesso
   codice di produzione) sul sink audio interno (nessun diffusore BT
   connesso al momento del test); 42s catturati dal nodo
   `btmultizone.keepalive` con `pw-record --target`. Analisi quantitativa
   (RMS/picco a bucket di 0.25s) conferma silenzio pressoché totale con due
   blip a t≈18.5s e t≈38.5s (periodo 20.0s), picco esattamente 0.02 e RMS
   di bucket 0.010954 — coerente al centesimo con il valore atteso per un
   burst sinusoidale di 150ms dentro una finestra di 250ms
   (0.02/√2·√(0.15/0.25)). Generatore e ciclo ping/silenzio verificati
   end-to-end.
6. **Selezione file dal menu Input funzionante end-to-end (2026-08-30)**:
   `PatchManager::addFileInput` accettava un `QString` ma
   `FileDialog.selectedFile` in QML restituisce un `QUrl`
   (`file:///path/...`); la conversione automatica QML→C++ produceva la
   stringa con lo schema `file://` incluso, quindi `QFileInfo(...).exists()`
   falliva sempre e il file non veniva mai aggiunto (fallimento silenzioso:
   `patchError` veniva emesso ma nulla lo ascoltava in QML). Corretto
   cambiando la firma in `addFileInput(const QUrl &fileUrl)` e convertendo
   con `fileUrl.toLocalFile()` prima di usare `QFileInfo`. Aggiunta anche una
   label di errore in `Main.qml` (collegata a `patchManager.patchError`,
   visibile 5s) così un eventuale fallimento futuro (es. formato non
   supportato) non passa più inosservato. **Testato (2026-08-30)**: harness
   standalone che chiama `PatchManager::addFileInput` con un `QUrl` costruito
   esattamente come farebbe `FileDialog.selectedFile`
   (`QUrl::fromLocalFile(...)`) sullo stesso WAV reale usato nei test
   precedenti — il nodo `btmultizone.file.1` compare correttamente via
   `nodeAdded`, nessun `patchError`. App avviata anche a schermo con
   `DISPLAY`/`WAYLAND_DISPLAY` reali per verificare che non crashi
   all'avvio.

7. **Cavi disegnati e routing a trascinamento (2026-08-30)**: sostituito il
   vecchio flusso "click su input, click su output" con un trascinamento
   stile patchbay fisica. Ogni `PortRow` ha un connettore (pallino) sul
   bordo interno — a destra sulle righe Input, a sinistra sulle righe
   Output — ed emette di continuo la propria posizione globale
   (`anchorChanged`, via `mapToItem(null, ...)`) così `Main.qml` mantiene
   due registri nodeId→punto (`inputAnchors`/`outputAnchors`) sempre
   aggiornati, robusti a resize/scroll/riciclo dei delegate. Il
   trascinamento parte solo dai connettori Input (`MouseArea` con
   `preventStealing: true`, che continua a ricevere gli eventi anche fuori
   dai bordi della riga una volta agganciato il mouse) e il rilascio è
   testato contro il **rettangolo intero** della riga Output (non solo il
   pallino), per un bersaglio più permissivo. Un `Canvas` overlay a tutta
   finestra (`z: 10`, nessuna `MouseArea` propria quindi gli eventi
   passano alle colonne sottostanti) disegna una curva di Bezier per ogni
   connessione attiva (lette da `patchManager.connectionsModel`, una nuova
   `Q_PROPERTY` reattiva — prima `connections()` non era affatto
   esposta a QML) più un cavo tratteggiato "in volo" che segue il cursore
   durante il trascinamento. Trascinare di nuovo verso un output già
   collegato lo scollega (stesso `toggleConnection`, che già alternava).
   Le righe Output mostrano un evidenziatore quando il cursore le
   sorvola durante un trascinamento attivo. Nessuna modifica al backend di
   routing: solo `connectionsModel` è nuovo, `toggleConnection`/`linkNodes`
   sono invariati. Testato con `cmake --build` (compilazione C++ e AOT QML
   pulite) e avvio reale dell'app (nessun warning QML a runtime); il
   trascinamento interattivo non è stato automatizzato con uno strumento di
   input (nessun tool di automazione UI disponibile in questo ambiente),
   quindi va riprovato a mano dall'utente.
8. **Playlist stile QLab/Cuelab al posto della colonna Input generica
   (2026-08-30)**: la colonna Input non mostra più ogni sorgente
   `Audio/Source` scoperta da PipeWire (incluso il microfono hardware) —
   ora è una playlist (`CueList.qml` + `PatchManager` cue API) che
   contiene solo le tracce aggiunte esplicitamente dall'utente col tasto
   "+". Ogni cue è `{filePath, displayName, nodeId}`: `nodeId` resta 0
   finché non è il suo turno, perché non esiste ancora uno stream
   PipeWire per le tracce in coda. La barra spaziatrice
   (`Shortcut { sequence: "Space" }` in `Main.qml`) chiama
   `PatchManager::advanceCue()`: ferma la traccia in riproduzione (se
   c'è, scollegandola da qualunque output — nessun link orfano), avvia
   quella "in armo" (`armedCueIndex`, evidenziata con ▷ nella lista) e
   arma la successiva. La traccia in riproduzione (`playingCueIndex`,
   evidenziata con ▶ e sfondo pieno) è l'unica ad avere un connettore
   trascinabile per instradarla verso uno o più output — le altre righe
   mostrano un connettore smorzato/non trascinabile (`nodeId <= 0`).
   `PatchManager::addFileInput`/`removeInput` (il vecchio meccanismo
   generico "aggiungi subito uno stream file") sono stati rimossi, sostituiti
   da `addCueFile`/`removeCue`/`advanceCue`. Il nodeId di ogni cue viene
   assegnato in modo asincrono confrontando `node.name` con il prefisso
   `"btmultizone.file."` (lo stesso usato da `createFileStream`) sul primo
   nodo Source che arriva dopo una chiamata a `advanceCue()` (tracciato con
   `m_pendingCueIndex`); dato che una nuova traccia parte sempre solo dopo
   aver fermato la precedente, il rischio di un'associazione errata esiste
   solo in caso di doppia pressione della barra spaziatrice entro la
   finestra di discovery asincrono (tipicamente <100ms) — rischio accettato,
   non gestito esplicitamente. **Testato (2026-08-30)** con un harness
   standalone che pilota `PatchManager` direttamente (stesso pattern riusato
   in tutta la sessione): sequenza `addCueFile` ×2 → `advanceCue` ×3 verifica
   `armedCueIndex`/`playingCueIndex`/`nodeId` a ogni passo (inclusa
   l'assegnazione asincrona del nodeId dopo ~600ms) — tutti i valori attesi
   confermati, inclusa la coda che si esaurisce correttamente
   (`armedCueIndex == -1`) dopo l'ultima traccia. Verificato anche che un
   collegamento creato manualmente verso la traccia in riproduzione
   (`toggleConnection`) venga rimosso automaticamente da `advanceCue()`
   quando quella traccia viene fermata (`connectionsModel` passa da 1 a 0
   voci) — nessun link orfano quando si avanza la playlist.
9. **Fix: le tracce in playlist comparivano come "undefined" e non si
   potevano collegare (2026-08-30)**: `CueList.qml` legava il `ListView`
   direttamente a `patchManager.cueModel` (un `QVariantList`, non un vero
   `QAbstractListModel`) e leggeva i campi nel delegate con `model.xxx`
   come se fosse un modello con ruoli — per un `QVariantList` questo non è
   affidabile e risultava in `undefined` (quindi anche `nodeId` non
   valido, che bloccava il trascinamento verso gli output). Corretto
   indicizzando l'array direttamente (`column.cueModel[index]`, con
   `index` sempre disponibile in un delegate indipendentemente dal tipo di
   modello), con guardia contro indici transitori fuori range durante
   rimozioni. **Verificato visivamente (2026-08-30)**: nome file corretto
   in lista, avanzamento con barra spaziatrice confermato (evidenziazione
   ▶/▷ e nodeId assegnato), un trascinamento verso un output ha prodotto un
   cavo disegnato correttamente. Un secondo tentativo di trascinamento ha
   prodotto 3 cavi invece di 1 — quasi certamente un artefatto dell'ambiente
   di test (processi/finestre residui da tentativi precedenti nello stesso
   turno, non riproducibile per costruzione dal codice: `endDrag()` può
   chiamare `toggleConnection` al massimo una volta per invocazione), ma
   non confermato con certezza — vedi TODO punto 6.
10. **Impostazione per disattivare il keepalive Bluetooth (2026-08-30)**:
    `PatchManager::keepAliveEnabled` (default `true`, persistito via
    `QSettings`), esposta come voce spuntabile nel menu "Impostazioni".
    Quando disattivata, disabilita il ping su tutti i sink Bluetooth
    attualmente tracciati (`m_bluetoothOutputNodeIds`) e smette di
    attivarlo automaticamente per i nuovi sink Bluetooth che compaiono;
    riattivandola, lo riabilita su tutti quelli tracciati. Costruita e
    avviata senza warning; il toggle stesso non testato interattivamente
    (vedi TODO punto 6).
11. **Salvataggio/caricamento progetti su file, con menu e recenti
    (2026-08-30)**: menu "File" con Nuovo/Apri/Salva/Apri recenti.
    Un progetto è un JSON con la sola playlist (percorsi file, in ordine —
    non lo stato di riproduzione né i collegamenti agli output, che
    dipendono da id PipeWire non stabili tra un avvio e l'altro), estensione
    `.btmzproj`. `PatchManager::saveProject/loadProject(QUrl)` (dal
    `FileDialog`) e `loadProjectFromPath(QString)` (dal menu "Apri
    recenti", che ha già un path locale e non passa da un `FileDialog`).
    L'elenco dei recenti (max 8, più recente in cima, deduplicato) è
    persistito via `QSettings` e esposto reattivamente
    (`recentProjects` + `recentProjectsChanged`), popolato nel menu con un
    `Instantiator`. Caricare un progetto ferma la riproduzione corrente e
    salta con un `patchError` (senza bloccare il resto) i file non più
    presenti sul disco. Costruito e avviato senza warning; il flusso
    salva/apri non testato interattivamente (richiede il vero
    `FileDialog` nativo, non automatizzato in questo ambiente — vedi TODO
    punto 6).
12. **"Scollega tutto" e Stop/Esc in stile QLab "panic" (2026-08-30)**:
    due azioni distinte, richieste separatamente dall'utente.
    `PatchManager::clearAllConnections()` rimuove tutti i collegamenti di
    routing attivi (qualunque input verso qualunque output) senza toccare
    la playlist né la riproduzione — bottone "Scollega tutto" sopra le
    colonne. `PatchManager::stopAllCues()` ferma subito la traccia in
    riproduzione (se c'è), scollegandola da qualunque output, senza
    toccare la playlist o l'indice armato (una successiva pressione della
    barra spaziatrice riparte da dove ci si aspetta) — bottone "Stop
    (Esc)" più `Shortcut { sequence: "Escape" }`, esattamente come il
    tasto Panic di QLab. Costruiti e avviati senza warning; non ancora
    premuti interattivamente (vedi TODO punto 6).

13. **Fix: connettore delle righe Playlist non trascinabile/cliccabile per
    le tracce non ancora in riproduzione (2026-08-30)**: l'utente ha
    riportato "non posso selezionare nulla, il click non funziona e non
    posso patchare" usando l'app per davvero (non un test automatizzato).
    Causa reale: il connettore era abilitato solo con `nodeId > 0`, ma una
    traccia in coda ha sempre `nodeId == 0` finché non parte — e farla
    partire dipendeva SOLO dalla barra spaziatrice (`Shortcut`), la cui
    affidabilità non era mai stata confermata interattivamente (anzi,
    durante i test automatizzati precedenti si erano già visti fallimenti
    intermittenti, allora attribuiti erroneamente all'ambiente di test).
    Se la scorciatoia non scatta, nessuna traccia ottiene mai un nodeId e
    il connettore resta permanentemente disabilitato: né click né
    trascinamento potevano funzionare, esattamente come riportato. **Fix**:
    il connettore ora è sempre abilitato sulle righe Input.
    `PortRow::activateRequested()` (nuovo segnale) scatta con un click
    semplice (spostamento <8px tra pressione e rilascio) quando
    `nodeId <= 0`, e chiama `PatchManager::playCueAt(index)` (nuovo metodo,
    estratto dalla logica già in `advanceCue()`, che ora lo riusa) — avvia
    direttamente quella traccia senza passare dalla tastiera. Un
    trascinamento vero (spostamento maggiore) continua a funzionare come
    prima quando `nodeId > 0`. Il connettore è ora blu quando cliccabile
    per avviare la traccia, per distinguerlo visivamente dal
    verde/grigio "bersaglio di routing" di una traccia già live.
14. **Fix: trascinamento ancora non funzionante dopo il punto 13
    (2026-08-30)**: l'utente ha confermato che il click-per-avviare non
    risolveva il problema — restava impossibile collegare input a output.
    Causa reale, introdotta insieme al `menuBar` (punto 11): tutte le
    coordinate di trascinamento/anchor venivano calcolate con
    `item.mapToItem(null, ...)`, che mappa alla scena INTERA della finestra
    (l'intero `Window`, menuBar incluso); ma sia il `Canvas` dei cavi sia la
    `ColumnLayout` con le colonne sono figli diretti di `ApplicationWindow`,
    quindi il loro vero genitore a runtime è `ApplicationWindow.contentItem`
    — che con un `menuBar` impostato parte più in basso dell'inizio reale
    della finestra, della sua altezza. Le due origini non coincidevano più:
    ogni punto di aggancio, ogni posizione di rilascio e ogni area di
    hit-test per gli output erano sistematicamente sfalsati verticalmente
    dell'altezza del menuBar. Il rilascio quindi non cadeva mai davvero
    sopra il rettangolo registrato per l'output di destinazione, quindi
    `toggleConnection` non veniva mai chiamato. **Fix**: aggiunta una
    proprietà `mapTarget` (un riferimento a `root.contentItem`, passato da
    `Main.qml` giù attraverso `CueList`/`PortColumn` fino a ogni `PortRow`)
    usata al posto di `null` in tutte le chiamate `mapToItem` — così ogni
    coordinata calcolata condivide la stessa origine del `Canvas` che deve
    disegnarla/testarla. **Confermato dall'utente (2026-08-30)**: dopo
    questo fix il trascinamento crea davvero il collegamento (log
    diagnostico temporaneo aggiunto per la conferma: `PatchManager:
    playCueAt`/`nodeId assegnato` in `PatchManager.cpp`, `beginDrag`/
    `endDrag`/`registerAnchor OUTPUT` in `Main.qml`, non ancora rimossi
    — utile se servono altre diagnosi, altrimenti da ripulire).
    L'utente riporta però "funziona ma con qualche intoppo", non ancora
    specificato in dettaglio — vedi TODO punto 6.
15. **Rimozione di una singola connessione cliccando sul cavo
    (2026-08-30)**: richiesta esplicita dell'utente ("devo anche poter
    cancellare le connessioni singolarmente"), oltre al "Scollega tutto"
    già esistente. Una `MouseArea` dentro `cableCanvas` fa hit-test
    manuale (campionamento della curva di Bezier + distanza punto-segmento,
    soglia 8px, `Main.qml`: `findConnectionNear`/`distanceToCable`/
    `distanceToSegment`) e chiama `patchManager.toggleConnection(...)` sulla
    connessione trovata (che la rimuove, dato che esiste già — stessa
    funzione usata per crearla). Se il click non cade abbastanza vicino a
    nessun cavo, `mouse.accepted = false` in `onPressed` lo lascia passare
    agli item sottostanti (connettori, righe, pulsanti), per non rompere il
    trascinamento appena confermato funzionante. Aggiunto un piccolo hint
    testuale in `PatchGrid.qml`. **Confermato dall'utente (2026-08-30)**:
    ora riesce a selezionare/collegare più sink a un unico file.
16. **Fix: si poteva creare una sola connessione per traccia
    (2026-08-30)**: causato dalla soglia di `findConnectionNear` (punto 15)
    che non escludeva i punti vicini ai connettori — un cavo parte
    esattamente dal connettore di origine, quindi premere di nuovo sullo
    stesso connettore (per trascinarne un secondo verso un altro output)
    veniva interpretato come un click di rimozione sul cavo già esistente,
    e il nuovo trascinamento non partiva mai. **Fix**: `findConnectionNear`
    ora esclude un raggio di 16px attorno a entrambi gli estremi di ogni
    cavo. Costruito e avviato senza warning; non ancora ritestato
    dall'utente dopo questo fix specifico (testato insieme al punto 17).
17. **Redesign: routing persistito per traccia + trasporto globale
    Play/Pausa/Stop (2026-08-30)**: su richiesta esplicita dell'utente
    ("voglio che queste patch vengano salvate in memoria e che il play sia
    slegato dalla selezione... ogni volta che mando in play una traccia
    devono essere visibili le patch ai sink"), cambiato il modello: il
    routing non è più legato solo al `nodeId` live (effimero, riassegnato
    ogni volta), ma è una proprietà persistente di ogni `Cue`
    (`desiredOutputNames`, un `QSet<QString>` di nomi stabili di sink, es.
    "alsa_output.pci-..." — non nodeId, che possono cambiare tra una
    riproduzione e l'altra). `PatchManager::toggleCueOutput(cueIndex,
    outputNodeId)` registra/rimuove il routing voluto per nome (funziona
    anche se la traccia non è ancora in riproduzione) e, se la traccia è
    già live, riflette subito la modifica sul collegamento PipeWire reale
    tramite il vecchio `toggleConnection` (ora solo interno). Quando una
    traccia parte (`playCueAt`, sia da `advanceCue`/barra spaziatrice sia
    dal nuovo pulsante Play), non appena PipeWire assegna il nodeId
    (asincrono) `applyDesiredConnections` ricollega automaticamente tutti
    gli output il cui nome è nel set voluto — così i cavi tornano visibili
    "ogni volta che si manda in play" senza doverli ridisegnare a mano.
    Il trascinamento (in `PortRow.qml`, area estesa a tutta la riga, vedi
    punto 14) ora funziona SEMPRE sulle righe Input, anche con `nodeId <=
    0`: porta con sé `rowIndex` (nuova proprietà/parametro nel segnale
    `dragStarted`) invece di dipendere solo dal nodeId live. Click/trascina
    sulla riga non avvia più la riproduzione (rimosso `activateRequested`,
    l'intero concetto di "click per avviare" è sparito): la riproduzione è
    ora esclusivamente gestita da un cluster di pulsanti globali "▶ Play /
    ⏸ Pausa / ⏹ Stop (Esc)" in alto a sinistra, più la barra spaziatrice
    (invariata, `advanceCue()`). **Nuova capacità nel motore**: prima non
    esisteva una vera pausa (solo crea/distruggi lo stream): aggiunto
    `PipeWireEngine::setFileStreamActive(nodeId, active)`
    (`pw_stream_set_active`), che silenzia lo stream senza distruggerlo —
    nodo e collegamenti restano vivi, a differenza di uno stop pieno.
    `PatchManager::pause()`/`play()` lo usano; `paused` è una nuova
    `Q_PROPERTY` (usata anche per mostrare ⏸ invece di ▶ nel prefisso della
    riga in riproduzione). Il routing voluto è anche persistito nel file di
    progetto (`saveProject`/`loadProjectFromPath`: nuovo campo JSON
    `"outputs"` per cue, un array di nomi di sink), quindi riaprire un
    progetto restaura anche la configurazione di routing, non solo la
    playlist — purché i sink con quei nomi siano ancora presenti al momento
    in cui la traccia riparte.

    **Feedback dell'utente dopo il primo test (2026-08-30) e fix
    successivi, stessa sessione**:
    - "quando clicco sulla traccia a sx non la vedo selezionata" +
      "non riesco a cambiare traccia da riprodurre": un click sulla riga
      finiva sempre interpretato come un (nullo) trascinamento, senza modo
      di scegliere manualmente quale traccia armare. **Fix**: nuovo
      `PatchManager::armCue(index)`; `Main.qml::endDrag` distingue click da
      trascinamento in base allo spostamento (<8px = click → `armCue`,
      altrimenti trascinamento come prima). La riga scelta mostra ora un
      bordo evidenziato (`PortRow::armed`, distinto dallo sfondo pieno di
      `selected` riservato alla traccia davvero in riproduzione).
    - "il cavo si deve vedere anche quando la traccia non è in play ma è
      selezionata": il disegno dei cavi leggeva solo `connectionsModel`
      (connessioni PipeWire reali, esistenti solo per la traccia live).
      **Fix**: `cueModel()` espone ora anche `desiredOutputNodeIds` (nodeId
      live correnti di ogni output il cui nome compare nel routing voluto
      della cue), e sia il disegno dei cavi sia l'hit-test per la rimozione
      (`findConnectionNear`) leggono da lì per OGNI traccia, non solo da
      quella in riproduzione. Gli anchor lato Input sono ora indicizzati
      per `rowIndex` (non più per `nodeId`, che vale 0 e collide per tutte
      le tracce non live) — `PortRow::anchorChanged`/`dragStarted` portano
      `rowIndex` end-to-end fino a `Main.qml`.
    - "il triangolino play si deve vedere solo quando quella traccia è in
      esecuzione": rimosso il prefisso "▷" per la traccia solo armata (non
      in riproduzione) — resta solo il bordo evidenziato (`armed`); "▶"/"⏸"
      compaiono solo per `playingCueIndex`.
    Costruito e avviato senza warning dopo ciascun fix.
18. **Colore per traccia + filtro "solo selezionata" (2026-08-30)**:
    richiesta esplicita dell'utente ("fai in modo di poter nascondere le
    patch che non stai editando con un radio button e metti le patch di
    ogni traccia con colore diverso"). `Main.qml`: `cableColors` (10 colori)
    + `colorForCue(index)` (`index % length`) danno un colore distinto a
    ogni traccia, usato sia per i cavi persistenti sia per quello "in volo"
    durante il trascinamento. Due `RadioButton` nella toolbar in alto
    (mutuamente esclusivi, stesso genitore) impostano
    `root.showAllPatches`: se `false`, sia il disegno dei cavi
    (`cableCanvas.onPaint`) sia l'hit-test per la rimozione
    (`findConnectionNear`) mostrano/considerano solo il routing della
    traccia "in armo" (`patchManager.armedCueIndex`, quella che si sta
    editando), nascondendo quello di tutte le altre. Default: tutte
    visibili (`showAllPatches: true`). Costruito e avviato senza warning;
    **non ancora testato dall'utente**.
19. **Bug reale scoperto e risolto: il keepalive Bluetooth non si è MAI
    agganciato automaticamente a nessuna cassa reale (2026-08-30)**.
    L'utente ha chiesto "sei sicuro che funzioni il ping per non farle
    spegnere?" — verifica diretta con `pw-dump`/`pw-link` (5 casse
    Bluetooth reali connesse) ha confermato che il nodo
    `btmultizone.keepalive` non veniva mai creato affatto. Causa: l'annuncio
    iniziale di un sink Bluetooth sul registry PipeWire (`global`) NON
    include ancora `device.api`/`api.bluez5.*` — queste proprietà vengono
    aggiunte al nodo solo un istante dopo, tramite un aggiornamento delle
    sue info (un evento distinto, non un secondo `global`). `onGlobal()`
    controllava `device.api` una sola volta, in quel primo momento, quindi
    `AudioNode::isBluetooth` risultava SEMPRE `false` per ogni sink
    Bluetooth, anche già connesso all'avvio dell'app — bug presente fin
    dalla primissima implementazione del keepalive (il test originale con
    RMS/picco aveva verificato solo il generatore in sé, collegandolo
    manualmente per nodeId all'output analogico interno, bypassando
    completamente il percorso automatico "isBluetooth → aggancio"). Il
    segnale `PipeWireEngine::nodeUpdated` esisteva già in dichiarazione ma
    non veniva MAI emesso da nessuna parte del codice. **Fix**: per ogni
    nodo Sink, `onGlobal()` ora si aggancia anche alle sue info
    (`pw_registry_bind` + `pw_node_add_listener`, nuovo
    `PipeWireEngine::Impl::SinkWatch`/`onSinkNodeInfo`); quando arriva
    l'aggiornamento con `device.api=bluez5`, il nodo memorizzato viene
    corretto ed emette `nodeUpdated` (ora finalmente utilizzato).
    `PatchManager` ha una nuova `handleSinkNode()` condivisa tra
    `nodeAdded`/`nodeUpdated` che aggancia il keepalive appena
    `isBluetooth` diventa vero, da qualunque dei due segnali arrivi.
    **Verificato (2026-08-30)** con `pw-dump`/`pw-link` su 5 casse
    Bluetooth reali connesse: il nodo `btmultizone.keepalive` ora compare e
    risulta collegato a tutte e 5 (`playback_FL`/`playback_FR` di ciascuna)
    subito dopo l'avvio, sia su build incrementale sia su rebuild completo
    da zero. **Nota di metodo per sessioni future**: in questo ambiente
    desktop i lanci di `btmultizone` vengono ri-scopati da systemd (unità
    transitorie "app-...service"), quindi la redirezione `> file 2>&1` di
    stdout/stderr di questo shell NON raggiunge il processo reale — è per
    questo che moltissimi "smoke test" precedenti in questa sessione non
    mostravano mai output anche quando `qDebug()` veniva chiamato
    correttamente. Il canale affidabile per vedere l'output di
    `btmultizone` in questo ambiente è **`journalctl --user`** (i messaggi
    `qDebug()`/`qWarning()` arrivano lì perché Qt rileva l'ambiente systemd
    e li invia direttamente al journal via `sd_journal_send()`,
    bypassando lo stdout/stderr del processo). Un semplice `fprintf(stderr,
    ...)` invece NON funziona in questo ambiente (va comunque a un fd non
    connesso al journal) — usare sempre `qDebug()`/`qWarning()` per
    diagnosi da codice C++ in questo progetto, mai `fprintf`/`printf`
    diretti, e leggere l'output con `journalctl --user -n N --no-pager`
    (eventualmente `| grep <pid o pattern>`), non con redirezione di shell.
20. **Fix: la prima traccia della playlist appariva con il cavo disegnato
    troppo in alto dopo l'apertura di un progetto (2026-08-30)**: causa —
    il testo di suggerimento in `CueList.qml` era visibile solo con
    playlist non vuota (`visible: cueModel.length > 0`); quando un progetto
    con tracce veniva caricato, quel testo compariva nello stesso
    aggiornamento che popolava la `ListView`, spostandone in basso la
    posizione — ma la riga già posizionata non ri-registrava il proprio
    punto di aggancio globale, perché `onXChanged`/`onYChanged` scattano
    solo quando cambia la posizione DELLA RIGA rispetto al proprio genitore
    diretto, non quando si sposta un antenato più in alto nella gerarchia
    (qui la `ListView` stessa). **Lezione da riusare**: qualunque elemento
    la cui visibilità/altezza può cambiare "spingendo giù" un fratello con
    contenuto che riporta la propria posizione globale (qui: i cavi
    disegnati sul `Canvas`) va reso SEMPRE presente (mai nascosto/mostrato
    condizionalmente), altrimenti va esplicitamente ri-propagato un
    ricalcolo quando l'antenato si sposta. **Fix**: il testo è stato
    spostato fuori da `CueList.qml`, in una barra fissa SEMPRE presente in
    fondo alla finestra in `Main.qml` (che unisce anche il vecchio hint di
    `PatchGrid.qml`, ora rimosso per evitare doppioni).
21. **Ridotto il volume del keepalive + nuovo pulsante "identifica" per
    sink (2026-08-30)**: con il keepalive finalmente funzionante per
    davvero (punto 19), l'utente ha sentito il blip come un beep
    percepibile ("non deve succedere"). Ridotta l'ampiezza da 0.02 a 0.005
    (~4x più silenzioso) e la durata da 150ms a 100ms; aggiunta anche una
    dissolvenza di 15ms in apertura/chiusura del tono (prima assente): un
    tono attivato/disattivato di netto genera un click udibile a
    prescindere dal volume, spesso più fastidioso del tono stesso.
    Compromesso NON ancora validato empiricamente — se risultasse
    insufficiente a evitare lo stand-by di qualche dispositivo va rialzata
    l'ampiezza (mai togliere la dissolvenza). Contestualmente, richiesta
    dell'utente per un modo di "pingare" una cassa alla volta per
    identificarla fisicamente: nuovo `PipeWireEngine::identifySink(nodeId)`
    (nuovo `Impl::IdentifyStream`, non condiviso/persistente come il
    keepalive — un nuovo stream per ogni chiamata, che si autodistrugge da
    solo circa 700ms dopo essersi collegato, via `QTimer::singleShot` sul
    thread Qt) riproduce due bip chiaramente udibili (880Hz, 150ms,
    ampiezza 0.35, separati da 120ms di silenzio, con dissolvenza di 10ms).
    Esposto come `PatchManager::identifySink(nodeId)` e un pulsante
    "🔊" su ogni riga della colonna Output in `PortRow.qml`. Anche
    l'icona Bluetooth placeholder è stata sistemata nello stesso giro:
    usava un codepoint `` di un font di icone mai incluso nel
    progetto, mostrato come un quadrato "tofu" (carattere mancante) —
    sostituito con una piccola etichetta testuale "BT". Costruito e
    avviato senza warning (verificato anche che il fix del punto 19 non
    avesse introdotto un doppio collegamento — un run isolato mostra
    esattamente un link per porta, un run precedente con più processi di
    test residui in contemporanea ne mostrava il doppio, non riproducibile
    con un solo processo). **Confermato dall'utente**: l'identify ora
    funziona.
22. **Fix: identify non produceva alcun suono (2026-08-30)**: causa —
    `PW_KEY_MEDIA_ROLE` era impostato a `"Notification"`, che fa scattare
    le politiche di auto-routing/ducking di WirePlumber (i suoni di
    notifica vengono instradati per default all'output attivo del sistema,
    ignorando il `linkNodes()` manuale verso il sink scelto) — a differenza
    di keepalive e file stream, che usano `"Music"` e infatti funzionano
    con il routing esplicito. **Fix**: cambiato anche identify a `"Music"`.
    Aggiunta anche la richiesta dell'utente di un riscontro visivo:
    `PatchManager::identifyingSinkId` (nuova `Q_PROPERTY`, torna a 0 da
    solo circa 700ms dopo la chiamata, via `QTimer::singleShot`) evidenzia
    con uno sfondo pieno il pulsante 🔊 del sink in test. **Confermato
    dall'utente**: ora riesce a identificare le casse.
23. **Rumore bianco invece del tono puro per il keepalive (2026-08-30)**:
    l'utente ha segnalato che il blip restava udibile anche dopo la
    riduzione di volume/durata del punto 21, e ha chiesto se si potesse
    mandare silenzio vero al suo posto. Non è possibile: il motivo stesso
    per cui il keepalive esiste è che molti altoparlanti Bluetooth vanno in
    stand-by rilevando silenzio digitale vero, quindi inviare silenzio
    reale al posto del ping vanificherebbe la funzione. **Fix migliore**:
    sostituito il tono puro a 440Hz con un breve fruscio di rumore bianco
    (xorshift32, nessuna dipendenza da `rand()`/`<random>` nel percorso
    audio realtime) alla stessa durata/dissolvenza — un tono puro spicca
    molto più di un rumore a banda larga della stessa energia perché
    l'orecchio è molto sensibile a una singola frequenza contro il
    silenzio, mentre l'energia del rumore è distribuita su tutte le
    frequenze e viene percepita come molto più tenue a parità di volume.
    Ampiezza ridotta ulteriormente (0.003, ~-50dBFS). Costruito e avviato
    senza warning, collegamento ai sink Bluetooth reali riverificato
    (nessun doppio collegamento, nessun `engineError`). **Non ancora
    testato dall'utente**: resta da verificare sia che il fruscio sia
    davvero meno percepibile del tono, sia — soprattutto, non ancora
    validato in nessuna iterazione — che sia comunque sufficiente a
    evitare lo stand-by reale di un dispositivo lasciato inattivo a lungo.
24. **Il ping non arrivava a JBL Xtreme 3 — quasi certamente artefatto di
    test, non un bug (2026-08-30)**: segnalato dall'utente subito dopo il
    punto 23, mentre aveva la propria istanza reale in esecuzione E io
    stavo contemporaneamente lanciando un mio processo di verifica
    separato per lo stesso identify/keepalive fix — probabile causa di una
    race transitoria sui link. Verificato subito dopo (con un solo
    processo attivo, il loro): JBL Xtreme 3 risultava correttamente
    collegato al generatore keepalive su entrambi i canali, senza
    doppioni. **Lezione da riusare**: non lanciare MAI un processo
    `btmultizone` di verifica proprio mentre l'utente ha la propria
    istanza reale aperta per testare — anche per un controllo rapido via
    `pw-dump`/`pw-link`, il rischio di interferenza (due generatori
    "btmultizone.keepalive" distinti, race sui link) supera il valore
    della verifica. Se serve controllare qualcosa mentre l'utente sta
    testando, usare solo strumenti di sola lettura (`pw-dump`, `pw-link
    -l`, `bluetoothctl`) sulla LORO istanza, mai avviarne una propria in
    parallelo.

25. **Pannello "Trasforma" opzionale: loop count, reverse, rotazione output
    (2026-08-30)**: richiesto dall'utente — una terza colonna centrale,
    nascosta di default e mostrabile col pulsante "🎛 Trasforma" in barra
    (`root.showTransformPanel`, sostituisce `PatchGrid` mantenendo la stessa
    posizione), che permette per ogni traccia della playlist:
    - **Numero di loop**: infinito (default, comportamento storico) o un
      numero editabile di ripetizioni, dopodiché la traccia si ferma da
      sola (come un utente che preme Stop).
    - **Riproduzione al contrario**: dall'ultimo campione al primo.
    - **Rotazione output** (richiesta con un secondo messaggio nella stessa
      chat): invece di suonare simultaneamente su tutti gli output
      collegati a una traccia, ne tiene attivo uno solo alla volta e passa
      al successivo (in ordine di collegamento) ad ogni giro completo della
      traccia — utile per far "girare" un sottofondo tra più zone nel tempo
      invece di diffonderlo ovunque insieme. Disponibile solo se la traccia
      ha almeno due output collegati.

    Cambiamenti principali:
    - `PipeWireEngine::createFileStream` non legge più il file in streaming
      sequenziale con libsndfile ad ogni callback: lo carica **per intero in
      memoria** una volta sola all'apertura (necessario per poter leggere
      all'indietro, impossibile con la sola lettura sequenziale). Nuovi
      parametri `loopCount`/`reverse`; nuovi metodi
      `setFileStreamLoopCount`/`setFileStreamReverse` per modifiche a caldo
      su una traccia già in riproduzione (via `std::atomic`, niente lock nel
      percorso realtime). Nuovi segnali `fileStreamFinished` (loop count
      esaurito) e `fileStreamLooped` (ogni giro, incluso l'ultimo).
    - `PatchManager`: `Cue` guadagna `loopCount`/`reverse`/`rotateOutputs`/
      `rotateOutputIndex`; `desiredOutputNames` **cambiato da `QSet<QString>`
      a `QVector<QString>`** (ordine di collegamento, necessario per una
      rotazione deterministica — anche l'ordine con cui i cavi vengono
      disegnati/salvati ora rispecchia l'ordine di collegamento, non più
      l'ordine hash del QSet). Nuovi `Q_INVOKABLE setCueLoopCount`/
      `setCueReverse`/`setCueRotateOutputs`; nuovo helper privato
      `resyncRotationConnection` (scollega tutto tranne l'output attivo,
      collega quello attivo) condiviso da `advanceOutputRotation`,
      `toggleCueOutput` e `applyDesiredConnections` quando `rotateOutputs`
      è true. `loopCount`/`reverse`/`rotateOutputs` persistiti nel file di
      progetto JSON.
    - Nuovo `qml/TransformPanel.qml` (stile coerente con
      `CueList`/`PortColumn`: header + `ListView` indicizzata per `index`
      sullo stesso `cueModel`), registrato in `CMakeLists.txt`. In
      `Main.qml` le due colonne laterali sono passate da
      `Layout.preferredWidth: (root.width - 200) / 2` a
      `Layout.fillWidth: true` per adattarsi automaticamente alla larghezza
      variabile del centro (120px placeholder vs 300px pannello) — la
      larghezza reale della `ListView` di ogni colonna cambia quindi
      davvero (non solo la posizione di un antenato), il che fa scattare
      correttamente `onWidthChanged`/`reportAnchor()` di ogni riga: stesso
      meccanismo del bug del punto 20, ma qui il fix è strutturale, non un
      workaround.

    **Non ancora testato interattivamente dall'utente** (solo build-verify:
    compila pulito, AOT QML compila senza errori — vedi
    [[feedback-user-does-ui-testing]] per il motivo per cui non è stato
    lanciato un controllo a schermo: l'istanza reale dell'utente era in
    esecuzione). Da verificare in particolare: loop count che si ferma
    davvero da solo, reverse audibilmente corretto, rotazione che passa
    davvero da un output all'altro ad ogni giro senza sovrapposizioni.

26. **Cue polifoniche stile QLab: pre wait/durata/post wait, avviare una
    traccia non ferma più le altre (2026-08-30)**: richiesto dall'utente —
    "ogni traccia deve funzionare come una cue, avere una durata, un pre
    wait e un post wait... quando do play alla cue successiva non deve
    interrompersi quella precedente". Cambiamento comportamentale
    importante: **prima** avviare una traccia (playCueAt/advanceCue)
    fermava sempre quella precedente (un solo `m_playingIndex` globale);
    **ora** più tracce possono restare in riproduzione insieme, esattamente
    come cue audio multiple in QLab.

    Per traccia (editabile col tasto destro sulla riga della playlist →
    modal "Configura traccia" in `Main.qml`, persistito nel progetto JSON):
    - **Pre wait**: ritardo (secondi) tra Play/trigger e l'inizio VERO
      dell'audio. Durante l'attesa la riga mostra "⏳" ed è considerata
      "occupata" (blocca un secondo trigger sulla stessa traccia).
    - **Durata**: se >0, la traccia si ferma da sola dopo tot secondi di
      riproduzione, indipendentemente da loopCount — utile per "sottofondo
      in loop infinito che dura esattamente 5 minuti" senza calcolare a
      mano i loop. 0 = nessun limite (comportamento storico: dura finché i
      loop non finiscono da soli o viene fermata a mano).
    - **Post wait**: dopo la fine "naturale" (durata scaduta O loopCount
      esaurito — MAI dopo uno stop manuale, quello resta istantaneo), la
      traccia resta collegata ma silenziosa per tot secondi prima di
      scollegarsi davvero (riga mostra "…"). 0 = smontaggio immediato.

    Cambiamenti principali (necessari per rendere sicura la polifonia, non
    solo per i tre campi):
    - `Cue` guadagna un **id stabile** (`quint64`, mai riassegnato,
      indipendente dalla posizione in `m_cues`) e un
      `playbackGeneration` (incrementato ad ogni trigger): tutti i timer
      asincroni (pre wait, durata, post wait) catturano id+generation, non
      un indice grezzo — necessario perché con più tracce avviabili in
      contemporanea e rimozioni nel mezzo, un indice catturato al momento
      del trigger può diventare stale o puntare a una traccia diversa
      prima che il timer scatti. `findCueIndexById` fa da lookup robusto in
      ogni callback.
    - **Bug latente scoperto e corretto nello stesso passaggio**: la
      correlazione nodeId↔cue in arrivo da `nodeAdded` (async, PipeWire
      assegna il nodeId dopo la connessione dello stream) si basava su un
      singolo `m_pendingCueIndex` globale — con due `playCueAt` ravvicinati
      (ora possibile, prima non lo era mai) il secondo avrebbe sovrascritto
      il primo, perdendo per sempre il nodeId della prima traccia. Fix:
      `PipeWireEngine::createFileStream` ora ritorna il **nome** univoco
      dello stream PipeWire generato (`QString`, non più `bool`); ogni
      `Cue` lo tiene in `pendingStreamName` finché `nodeAdded` non lo
      corrisponde per nome, non per posizione/ordine.
    - `PatchManager::m_playingIndex`/`m_pendingCueIndex`/`m_paused`
      (globali, singoli) **rimossi**: sostituiti da stato per-cue
      (`Cue::nodeId`, `waitingToStart`, `inPostWait`, `ended`, `paused`).
      Rimossa anche la Q_PROPERTY `playingCueIndex`/`paused` di
      `PatchManager`: la UI ora calcola "c'è qualcosa in riproduzione/in
      pausa/riprendibile" scorrendo `cueModel` (`Main.qml`:
      `anyCuePlaying`/`anyCuePausable`/`anyCueResumable`), usate per
      abilitare i pulsanti Play/Pausa/Stop.
    - `play()`/`pause()` globali ora agiscono su **tutte** le tracce in
      riproduzione/pausa insieme (non più una sola): Pausa mette in pausa
      tutto ciò che sta suonando, Play riprende tutto ciò che è in pausa
      (o, se niente era in pausa, avvia la traccia in armo senza fermare le
      altre). Stop (Esc/pulsante) ferma tutto, invariato nello spirito.
    - Nuovo tasto destro su `PortRow.qml` (solo lato Input): `MouseArea`
      estesa a `Qt.LeftButton | Qt.RightButton`, il tasto destro emette
      `configureRequested()` invece di iniziare un trascinamento.

    **Non ancora testato interattivamente dall'utente** (solo build-verify:
    build incrementale pulita + build pulita da zero in una directory
    temporanea separata, entrambe senza errori — l'istanza reale
    dell'utente era in esecuzione, quindi nessun lancio di verifica, vedi
    [[feedback-user-does-ui-testing]]). Da verificare in particolare: due+
    tracce che suonano davvero insieme senza che una interrompa l'altra,
    pre wait che ritarda l'audio ma non il resto della UI, durata che
    ferma solo quella traccia lasciando le altre intatte, post wait
    percepibile (routing visivo ancora presente per qualche secondo dopo
    che l'audio finisce), e il modal di configurazione (tasto destro sulla
    riga) che si apre/chiude/salva correttamente.

27. **Rotazione output diventata a comando (barra spaziatrice) invece che
    automatica per loop, etichette visive, testo illeggibile nel pannello
    Trasforma corretto, cavi che non attraversano più il pannello
    (2026-08-30)**: quattro correzioni richieste dall'utente dopo aver
    provato il punto 25/26.
    - **Rotazione ora manuale**: la connessione automatica
      `fileStreamLooped` → `advanceOutputRotation` (introdotta al punto 25)
      è stata **rimossa**. `PatchManager::advanceCue()` ora controlla PRIMA
      se una traccia in riproduzione ha `rotateOutputs` con più di un
      output: se sì, la barra spaziatrice avanza la SUA rotazione invece di
      avviare la traccia successiva in coda — la rotazione "occupa" la
      barra spaziatrice finché una traccia così sta suonando.
    - **Etichette di rotazione**: sotto la riga di una traccia con
      `rotateOutputs` attivo, `CueList.qml` mostra ora tante piccole
      etichette quanti sono gli output collegati (nome leggibile, non id
      PipeWire), con quella davvero attiva evidenziata — il delegate della
      ListView è passato da un singolo `PortRow` a un `ColumnLayout`
      (`PortRow` + `RowLayout` di etichette condizionale). Richiesto
      `cueModel()` esteso con `desiredOutputLabels` (nomi leggibili,
      `AudioNode::description`, stessa posizione/ordine di
      `desiredOutputNodeIds`) — nuova mappa `m_outputNodeDescriptions` in
      `PatchManager`, popolata/svuotata insieme a `m_outputNodeNames`.
      **Bug corretto nello stesso passaggio**: `desiredOutputNodeIds` era
      costruito iterando `m_outputNodeNames` (ordinato per nodeId scoperto),
      non nell'ordine di collegamento/rotazione — ora costruito iterando
      `Cue::desiredOutputNames` direttamente, quindi nello stesso ordine
      ovunque (etichette, evidenziazione, cavi).
    - **Testo bianco su bianco nel pannello Trasforma**: l'app non imposta
      uno stile `QtQuick.Controls` esplicito, quindi `CheckBox`/`Label`/
      `SpinBox` prendevano i colori di default dello stile della
      piattaforma — su alcuni ambienti risultava testo chiaro su sfondo
      chiaro. Fix a due livelli: `palette.windowText`/`palette.text`/
      `palette.buttonText` impostati su `ApplicationWindow` (si propagano a
      tutti i `Control` della finestra) in `Main.qml`, PIÙ colori espliciti
      punto per punto in `TransformPanel.qml` (coerente con il resto del
      codebase, che non si è mai affidato ai default di stile).
    - **Cavi che non attraversano più il pannello Trasforma**: il pannello
      è stato ristretto (`Layout.preferredWidth` 300→210) e, soprattutto,
      `cableCanvas` (il `Canvas` che disegna i cavi, sempre sopra a tutto)
      ora ritaglia (`ctx.clip()`) il disegno di ogni cavo quando il
      pannello è visibile: la stessa curva viene disegnata due volte, una
      ritagliata alla zona a sinistra del pannello e una a destra
      (`drawCableAroundTransformPanel` in `Main.qml`) — il tratto centrale
      semplicemente non viene disegnato, dando l'effetto "entra a sinistra,
      esce a destra" richiesto invece di un cavo sopra ai controlli.

    **Non ancora testato interattivamente dall'utente** (solo build-verify,
    stesso motivo dei punti precedenti — istanza reale in esecuzione).

28. **Ritardo di output regolabile per sink, per sincronizzare audio
    interno e Bluetooth (2026-09-02)**: richiesto esplicitamente
    dall'utente — "l'audio interno ha meno delay di quello trasmesso su
    bluetooth, vorrei poter impostare un ritardo sull'audio interno per
    poterlo far suonare contemporaneamente". Un sink Bluetooth aggiunge
    sempre trasporto+decodifica A2DP (tipicamente 100-300ms) rispetto
    all'audio interno, che parte "in anticipo" se la stessa cue è collegata
    a entrambi contemporaneamente — la soluzione è ritardare il percorso
    più veloce, non provare ad "accelerare" quello Bluetooth.

    Realizzato in modo GENERICO (qualunque output, non solo quello
    interno) interponendo un `pw_filter` dedicato tra le sorgenti e il
    sink reale, invece di limitarsi a un parametro applicativo: i link
    PipeWire sono passaggio diretto di campioni, quindi l'unico modo per
    introdurre un vero ritardo relativo tra due percorsi che condividono la
    stessa sorgente è interporre un vero stadio di elaborazione nel grafo,
    non un flag.
    - **`PipeWireEngine::Impl::DelayFilter`** (nuovo, in
      `PipeWireEngine.cpp`): un `pw_filter` con 2 porte di ingresso e 2 di
      uscita in modalità DSP (`spa_format_audio_dsp_build`,
      `SPA_AUDIO_FORMAT_DSP_F32`, niente `PW_KEY_MEDIA_CLASS` quindi
      invisibile in UI, stesso principio del generatore keepalive). Un ring
      buffer per canale (3s di capacità a 48kHz, letto/scritto SOLO dal
      thread audio in `onDelayFilterProcess`) realizza il ritardo:
      ogni ciclo scrive i campioni in ingresso in coda al buffer e legge
      quelli in uscita da `delayFrames` campioni indietro — `delayFrames`
      è un `std::atomic<int>` (scritto dal thread Qt via
      `setOutputDelayMs`, letto dal thread audio), stesso principio già
      usato per `FileStream::reverseRequested`. Creato pigramente alla
      prima richiesta con delayMs>0 per un dato sink e MAI distrutto per il
      resto della sessione anche se il ritardo torna a 0 (diventa un
      passthrough) — evita di dover ricollegare la topologia esistente ad
      ogni cambio.
    - **Reindirizzamento trasparente in `linkNodes`**: se il sink di
      destinazione ha già un filtro attivo, `PipeWireEngine::linkNodes`
      collega automaticamente alle sue porte di ingresso invece che al sink
      direttamente — nessuna modifica richiesta a `PatchManager` o al resto
      del routing (drag&drop, rotazione output, cue polifoniche: tutti
      continuano a chiamare `linkNodes(sorgente, sinkNodeId)` come prima).
      Il corpo originale di `linkNodes` (abbinamento porte per canale,
      retry su discovery incompleto, rollback su errore parziale) è stato
      estratto in `Impl::createPortLink`, usato anche per il link
      permanente filtro→sink reale (creato una sola volta non appena
      PipeWire assegna il nodeId del filtro, stesso pattern asincrono di
      FileStream/keepalive) — DEVE restare un metodo separato dal
      `linkNodes` pubblico, altrimenti quel link si richiuderebbe su se
      stesso invece di raggiungere il sink vero.
    - **`AudioEngine::setOutputDelayMs`** (nuovo metodo virtuale puro
      nell'interfaccia astratta): implementato per intero su
      `PipeWireEngine` (Linux); su `CoreAudioEngine`/`WasapiEngine`
      (macOS/Windows) è per ora uno stub che segnala l'errore via
      `engineError` invece di fallire silenziosamente — nessuna cassa
      Bluetooth reale disponibile per validare un'implementazione nativa su
      quelle piattaforme in questa sessione.
    - **`PatchManager::setOutputDelayMs`/`AudioNode::delayMs`/
      `PortModel::DelayMsRole`**: stesso schema di persistenza per nome
      stabile del sink già usato da `setOutputNickname` (sopravvive a
      riavvii/riconnessioni del dispositivo), applicato subito al motore
      alla riscoperta del sink (`handleSinkNode`). UI: pulsante "⏱" su ogni
      riga della colonna Output (`PortRow.qml`, stesso stile a pillola di
      identifica/muto, mostra "⏱ Nms" quando N>0) che apre un dialog con
      uno `SpinBox` 0-2000ms (`Main.qml`: `delayDialog`).

    **Verificato strutturalmente (2026-09-02)** con un harness standalone
    (stesso pattern riusato in tutta la storia di questo progetto: chiama
    `PipeWireEngine` direttamente, bypassando QML) compilato a parte con
    `moc`/g++ e collegato al vero `PipeWireEngine.cpp`: trovato il sink
    audio interno reale, chiamato `setOutputDelayMs(sink, 300)`, creato un
    file stream reale e collegato al sink con `linkNodes`. `pw-dump`
    (interrogato mentre l'harness era ancora in esecuzione, non dopo la sua
    uscita — un primo tentativo dopo l'uscita non mostrava più nulla,
    ovviamente: `stop()` distrugge tutto) conferma ESATTAMENTE la topologia
    attesa: nodo `bluecue.delay.56` con 2 porte in (`in_0`/`in_1`) e 2 porte
    out (`out_0`/`out_1`), il file stream collegato alle porte IN del
    filtro (non al sink direttamente), le porte OUT del filtro collegate
    permanentemente alle porte reali del sink (`playback_FL`/
    `playback_FR`). Nessun `engineError` nel log. **Non ancora verificato
    l'effetto audio reale** (che il ritardo percepito sia effettivamente
    ~300ms e che l'audio non abbia artefatti/click) — richiede un ascolto
    reale con una cassa Bluetooth vera collegata in parallelo all'audio
    interno, lasciato all'utente (vedi
    [[feedback-user-does-ui-testing]]). Il rate del grafo è assunto 48kHz
    (hardcoded in `DelayFilter::rate`, stessa assunzione già presente altrove
    nel codebase per keepalive/identify) — da rivedere se in futuro
    emergesse un sistema con un sample rate di default diverso.

29. **Catturare l'audio di un'altra applicazione (Firefox, ecc.) come
    sorgente in playlist, "sposta" non "duplica" (2026-09-02)**: richiesto
    esplicitamente dall'utente subito dopo il punto 28 — "vorrei poter
    aggiungere anche gli stream audio come fonte nella tracklist (firefox,
    o altri stream che potrei aprire sul pc)". Chiesto esplicitamente
    all'utente se l'audio dell'app dovesse continuare a sentirsi ANCHE
    sull'uscita di sistema originale (duplica) o smettere di uscire da lì
    (sposta): scelto "sposta".
    - **Nuovo `AudioNode::Kind::AppStream`**: `kindFromMediaClass` riconosce
      ora anche `Stream/Output/Audio` (i nodi di stream di riproduzione di
      qualunque app, Firefox/VLC/ecc. — prima ignorati del tutto,
      `Kind::Unknown`). `PatchManager::handleAppStreamNode` li traccia in
      `m_availableAppStreams` (esclude il nostro stesso `bluecue.appcapture.
      *`/"-in" per nome), esposti a QML via `appStreamsModel`. Nuovo
      pulsante "🎧+" in `CueList.qml` apre un elenco (`Main.qml`:
      `appStreamPickerDialog`) da cui scegliere quale stream catturare.
    - **`PatchManager::addAppStreamCue(appStreamNodeId)`**: crea una cue con
      `isAppStream=true` (in coda, non avviata subito — parte con
      Play/barra spaziatrice come una cue file, riusando IDENTICO tutto il
      resto dell'infrastruttura cue: pre wait/durata/post wait, drag&drop
      verso gli output, rotazione output, tutto già generico rispetto al
      tipo di sorgente). Quando parte (`startCueNow`), invece di
      `createFileStream` chiama `beginAppStreamCapture`: crea un sink
      virtuale di cattura dedicato (`PipeWireEngine::createVirtualSink`,
      nome `bluecue.appcapture.<appStreamNodeId>`) e, una volta correlato
      per nome il suo nodeId (stesso schema asincrono di
      `pendingStreamName`), chiama `setStreamTarget` per "spostare" lo
      stream applicativo lì dentro — `Cue::nodeId` diventa il nodeId del
      sink di cattura, riusato ovunque il resto del codice si aspetta "il
      nodo live da cui collegare gli output" (le sue porte `monitor_*`
      portano l'audio catturato, linkabili con `linkNodes` come qualunque
      altra sorgente, delay filter compreso — vedi sotto).
    - **Fix di `createVirtualSink` scoperto per necessità**: il modulo
      `libpipewire-module-loopback` di base rimanda l'audio catturato
      ANCHE indietro verso l'uscita di sistema di default tramite il
      proprio stream di playback interno (il lato "-in") — verificato
      empiricamente con `pw-dump`, esattamente la duplicazione che
      l'utente aveva scartato. Fix: `node.autoconnect = false` sulle
      `playback.props` di quello stream interno, così l'audio catturato
      resta confinato al sink virtuale finché non è la nostra
      `linkNodes()` esplicita a instradarlo. Anche `audio.channels`/
      `audio.position` espliciti (mancavano) e un `node.name` a livello
      TOP del modulo (prima solo dentro `capture.props`, necessario perché
      il modulo assegni un `node.group`/`node.link-group` coerente).
    - **`AudioEngine::setStreamTarget`/`clearStreamTarget`** (nuovi metodi
      virtuali; stub "non implementato" su CoreAudioEngine/WasapiEngine):
      usano la metadata "default" di PipeWire (chiave `target.object`,
      stessa che usano pavucontrol/wpctl) per "spostare" uno stream. **Bug
      scoperto e risolto nello stesso giro, il più importante di questa
      voce**: la sola metadata NON basta in modo affidabile.
      - Il valore va scritto con tipo `Spa:String` esplicito e SENZA
        virgolette JSON — con un valore JSON-quotato o senza tipo, la
        metadata risultava impostata correttamente (verificabile con
        `pw-metadata`) ma WirePlumber la ignorava silenziosamente, nessun
        errore.
      - **Molto più subdolo**: con un `DelayFilter` (punto 28) presente
        OVUNQUE nel grafo — anche per un sink completamente estraneo allo
        spostamento in corso — WirePlumber smette di onorare
        `target.object` per QUALUNQUE stream, in modo permanente finché il
        filtro esiste (non un problema di timing: riprodotto anche
        aspettando 5+ secondi dalla creazione del filtro prima di provare
        lo spostamento). Non è stato possibile determinare la causa esatta
        dentro WirePlumber in tempi ragionevoli — isolato con una serie di
        harness standalone che hanno escluso PatchManager, il timing, e le
        proprietà del sink di cattura (reso via via identico, proprietà
        per proprietà, a un sink creato dalla CLI `pw-loopback` che invece
        funzionava sempre) come cause, lasciando come unica variabile
        residua la presenza del filtro.
      - **Fix robusto**: invece di dipendere dalla cooperazione (dimostrata
        inaffidabile) del session manager, `setStreamTarget`/
        `clearStreamTarget` prendono il controllo diretto del link,
        esattamente come fa già tutto il resto del routing di quest'app.
        Nuovo tracciamento in `PipeWireEngine::Impl`: `allLinks` (OGNI link
        del grafo, non solo i nostri, popolato dal discovery del registry
        su `PW_TYPE_INTERFACE_Link`, prima ignorato) e un listener sulla
        metadata "default" (`onDefaultMetadataProperty`) che tiene
        sincronizzato `defaultAudioSinkName` (parsing minimale del JSON
        `{"name":"..."}` della chiave `default.audio.sink`). `setStreamTarget`
        imposta comunque la metadata (innocuo, un suggerimento in più) poi,
        se lo stream non risulta già collegato al sink target
        (`isNodeLinkedToNode`), distrugge qualunque suo link esistente
        (`destroyForeignLinksFromNode`, via `pw_registry_destroy` — non
        bind+`pw_proxy_destroy`, che rilascerebbe solo il nostro
        riferimento locale senza chiedere al server di distruggere
        l'oggetto altrui, la stessa primitiva di "pw-link -d") e ne crea
        uno nuovo con `createPortLink`. `clearStreamTarget` fa il
        simmetrico all'indietro: cancella la metadata, distrugge il link
        verso il nostro sink di cattura, e ricollega esplicitamente al sink
        di sistema corrente (risolto da `defaultAudioSinkName`) — **anche
        qui scoperto necessario**: uno stream scollegato con un
        `target.object` appena rimosso NON veniva ripreso in carico da
        WirePlumber (osservato scollegato per 10+ secondi in un test
        controllato), a differenza di un semplice scollegamento "grezzo"
        mai passato da un target esplicito (quello sì, ripreso in carico
        da solo in ogni test). L'ordine delle due operazioni dentro
        `clearStreamTarget` conta: cancellare la metadata PRIMA di
        distruggere il link (mai il contrario, verificato peggiorare la
        situazione).
      - `PatchManager::m_appStreamReassertTimer` (5s) riafferma
        periodicamente `setStreamTarget` per ogni cue "sorgente app"
        attiva — auto-recovery richiesta esplicitamente dall'utente,
        idempotente (`isNodeLinkedToNode` la rende un no-op se il link è
        già quello giusto).
    - Cue "sorgente app" NON persistite nel file di progetto
      (`writeProjectToPath` le salta): un nodeId di sessione non stabile
      non ha nulla di sensato da ricaricare.

    **Verificato end-to-end (2026-09-02)** con una serie di harness
    standalone (`PipeWireEngine` puro, poi `PatchManager` completo con un
    `BluetoothManager` finto) contro un vero stream `pw-play` di lunga
    durata usato come "app" di prova, includendo esplicitamente lo scenario
    avverso (delay filter presente sul sink di destinazione — verificato
    con `engine->setOutputDelayMs` chiamato apposta prima del test).
    Confermato con `pw-dump`, durante l'esecuzione dell'harness (non dopo:
    `stop()` disfa tutto), l'intera catena `pw-play → sink di cattura
    (monitor_*) → filtro di ritardo (in_*/out_*) → sink reale
    (playback_*)` — cattura e ritardo compongono correttamente insieme.
    Confermato anche il ciclo completo: `addAppStreamCue` → `playCueAt`
    (sposta davvero, sink di sistema scollegato) → `toggleCueOutput`
    (instradato nella patch bay) → `removeCue` (stream restituito al sink
    di sistema corrente, sink di cattura distrutto, nessun residuo
    `bluecue.appcapture.*` nel grafo). **Lezione generale da questa
    sessione**: mai lanciare un processo di verifica proprio mentre
    l'istanza reale dell'app è aperta (successo per caso durante questo
    sviluppo — vedi [[feedback-check-for-live-instance-before-testing]],
    scritta apposta dopo averlo fatto per errore a metà di questa stessa
    sessione). **Testato dall'utente nella UI reale (2026-09-02) — trovati e
    risolti due problemi reali non emersi con `pw-play`**:
    - **Messaggio d'errore inutile per diagnosticare**: "Porte non trovate
      per uno dei due nodi (discovery incompleto?)" non diceva QUALI nodi
      né se uno dei due fosse addirittura sparito nel frattempo. Esteso a
      includere entrambi gli id, quante porte ciascuno ha trovato, e se il
      nodo stesso è ancora noto ("presente" vs "SPARITO") — stato
      determinante per capire il problema vero sotto. Aggiunti anche
      `qDebug()` mirati in `beginAppStreamCapture`/la correlazione del sink
      di cattura/`setStreamTarget` per tracciare l'intera sequenza.
    - **Bug reale, il più importante**: catturando un video YouTube in
      Firefox (riproduzione continua, nessuna pausa/pubblicità/interruzione
      confermata dall'utente), lo stream PipeWire di Firefox (nodeId scelto
      dal picker) **spariva e ne veniva ricreato uno nuovo** nel giro di
      pochi istanti — invisibile all'utente (l'audio continuava a sentirsi
      normalmente da Firefox), ma fatale per la cattura, che teneva un
      singolo nodeId congelato e si arrendeva non appena spariva.
      Causa identificata ispezionando le proprietà reali del nodo
      (`client.api = "pipewire-pulse"`): Firefox passa dal layer di
      compatibilità PulseAudio, che a quanto pare ricrea il proprio stream
      più spesso di un client PipeWire nativo, anche a riproduzione
      ininterrotta. **Fix**: nuovo `AudioNode::appProcessId`
      (`PW_KEY_APP_PROCESS_ID`, il PID del processo proprietario — stabile
      anche se lo stream cambia nodeId) e `Cue::appProcessId`, copiato al
      momento di `addAppStreamCue`. Quando lo stream catturato sparisce
      (`nodeRemoved`), invece di fermare la cattura si cerca subito un
      sostituto dallo stesso PID tra gli stream già noti
      (`findReplacementAppStreamNodeId`); se non c'è ancora,
      `Cue::appStreamNodeId` resta a 0 ("in attesa") finché
      `handleAppStreamNode` non ne scopre uno nuovo dallo stesso processo e
      lo raggancia da solo (`setStreamTarget` di nuovo, sink di cattura
      mai distrutto/ricreato — resta lo stesso per tutta la cattura).
      Aggiunti guard espliciti (`appStreamNodeId != 0`) in
      `stopCueAt`/`setCueLiveActive`: senza, un tentativo con
      `appStreamNodeId == 0` avrebbe scritto sulla metadata con subject 0,
      che in PipeWire è quello dei default globali
      ("default.audio.sink"/ecc.), non "nessuno stream".
    - Anche il controllo "non staccare un link se non è già verificato che
      si può ricreare" (vedi sopra) si è rivelato utile in pratica: nei log
      reali ha correttamente rimandato invece di rompere qualcosa quando il
      sink di cattura non aveva ancora le sue porte pronte.
    - **Confermato dall'utente (2026-09-02)**: dopo questo giro di fix, la
      cattura di un video YouTube in Firefox "ora sembra funzionare".

30. **Cue "sorgente app" persistite nel file di progetto (2026-09-02)**:
    richiesto esplicitamente dall'utente subito dopo aver confermato il
    fix del punto precedente ("mannaggia a te se ha senso salvare, fallo
    subito"). Nuovo `Cue::appStreamMatchName` (il nome tecnico stabile
    dello stream, es. "Firefox", copiato da `AudioNode::name` al momento
    di `addAppStreamCue`) — PERSISTITO in JSON insieme a `isAppStream`,
    a differenza di `appStreamNodeId`/`appProcessId` (id di sessione, mai
    salvati). Al caricamento la cue torna con `appStreamNodeId`/`nodeId`
    a 0 ("non ancora risolta"): `PatchManager::startCueNow` ri-risolve al
    volo, al momento del Play, cercando tra gli stream applicativi
    attualmente noti quello con lo stesso `appStreamMatchName` — se non
    trovato (l'app non è aperta in quel momento), fallisce con un
    `patchError` invece di partire a vuoto. Ambiguo se più stream con lo
    stesso nome sono attivi insieme (es. due finestre Firefox): prende il
    primo, limite accettato — nessuna proprietà PipeWire distingue in modo
    affidabile due istanze della stessa app. **Non ancora testato
    dall'utente** (implementato subito dopo la richiesta, build verificata
    ma nessun giro di salva-chiudi-riapri-carica-play ancora confermato).

31. **Calibrazione automatica del ritardo di output, tramite misura
    acustica reale (2026-09-02)**: richiesto esplicitamente dall'utente
    ("mi serve un modo per correggere automaticamente il ritardo, non
    riesco a farlo dalla ui, calcolalo e applicalo automaticamente").
    Nessuna proprietà PipeWire espone in modo affidabile la latenza reale
    di un sink Bluetooth (dipende da codec/dispositivo/condizioni radio,
    non standardizzata da A2DP — verificato ispezionando le proprietà
    reali di due casse BT connesse, nessun valore numerico di latenza
    presente) — confermato con l'utente che l'unico modo per avere un
    numero vero è misurarlo acusticamente, non stimarlo.
    - **Meccanismo** (`AudioEngine::calibrateOutputDelay(sinkA, sinkB,
      micNodeId)`, implementato solo per `PipeWireEngine`/Linux — stub
      "non implementato" su CoreAudioEngine/WasapiEngine): un breve click
      di rumore bianco (15ms, inviluppo con fade-in di 2ms per evitare un
      fronte perfettamente istantaneo) riprodotto sul sink A, poi — un
      secondo dopo — sullo stesso stream riportato al sink B (un solo
      generatore riusato in sequenza via `unlinkNodes`/
      `Impl::createPortLink` diretto: **niente sincronizzazione fra due
      stream diversi**, scelta deliberata per semplicità/robustezza — il
      microfono registra in un unico buffer continuo, la differenza si
      ricava dagli istanti di arrivo misurati nella stessa registrazione,
      non da un trigger condiviso). `createPortLink` diretto invece del
      `linkNodes` pubblico: bypassa deliberatamente un eventuale filtro di
      ritardo già presente sul sink, per non contaminare la misura con un
      valore già applicato in precedenza.
    - **Individuazione dei click**: inviluppo RMS a finestre di ~5ms,
      soglia = 6× il rumore di fondo misurato nel silenzio prima del primo
      click — sufficiente per un impulso di rumore bianco (fronte molto
      più netto di un tono puro), non serve una vera cross-correlazione.
      `deltaMsAtoB = latenza(B) - latenza(A)`: positivo → A è il sink più
      veloce e va ritardato di quel tanto (B riportato a 0, ora è lui il
      riferimento); negativo → il contrario. `PatchManager::
      calibrateOutputDelay` applica il risultato chiamando
      `setOutputDelayMs` su entrambi i sink (già persistito/esposto in UI
      come il resto del ritardo manuale).
    - **UI**: il dialog "Ritardo output" (pulsante "⏱" per riga, vedi
      punto 28) ha ora anche un `ComboBox` per scegliere il secondo output
      da confrontare e un pulsante "🎯 Calibra automaticamente" (disabilitato
      se manca un microfono o se i due output scelti coincidono); il
      microfono viene scelto in automatico (il primo hardware trovato,
      niente picker dedicato per ora — praticamente ogni sistema ne ha uno
      solo). Nuove `PatchManager::microphonesModel`/`calibrationInProgress`/
      segnale `calibrationResult` per il feedback testuale nel dialog.
    - **Verificato con un harness standalone e un vero microfono/
      altoparlante reali (2026-09-02)**, calibrando un sink CONTRO SE
      STESSO (stesso id per A e B: stesso percorso acustico, quindi il
      delta atteso è ~0) — risultato: `deltaMs 0 success true`, confermando
      l'intera catena end-to-end (generazione del click, cambio di
      collegamento in sequenza, cattura reale dal microfono del laptop,
      individuazione dei due transienti).
    - **Bug reale scoperto con un test asimmetrico vero (2026-09-02,
      stesso giorno)**: l'utente ha riportato "ritardo misurato 0ms e non
      si calibra" collegando davvero un output Bluetooth diverso
      dall'interno. Causa sospetta: il click viene sparato SUBITO dopo aver
      collegato lo stream al sink, ma un sink Bluetooth appena collegato
      ha un transitorio di "warm-up" del trasporto A2DP (stesso motivo per
      cui esiste il keepalive — vedi punto 5/19/24) durante il quale il
      primo audio inviato può non arrivare mai fisicamente all'altoparlante
      o arrivarci troncato/distorto, facendo fallire la individuazione
      dell'onset per quel sink. **Fix**: aggiunta un'attesa di 500ms dopo
      il collegamento e prima di sparare il click, sia per il sink A che
      per il sink B (simmetrica: si cancella nel calcolo del delta, non
      distorce la misura relativa). **Non ancora riconfermato dall'utente**
      dopo il fix — il retest era bloccato da due istanze di bluecue aperte
      contemporaneamente nello stesso momento (vedi nota già presente più
      sopra su questo genere di interferenza).

## Bug di packaging CI: deploy tool senza --qmldir (2026-09-04)

Primo test reale su una VM Windows (VirtualBox): l'app non parte, nessuna
finestra, nessun errore visibile nemmeno da PowerShell — coerente con
l'ipotesi già scritta sopra (WIN32_EXECUTABLE senza console). Il file di
log aggiunto in `main.cpp` (vedi sotto) ha permesso di leggere il vero
motivo: warning per moduli QML "non installato" — `QtQuick`,
`QtQuick.Dialogs` (usato da `Main.qml` per il file picker), e a cascata
anche `Main.qml` stesso (che invece è compilato dentro l'eseguibile via
risorsa Qt, non un file esterno — l'errore compare comunque perché
`QQmlApplicationEngine` non riesce a risolvere la catena di import e
segnala fallito l'intero documento).

**Causa**: lo step di packaging Windows in `.github/workflows/ci.yml`
lanciava `windeployqt` senza l'opzione `--qmldir`. Senza quel flag,
`windeployqt` NON ha modo di sapere quali moduli QML servono all'app — li
scopre analizzando gli `import` nei sorgenti `.qml`, non li deduce dal
binario — quindi non copia accanto all'exe i plugin di `QtQuick`,
`QtQuick.Dialogs`, `QtQuick.Layouts` (usato da `DurationField.qml`) ecc.
Il motore QML, a runtime, non li trova più e l'intera UI fallisce a
caricarsi silenziosamente (nessuna finestra, solo warning nel log).

**Fix**: aggiunto `--qmldir qml` alla chiamata `windeployqt` (job
`build-windows`). Controllato lo stesso schema anche per gli altri due
pacchetti, mai testati scaricati e lanciati "a freddo" (solo la build
locale Linux contro il Qt di sistema, che non manifesta il bug perché
tutti i plugin QML sono già installati sul sistema indipendentemente dal
packaging):
- **macOS** (`macdeployqt`): mancava `-qmldir=qml` — aggiunto.
- **Linux** (`linuxdeploy-plugin-qt`): mancava la variabile d'ambiente
  `QML_SOURCES_PATHS` (l'equivalente per questo tool) — aggiunta
  (`QML_SOURCES_PATHS="$PWD/qml"`).

**Aggiornamento (stesso giorno, dopo il fix)**: l'utente ha davvero testato
l'installer aggiornato su una VM Windows (VirtualBox) — **l'app ora si
avvia**, prima vera conferma dal vivo che il fix `--qmldir` era quello
giusto. Da lì sono emersi due problemi nuovi, entrambi diagnosticati e/o
chiariti nella stessa sessione:
- **Bluetooth non trovato**: non è un bug — la VM non ha nessun adattatore
  Bluetooth (confermato in Gestione dispositivi), quindi nessuna app può
  vederne uno lì dentro. Serve un dongle USB Bluetooth passato alla VM (o
  hardware reale) per testare quella parte.
- **Colonna Output vuota anche con l'audio della VM presente in Windows**:
  bug reale, trovato leggendo `WasapiEngine::start()` — vedi il primo punto
  della sezione bug statici qui sotto (`refreshDeviceList(false)` invece di
  `true`), stesso identico bug trovato e corretto anche in
  `CoreAudioEngine::start()` (mai testato dal vivo, ma stesso schema di
  codice, quasi certo lo stesso problema su macOS).

## Bug trovati per revisione statica del codice nei backend Windows/macOS
(2026-09-02/03), mai testati dal vivo

L'utente non ha un Mac/PC Windows a disposizione in questa sessione
("trova un modo per beccare i bugs nelle versioni apple e windows"): questi
sono stati individuati leggendo il codice con attenzione, basandosi su
comportamento documentato delle rispettive API (non su esecuzione reale —
vedi la nota in cima a `WasapiEngine.h`/`CoreAudioEngine.h`/
`AppleBluetoothManager.mm`/`WindowsBluetoothManager.cpp`: nessuno di questi
file è mai stato compilato/eseguito su hardware reale, solo via CI). Tutti
corretti, **nessuno verificato dal vivo** — da confermare quando l'utente
potrà testare su un Mac/PC Windows reale.

- **`WasapiEngine.cpp`/`CoreAudioEngine.cpp`: `start()` chiamava
  `refreshDeviceList(false)` per la scansione iniziale dei device.**
  **Questo è l'unico bug di questa lista CONFERMATO dal vivo** (2026-09-04,
  VM Windows): `emitSignals=false` sopprime l'emissione di `nodeAdded` per
  ogni sink trovato — ma `PatchManager` scopre gli output ESCLUSIVAMENTE
  tramite quel segnale (vedi il commento sopra la sua connessione in
  `PatchManager.cpp`: "copre sia i jack hardware sia i sink Bluetooth già
  connessi al momento dell'avvio"), non c'è nessuna chiamata separata tipo
  "dammi la lista attuale". Risultato: colonna Output vuota per sempre,
  anche con un output audio perfettamente funzionante in Windows/macOS —
  esattamente il sintomo riportato ("vedo Altoparlanti in Windows ma niente
  tra i sink di bluecue"). Il commento originale ("nessun listener QML
  ancora collegato") era una motivazione sbagliata: l'emit passa comunque
  per `QMetaObject::invokeMethod(..., Qt::QueuedConnection)`, quindi la
  consegna avviene solo dopo l'avvio del loop eventi Qt (`app.exec()`) —
  molto dopo che `PatchManager` si è già collegato al segnale in `main.cpp`.
  **Fix**: `refreshDeviceList(true)` in entrambi. Non ancora riverificato
  dal vivo dopo il fix (serve un nuovo giro di CI + retest sulla VM).

- **`main.cpp`: `QT_QPA_PLATFORMTHEME=xdgdesktopportal` applicato senza
  guardia di piattaforma.** Era impostato incondizionatamente per tutti i
  sistemi (serve solo per l'integrazione xdg-desktop-portal su Linux), il
  sospetto principale dietro il primo bug riportato in sessione
  ("l'app non parte su Windows, nessuna finestra, nessun errore"): un
  `WIN32_EXECUTABLE` non ha console, quindi anche un warning Qt per un
  plugin platform-theme inesistente sarebbe stato invisibile, e non è da
  escludere che la ricerca di quel plugin interferisse con l'inizializzazione
  QPA. **Fix**: `#if !defined(Q_OS_MACOS) && !defined(Q_OS_WIN)` attorno
  alla riga. Aggiunto anche, solo per Windows, un message handler Qt
  (`qInstallMessageHandler`) che scrive ogni log in
  `<AppData>/bluecue.log`, per rendere diagnosticabile un futuro fallimento
  all'avvio senza dover lanciare da terminale (che per un `.exe` senza
  console richiederebbe comunque aprirlo PRIMA del doppio click).
- **`WasapiEngine.cpp`: tre thread worker mai inizializzano COM sul thread
  proprio.** `CoInitializeEx` viene chiamato una sola volta, sul thread che
  chiama `start()`, ma il thread di `fileStreamWorkerLoop` (riproduzione
  file), il thread di `setKeepAliveEnabled` e quello di `identifySink`
  girano su thread NUOVI creati con `QThread::create` che non chiamano mai
  `CoInitializeEx` per conto proprio — obbligatorio per ogni thread che
  invoca API COM, per documentazione Microsoft. `createRenderTarget`
  all'interno di questi due ultimi thread chiama `IMMDevice::Activate`
  (una vera creazione COM) da un thread non inizializzato: fallisce con
  `CO_E_NOTINITIALIZED`, intercettato silenziosamente da un semplice
  `if (!target) return;` — keepalive e identify-sink smetterebbero di
  funzionare senza alcun errore visibile. **Fix**: `CoInitializeEx`/
  `CoUninitialize` aggiunti attorno al corpo di tutti e tre.
- **`WasapiEngine.cpp`: `EndpointNotificationClient` mai deregistrata.**
  `stop()` rilasciava l'enumerator senza prima chiamare
  `UnregisterEndpointNotificationCallback` — una notifica di
  aggiunta/rimozione device arrivata dopo `stop()`/durante la distruzione
  avrebbe invocato una callback dentro un `Impl` già smontato. **Fix**:
  nuovo campo `Impl::notificationClient`, deregistrato e rilasciato
  esplicitamente in `stop()` prima di rilasciare l'enumerator.
- **`WindowsBluetoothManager.cpp`: race sull'HANDLE del radio Bluetooth tra
  `connectDevice`/`disconnectDevice` e `refreshDevices()`.** Il thread
  asincrono di connessione cattura per valore lo stesso `HANDLE` tenuto in
  `Impl::recordsByAddress`; se `refreshDevices()` viene richiamato nel
  frattempo (bottone "Aggiorna" nel dialog Bluetooth, o anche solo
  riaprendo il dialog — `onOpened: blueZManager.refreshDevices()` in
  Main.qml), `clearRadioHandles()` chiude quell'HANDLE (magari
  riassegnato nel frattempo dall'OS ad altro) MENTRE il thread in
  background lo sta ancora per usare in `BluetoothSetServiceState`.
  Raggiungibile con un click "Connetti" seguito a ruota da un refresh, non
  uno scenario raro. **Fix**: `DuplicateHandle` chiamato in modo sincrono
  (nessuno yield possibile) prima di avviare il thread — il thread lavora
  su una copia indipendente dell'handle, immune a cosa faccia
  `refreshDevices()` nel frattempo, e la chiude lui stesso a lavoro
  finito.
- **`CoreAudioEngine.cpp`: `kAudioQueueProperty_CurrentDevice` impostata su
  una AudioQueue già avviata.** `createFileStream` crea la coda e la avvia
  SUBITO (`AudioQueueStart`), prima ancora che esista un routing verso un
  output — il primo collegamento arriva sempre dopo, quando l'utente
  patcha il cue su un output. `syncAggregateForProducer`, nel ramo "nuovo
  aggregate", reimposta `CurrentDevice` sulla coda del file stream SENZA
  fermarla prima — ma questa property è documentata da Apple come
  impostabile solo a coda ferma. Senza controllo del valore di ritorno,
  l'effetto sarebbe silenzioso: il cue continuerebbe a suonare sul device
  di default invece che sull'aggregate appena creato, cioè il routing non
  si applicherebbe MAI al primo collegamento di un cue appena aggiunto —
  l'equivalente CoreAudio del problema WirePlumber già trovato su Linux
  (vedi punto 29). **Fix**: `AudioQueueStop`/`AudioQueueStart` attorno alla
  `AudioQueueSetProperty`. I percorsi keepalive/identify-sink impostano già
  `CurrentDevice` PRIMA di avviare la coda, quindi non sono affetti.
- **`CoreAudioEngine.cpp`: il listener di cambio-device non viene mai
  davvero rimosso.** `stop()` chiamava `AudioObjectRemovePropertyListenerBlock`
  con un NUOVO block literal vuoto, diverso (per identità, non solo per
  contenuto) da quello passato a `AudioObjectAddPropertyListenerBlock` in
  `start()` — l'API CoreAudio deregistra per identità del block, quindi
  quella chiamata non deregistrava nulla: un cambio hardware dopo lo
  stop/durante la distruzione avrebbe invocato una callback dentro un
  `Impl` già smontato (stesso genere di bug del punto WASAPI sopra).
  **Fix**: il block è ora tenuto in vita (`Block_copy`, necessario perché
  questo è un file `.cpp` puro, senza ARC, quindi un block literal locale
  non sopravvive di suo oltre `start()`) in `Impl::deviceListChangeBlock` e
  riusato identico per la rimozione in `stop()`.

**Nessuna di queste modifiche è stata compilata**: nessun SDK
Windows/macOS è disponibile in questo ambiente Linux — verificate solo per
coerenza sintattica e comportamento documentato delle rispettive API. Da
compilare/testare alla prima occasione su hardware reale.

## Cosa NON funziona ancora (TODO noti)

1. **Formati compressi (MP3) nella riproduzione file**: **nota corretta
   (2026-08-30)**: contrariamente a quanto scritto qui in precedenza, i
   log diagnostici dell'utente mostrano `createFileStream=true` su file
   `.mp3` reali (es. `fart-noises-83359.mp3`) — la libsndfile installata
   sul suo sistema (Arch Linux) evidentemente supporta la lettura MP3
   (funzionalità opzionale disponibile da libsndfile 1.1+). Non è quindi
   un blocco universale; resta possibile che non tutti i sistemi/versioni
   di libsndfile abbiano questo supporto compilato, nel qual caso
   `createFileStream` fallisce con "Impossibile aprire il file audio" e va
   valutato libavformat/ffmpeg come fallback.
2. **Dialog dispositivi Bluetooth**: il dialog "Aggiungi output Bluetooth"
   in `Main.qml` è un placeholder testuale, non ancora collegato a
   `blueZManager.devices()`.
3. **`BlueZManager::refreshDevices()`**: stub, non fa ancora il parsing di
   `org.freedesktop.DBus.ObjectManager.GetManagedObjects()` su
   `org.bluez`. Serve per popolare la lista dispositivi accoppiati.
4. ~~`createVirtualSink` non traccia il `pw_impl_module*`~~ — **risolto
   2026-09-02** (necessario per i sink di cattura del punto 29): ora
   tracciato in `Impl::virtualSinks` (correlato per nome, come i file
   stream), `removeVirtualSink` distrugge davvero il modulo, e
   `ownedVirtualSinkIds`/`Kind::VirtualSink` vengono popolati per davvero
   (prima il controllo era per nodeId, impossibile da soddisfare in tempo
   utile). Resta comunque vero che nessun sink virtuale creato con questa
   funzione compare ancora come voce scelta dall'utente in colonna
   Output — l'unico uso attuale è interno (sink di cattura per le cue
   "sorgente app").
5. **Abbinamento canali per ordine di scoperta** (vedi punto 2 sopra): da
   rendere robusto leggendo `PW_KEY_AUDIO_CHANNEL` dalla porta invece di
   fare affidamento sull'ordine di arrivo delle callback.
6. **Diverse feature UI non ancora verificate a fondo dall'utente
   (2026-08-30)**: trascinamento cavi (punto 7), toggle keepalive nel menu
   Impostazioni (punto 10), salva/apri progetto e menu recenti (punto 11),
   "Scollega tutto" e Stop/Esc (punto 12). Tutte compilano e l'app si avvia
   senza warning QML, ma l'ambiente di sviluppo usato in queste sessioni
   non ha un window manager reale: l'automazione UI (xdotool + screenshot)
   ha dato risultati inconsistenti (focus/eventi tastiera non sempre
   recapitati, un trascinamento ha prodotto 3 cavi invece di 1 una sola
   volta, non riproducibile) — probabilmente artefatti dell'ambiente più
   che bug, ma non e' stato possibile escluderlo con certezza in modo
   automatico. **L'utente ha chiesto di occuparsi lui stesso dei test UI
   interattivi** da qui in avanti; se durante l'uso reale emergono
   problemi (in particolare sul trascinamento, che è l'unico punto con un
   comportamento anomalo osservato anche se non riproducibile), vanno
   segnalati per un fix mirato.
7. **Input microfono**: `addMicrophoneInput`/`m_inputs` (il `PortModel`
   generico) esistono ancora ma non sono più collegati a nessuna UI da
   quando la colonna Input è diventata la playlist (punto 8 sopra) — vanno
   ripensati insieme al supporto microfono quando verrà implementato,
   probabilmente con una UI separata dalla playlist file.
8. **Effetti audio nel pannello "Trasforma" (richiesto esplicitamente come
   solo-TODO, 2026-08-30)**: l'utente ha chiesto equalizzatore/effetti come
   possibile estensione futura del pannello "Trasforma" (punto 25 sopra),
   ma ha chiesto esplicitamente di NON implementarli ora, solo di
   annotarli. Andrebbero probabilmente innestati come catena di filtri SPA
   (`spa_process_audio`/plugin LADSPA via PipeWire) tra la lettura del
   buffer in memoria e `pw_stream_queue_buffer` in
   `PipeWireEngine::Impl::onFileStreamProcess` — non progettato nel
   dettaglio.
9. **Caricamento file in memoria per intero (punto 25 sopra) — costo per
   file molto lunghi**: `createFileStream` ora decodifica l'intero file
   prima di avviare lo stream (necessario per reverse/loop), bloccante sul
   thread Qt e proporzionale in RAM alla durata del file. Per la musica di
   sottofondo di una zona (tipicamente minuti) è trascurabile; se in futuro
   servissero file molto lunghi (ore) andrebbe rivista con una variante a
   streaming con buffer circolare invece del caricamento completo.
10. ~~Catturare stream audio di altre app (es. Firefox) come sorgente in
    playlist~~ — **implementato, vedi punto 29** più sopra
    ("sposta", non "duplica", con presa di controllo diretta del link
    invece della sola metadata `target.object`, rivelatasi inaffidabile in
    più di un caso). Non ancora testato dall'utente nella UI reale.

## Bug risolti in questa chat (per riferimento, se riappaiono pattern simili)

- `Q_INVOKABLE` mancante su metodi pubblici richiamati da QML → sempre
  necessario per ogni metodo C++ chiamato da QML.
- Errore moc "Meta Types must be fully defined" → causato da
  `Q_PROPERTY(PortModel *...)` con `PortModel` solo forward-dichiarata;
  serve l'include completo quando un tipo compare in `Q_PROPERTY`.
- Stesso errore mascherato da **path di include incoerenti**
  (`"../models/PortModel.h"` vs `"models/PortModel.h"`): con
  `target_include_directories` puntato a `src/`, usare SEMPRE path
  relativi a `src/` in ogni `#include` locale, mai `../`.
- `pw_impl_module`/`pw_context_load_module` non dichiarati → serve
  `#include <pipewire/impl.h>` oltre a `<pipewire/pipewire.h>`.
- Mismatch di tipo `int` vs `qsizetype` in `std::min`/`std::max` con
  `QVector::size()` → castare esplicitamente a `int` quando si mescolano
  contatori `int` con `.size()` di container Qt.
- Errore QML "Value is null" nel tema Breeze → causato da un
  `property alias` fragile (`addButtonText: addButton.text`) e da
  riferimenti ambigui a ruoli del model nei delegate (es.
  `isBluetooth: isBluetooth`); risolto rendendo esplicito `model.xxx` nei
  delegate e sostituendo l'alias con una proprietà normale.

## Ambiente di build

Arch Linux:
```bash
sudo pacman -S qt6-base qt6-declarative pipewire cmake base-devel
pkg-config --modversion libpipewire-0.3   # verifica che sia visibile
```

Build:
```bash
cd bt-multizone-audio
rm -rf build
cmake -B build
cmake --build build
./build/btmultizone
```

## Prossimo passo naturale

**Priorità immediata (2026-08-30): test interattivi manuali dell'utente.**
Nell'ultima sessione sono state implementate/completate diverse feature UI
(trascinamento cavi, playlist/cue list, toggle keepalive, salva/apri
progetto + recenti, "Scollega tutto", Stop/Esc) verificate solo via
build+avvio senza warning, non tramite uso interattivo reale (l'ambiente di
sviluppo non ha un window manager e l'automazione UI ha dato risultati
inconsistenti — vedi TODO punto 6). L'utente ha chiesto di occuparsene lui
stesso: qualunque problema trovato va segnalato per un fix mirato prima di
considerare queste feature definitivamente concluse.

Dopo quello, con playback file, routing a trascinamento, playlist stile
QLab, discovery nodi e keepalive Bluetooth tutti funzionanti, il prossimo
passo con più impatto resta collegare il dialog dispositivi Bluetooth a
`blueZManager.devices()` (TODO punto 2) e implementare
`BlueZManager::refreshDevices()` (TODO punto 3): senza quello, l'unico modo
per aggiungere un output Bluetooth resta collegare il dispositivo da fuori
l'app. Il meccanismo di aggancio automatico del keepalive è ora verificato
(punto 19: il nodo generatore si collega correttamente a tutti i sink
Bluetooth reali, confermato con `pw-dump`/`pw-link`) — resta da validare
solo l'EFFETTO pratico, cioè se il ping impedisce davvero l'auto-standby
del dispositivo (richiede una cassa reale collegata e inattiva per una
sessione lunga, >20 minuti, oltre il tempo tipico di auto-standby, per
vedere se resta connessa invece di spegnersi/disconnettersi come succedeva
prima di questo fix).
