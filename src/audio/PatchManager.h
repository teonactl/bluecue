#pragma once

#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVector>

#include "AudioNode.h"
#include "models/PortModel.h"

class AudioEngine;
class BluetoothManager;
class QTimer;

// Rappresenta una singola connessione attiva nella griglia di routing:
// un input collegato a un output, con l'id del link PipeWire risultante.
struct PatchConnection
{
    uint32_t inputNodeId = 0;
    uint32_t outputNodeId = 0;
    uint32_t linkId = 0;
};

// Una traccia in coda nella playlist stile QLab/Cuelab: esiste come voce
// della lista fin da quando l'utente la aggiunge, ma diventa un nodo
// PipeWire vero (nodeId != 0) solo mentre è effettivamente in riproduzione.
// Le altre tracce (già riprodotte o non ancora arrivate) non hanno uno
// stream attivo, quindi non possono essere collegate a un output finché non
// tocca a loro.
struct Cue
{
    // Identificatore stabile e univoco (mai riassegnato, indipendente dalla
    // posizione in m_cues), assegnato alla creazione. Necessario perché con
    // più tracce riproducibili in contemporanea (vedi sotto) i callback
    // asincroni di preWait/durata/postWait/nodeId devono poter ritrovare LA
    // cue giusta anche se nel frattempo altre tracce sono state rimosse e
    // gli indici sono cambiati — un semplice indice catturato al momento del
    // trigger non è più affidabile.
    quint64 id = 0;
    QString filePath;
    QString displayName;
    uint32_t nodeId = 0;
    // Nomi stabili (AudioNode::name, es. "alsa_output.pci-..." o
    // "bluez_output.XX:XX...") dei sink verso cui questa traccia deve
    // essere instradata, nell'ordine in cui l'utente li ha collegati (usato
    // come ordine di rotazione da rotateOutputs, vedi sotto). A differenza
    // di nodeId (valido solo mentre la traccia è effettivamente in
    // riproduzione, riassegnato dal nulla ogni volta), questa lista resta
    // valida indipendentemente dallo stato di riproduzione: definisce il
    // routing "voluto" per la traccia, applicato automaticamente ogni volta
    // che diventa quella in riproduzione (vedi
    // PatchManager::applyDesiredConnections). Niente duplicati: trattata
    // come un insieme ordinato, non una vera QList con ripetizioni.
    QVector<QString> desiredOutputNames;

    // --- Modificatore audio (pannello "Trasforma", opzionale) ---

    // -1 = loop continuo. N>0 = la traccia si ferma da sola dopo N
    // ripetizioni (vedi PipeWireEngine::fileStreamFinished). Default 1
    // (riproduzione singola): richiesto esplicitamente dall'utente — il
    // vecchio default -1 risaliva a quando il loop continuo era l'UNICO
    // comportamento possibile (prima del pannello Trasforma, vedi
    // PROJECT_STATUS punto 25), e non riflette più cosa un utente si
    // aspetta da una nuova traccia appena aggiunta.
    int loopCount = 1;
    // true = riproduzione all'indietro (dall'ultimo campione al primo).
    bool reverse = false;
    // true = invece di suonare simultaneamente su tutti gli output in
    // desiredOutputNames, ne tiene collegato uno solo alla volta e passa al
    // successivo (in ordine) ad ogni giro completo della traccia — pensato
    // per far "girare" un sottofondo tra più zone nel tempo invece che
    // diffonderlo ovunque insieme.
    bool rotateOutputs = false;
    // Indice (in desiredOutputNames) dell'output attualmente attivo quando
    // rotateOutputs è true. Non persistito: riparte da 0 ad ogni avvio della
    // traccia.
    int rotateOutputIndex = 0;
    // Quanti giri completi della rotazione (un giro = tornare all'output di
    // partenza dopo aver toccato tutti quelli in desiredOutputNames) fare
    // prima di considerare la traccia finita e passare automaticamente a
    // quella successiva in coda — stesso spirito di loopCount ma per la
    // rotazione output invece che per le ripetizioni del file. -1 = infinito
    // (default, comportamento storico: la rotazione avanza solo a comando,
    // via barra spaziatrice, senza mai fermarsi da sola).
    int rotationCycleCount = -1;
    // Contatore runtime dei giri completati durante la riproduzione
    // corrente, confrontato con rotationCycleCount ad ogni giro chiuso
    // (vedi PatchManager::advanceOutputRotation). Non persistito: riparte da
    // 0 ad ogni avvio della traccia, come rotateOutputIndex.
    int rotationCyclesCompleted = 0;

    // --- Timing stile QLab (pre wait / durata / post wait) ---
    // Editabili col tasto destro sulla riga della playlist (modal di
    // configurazione in Main.qml). Persistiti nel file di progetto; 0 in
    // tutti e tre = comportamento storico (parte subito, dura finché i loop
    // non finiscono o viene fermata a mano, si scollega immediatamente alla
    // fine).

    // Ritardo (secondi) tra il trigger (playCueAt) e l'inizio VERO
    // dell'audio: durante l'attesa la traccia è "in armo esteso"
    // (Cue::waitingToStart), nodeId resta 0.
    double preWaitSeconds = 0.0;
    // Se > 0, la traccia si ferma da sola (durata massima, secondi) dopo
    // essere partita, indipendentemente da loopCount/reverse — utile per
    // "questo sottofondo in loop infinito dura esattamente 5 minuti" invece
    // di calcolare a mano quanti loop servono. 0 = nessun limite (usa
    // loopCount/la lunghezza naturale del file, comportamento storico).
    double durationSeconds = 0.0;
    // Dopo la fine "naturale" (durationSeconds scaduta, o loopCount
    // esaurito — MAI dopo uno stop manuale, quello resta immediato), quanti
    // secondi il nodo/i collegamenti restano vivi ma silenziosi prima dello
    // smontaggio vero (stopCueAt). 0 = smontaggio immediato (comportamento
    // storico).
    double postWaitSeconds = 0.0;

    // --- Stato di trasporto runtime (mai persistito) ---
    quint64 playbackGeneration = 0; // incrementato ad ogni trigger (playCueAt): invalida i timer di un avvio precedente della stessa cue
    bool waitingToStart = false;    // true durante preWaitSeconds, prima che l'audio inizi davvero
    bool ended = false;             // true dopo la fine naturale (durata/loop), prima/durante il post wait
    bool inPostWait = false;        // true mentre si attende postWaitSeconds prima dello smontaggio vero
    bool paused = false;            // sostituisce il vecchio flag globale: ora ogni cue si mette in pausa indipendentemente
    QString pendingStreamName;      // nome dello stream file in attesa che PipeWire assegni il nodeId (correlazione robusta con nodeAdded, vedi costruttore)

    // --- Sorgente "app" (Firefox, ecc.) invece di un file — vedi
    // PatchManager::addAppStreamCue. filePath resta vuoto per queste cue:
    // non c'è nulla da decodificare, l'audio arriva già pronto da
    // un'applicazione in esecuzione, "spostato" nella patch bay tramite un
    // sink virtuale di cattura invece che duplicato. loopCount/reverse non
    // hanno alcun effetto per queste cue (nessuno stream file su cui
    // agire), ma restano innocui se toccati dalla UI (i relativi metodi
    // dell'engine no-oppano silenziosamente su un nodeId che non è un file
    // stream). preWait/durata/postWait invece si applicano regolarmente,
    // esattamente come per una cue file: sono generici, non dipendono dal
    // tipo di sorgente.
    bool isAppStream = false;
    // nodeId dello stream applicativo (Kind::AppStream) che questa cue sta
    // catturando — stabile per tutta la durata della cattura (a differenza
    // di nodeId, che è quello del NOSTRO sink di cattura, la "sorgente" da
    // cui si collegano gli output).
    uint32_t appStreamNodeId = 0;
    // PID del processo che possiede appStreamNodeId (AudioNode::
    // appProcessId), copiato al momento di addAppStreamCue — usato per
    // "seguire" l'app quando il suo stream sparisce e ne appare uno nuovo
    // dallo stesso processo (vedi il commento in nodeRemoved/
    // handleAppStreamNode): comune con client pipewire-pulse (es. Firefox),
    // che possono ricreare il proprio stream più volte durante una
    // riproduzione continua e ininterrotta dal punto di vista dell'utente.
    // 0 = sconosciuto (il client non lo espone): in quel caso non è
    // possibile alcun aggancio automatico, la cattura si ferma e basta se
    // lo stream sparisce.
    uint32_t appProcessId = 0;
    // Nome del sink virtuale di cattura creato per questa cue
    // ("bluecue.appcapture.<appStreamNodeId>", vedi
    // PatchManager::beginAppStreamCapture) — usato per correlare il nodeId
    // una volta scoperto (captureSinkNodeId resta 0 fino ad allora, stesso
    // principio di pendingStreamName) e per rimuovere il nome da
    // m_appCaptureSinkNames alla fine della cattura.
    QString captureSinkName;
    // nodeId del sink di cattura una volta noto — identico a nodeId (che
    // resta il campo generico usato da tutto il resto del routing), tenuto
    // qui separatamente solo per poter chiamare
    // PipeWireEngine::removeVirtualSink alla fine senza ambiguità.
    uint32_t captureSinkNodeId = 0;
    // Nome tecnico stabile (AudioNode::name, es. "Firefox") dello stream
    // scelto al momento di addAppStreamCue — PERSISTITO nel file di
    // progetto (a differenza di appStreamNodeId, un id di sessione).
    // Usato per ri-risolvere a un nodeId live al momento del Play se la
    // cue è stata ricaricata da un progetto (appStreamNodeId parte da 0 in
    // quel caso) — vedi PatchManager::startCueNow. Ambiguo se più stream
    // dello stesso nome sono attivi insieme (es. due finestre Firefox):
    // limite accettato, nessun modo migliore di distinguerli con le sole
    // proprietà che PipeWire espone per uno stream applicativo.
    QString appStreamMatchName;
};

// Orchestratore della patch bay: possiede i due PortModel (input/output)
// esposti a QML e traduce le azioni dell'utente (aggiungi input, aggiungi
// output Bluetooth, collega input a output) in chiamate a PipeWireEngine
// e BlueZManager.
class PatchManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(PortModel *inputs READ inputs CONSTANT)
    Q_PROPERTY(PortModel *outputs READ outputs CONSTANT)
    Q_PROPERTY(QVariantList connectionsModel READ connectionsModel NOTIFY connectionsChanged)
    Q_PROPERTY(QVariantList cueModel READ cueModel NOTIFY cuesChanged)
    Q_PROPERTY(int armedCueIndex READ armedCueIndex NOTIFY cuesChanged)
    Q_PROPERTY(QStringList recentProjects READ recentProjects NOTIFY recentProjectsChanged)
    // Percorso del progetto correntemente aperto/salvato, vuoto se nessuno
    // (progetto nuovo mai salvato) — usato dalla UI per decidere se Ctrl+S
    // deve salvare direttamente qui o aprire "Salva con nome" la prima volta.
    Q_PROPERTY(QString currentProjectPath READ currentProjectPath NOTIFY currentProjectPathChanged)
    Q_PROPERTY(bool keepAliveEnabled READ keepAliveEnabled WRITE setKeepAliveEnabled NOTIFY keepAliveEnabledChanged)
    Q_PROPERTY(uint identifyingSinkId READ identifyingSinkId NOTIFY identifyingSinkIdChanged)
    // Stream di riproduzione di altre applicazioni attualmente in
    // esecuzione (Firefox, VLC, ecc. — Kind::AppStream), selezionabili per
    // "spostarli" nella patch bay come una cue (vedi addAppStreamCue). Non
    // include mai i nostri stessi sink di cattura (filtrati per nome).
    Q_PROPERTY(QVariantList appStreamsModel READ appStreamsModel NOTIFY appStreamsChanged)
    // Microfoni hardware disponibili (per calibrateOutputDelay), come
    // QVariantList di {nodeId, description}.
    Q_PROPERTY(QVariantList microphonesModel READ microphonesModel NOTIFY microphonesChanged)
    // true mentre una calibrazione (vedi calibrateOutputDelay) è in corso
    // — la UI la usa per disabilitare un nuovo avvio e mostrare un
    // indicatore di progresso durante i ~2.3s della misura.
    Q_PROPERTY(bool calibrationInProgress READ calibrationInProgress NOTIFY calibrationStateChanged)
    // Parametri del ping keepalive, regolabili dal menu Impostazioni e
    // persistiti tra un avvio e l'altro — richiesto esplicitamente
    // dall'utente per poter sperimentare durata/ampiezza/frequenza senza
    // dover ricompilare, incluso provare una frequenza ultrasonica
    // (inaudibile). Ampiezza espressa come intero 0-100000 (centomillesimi
    // di fondo scala) invece di un float 0.0-1.0 per restare compatibile
    // con un semplice SpinBox QML — un float qui richiederebbe un
    // validator/textFromValue custom solo per la granularità.
    Q_PROPERTY(int keepAlivePingFrequencyHz READ keepAlivePingFrequencyHz WRITE setKeepAlivePingFrequencyHz NOTIFY keepAlivePingSettingsChanged)
    Q_PROPERTY(int keepAlivePingAmplitudeUnits READ keepAlivePingAmplitudeUnits WRITE setKeepAlivePingAmplitudeUnits NOTIFY keepAlivePingSettingsChanged)
    Q_PROPERTY(int keepAlivePingDurationMs READ keepAlivePingDurationMs WRITE setKeepAlivePingDurationMs NOTIFY keepAlivePingSettingsChanged)
    Q_PROPERTY(int keepAlivePingPeriodSeconds READ keepAlivePingPeriodSeconds WRITE setKeepAlivePingPeriodSeconds NOTIFY keepAlivePingSettingsChanged)

public:
    explicit PatchManager(AudioEngine *engine, BluetoothManager *blueZ, QObject *parent = nullptr);

    PortModel *inputs() const { return m_inputs; }
    PortModel *outputs() const { return m_outputs; }

    // --- Playlist (colonna Input, stile QLab/Cuelab) ---

    // Aggiunge un file alla coda (in fondo alla playlist). Riceve il file
    // già scelto dal file picker QML (QUrl, il tipo restituito da
    // FileDialog.selectedFile). Non avvia subito la riproduzione: resta in
    // coda finché non tocca il suo turno via advanceCue().
    Q_INVOKABLE void addCueFile(const QUrl &fileUrl);

    // Rimuove la traccia in posizione index dalla playlist. Se era in
    // riproduzione (o in attesa di partire durante il suo pre wait), la
    // ferma prima di rimuoverla.
    Q_INVOKABLE void removeCue(int index);

    // Sposta la traccia da fromIndex a toIndex nella playlist (drag-to-
    // reorder in CueList.qml, richiesto esplicitamente dall'utente). Non
    // tocca lo stato di riproduzione di nessuna cue — solo l'ORDINE
    // dell'array. armedIndex segue la stessa cue (per id, non per
    // posizione) se era armata.
    Q_INVOKABLE void moveCue(int fromIndex, int toIndex);

    // Rinomina la traccia in posizione cueIndex (doppio click sul nome in
    // CueList.qml, richiesto esplicitamente dall'utente) — sostituisce il
    // nome file come etichetta mostrata ovunque (playlist, pannello
    // Trasforma). Persistito nel file di progetto.
    Q_INVOKABLE void setCueDisplayName(int cueIndex, const QString &name);

    // Azione della barra spaziatrice. Se una traccia in riproduzione ha
    // rotateOutputs attivo con più di un output collegato, avanza PRIMA la
    // sua rotazione (fa scattare l'output successivo, ciclicamente — vedi
    // le etichette sotto la riga in CueList.qml) invece di avviare la
    // traccia successiva: la rotazione "occupa" la barra spaziatrice finché
    // c'è una traccia così in riproduzione. Solo se nessuna lo è, avvia la
    // traccia "in armo" (armedCueIndex) tramite playCueAt(), SENZA fermare
    // le tracce già in riproduzione — più cue possono suonare insieme.
    Q_INVOKABLE void advanceCue();

    // Sceglie manualmente quale traccia è "in armo" (quella che partirà con
    // Play/barra spaziatrice), senza avviarla subito — usato dal click
    // (non trascinamento) su una riga della playlist, per poter scegliere
    // liberamente quale traccia riprodurre invece di essere vincolati
    // all'ordine sequenziale della coda.
    Q_INVOKABLE void armCue(int index);

    // Avvia la traccia in posizione index (rispettando il suo preWaitSeconds
    // se impostato — l'audio vero parte solo dopo quel ritardo) e arma
    // quella successiva. NON ferma nessun'altra traccia già in riproduzione:
    // più cue possono restare attive in contemporanea, esattamente come in
    // QLab dove avviare una cue non interrompe le altre già partite. No-op
    // se la traccia in index è già in riproduzione o già in attesa di
    // partire (niente doppio trigger).
    Q_INVOKABLE void playCueAt(int index);

    // Aggiunge un input microfono, scegliendo tra le sorgenti hardware
    // rilevate da PipeWire (es. scheda audio USB, mic integrato).
    Q_INVOKABLE void addMicrophoneInput(uint32_t hardwareSourceId);

    // Aggiunge alla playlist una cue che, quando avviata (playCueAt, come
    // una qualunque traccia), "sposta" l'audio dello stream applicativo
    // indicato (appStreamNodeId, uno di quelli in appStreamsModel) nella
    // patch bay: crea un sink virtuale di cattura dedicato e reindirizza lì
    // lo stream con PipeWireEngine::setStreamTarget, invece di lasciarlo
    // sull'uscita di sistema di default — richiesto esplicitamente
    // dall'utente ("vorrei poter aggiungere anche gli stream audio come
    // fonte nella tracklist, firefox o altri stream"), con "sposta" invece
    // di "duplica" scelto esplicitamente (l'alternativa duplicherebbe
    // l'audio anche sull'uscita originale). Non aggiunge subito una cue
    // "live": resta in coda (nodeId 0) finché non viene avviata, esattamente
    // come una traccia file appena aggiunta.
    Q_INVOKABLE void addAppStreamCue(uint32_t appStreamNodeId);

    // --- Progetti (playlist salvata su file, menu File) ---

    // Ferma la traccia in riproduzione (se c'è) e svuota la playlist, senza
    // toccare l'elenco dei progetti recenti.
    Q_INVOKABLE void newProject();

    // Salva la playlist attuale (solo i percorsi file, in ordine — non lo
    // stato di riproduzione né i collegamenti agli output, che dipendono da
    // id PipeWire non stabili tra un avvio e l'altro) nel file scelto dal
    // FileDialog QML. Aggiunge il file ai progetti recenti.
    Q_INVOKABLE bool saveProject(const QUrl &fileUrl);

    // Ferma la riproduzione, svuota la playlist attuale e la ricarica dal
    // file indicato (QUrl, come restituito da FileDialog.selectedFile). I
    // file non più presenti sul disco vengono saltati con un patchError,
    // senza bloccare il caricamento del resto della playlist.
    Q_INVOKABLE bool loadProject(const QUrl &fileUrl);

    // Come loadProject(), ma riceve un path locale già pronto: usato dal
    // menu "Apri recenti", che non passa per un FileDialog e quindi non ha
    // mai una QUrl da convertire.
    Q_INVOKABLE bool loadProjectFromPath(const QString &filePath);

    // "Panic" in stile QLab: ferma immediatamente TUTTE le tracce in
    // riproduzione o in attesa di partire, scollegandole da qualunque
    // output — nessun link orfano. Non tocca la playlist né l'indice
    // armato: una successiva pressione della barra spaziatrice riparte da
    // dove l'utente si aspetta. Pensato per il tasto Esc / un pulsante
    // "Stop" sempre visibile.
    Q_INVOKABLE void stopAllCues();

    // --- Trasporto globale (pulsanti Play/Pausa/Stop, slegati dal
    // click/trascinamento sulle righe della playlist) ---

    // Riprende TUTTE le tracce attualmente in pausa. Se nessuna era in
    // pausa, avvia la traccia in armo (stesso effetto di advanceCue()) SENZA
    // fermare quelle già in riproduzione — più cue possono suonare insieme.
    Q_INVOKABLE void play();

    // Mette in pausa TUTTE le tracce attualmente in riproduzione (non già in
    // pausa, non in pre/post wait) SENZA fermarle: il nodo PipeWire e tutti
    // i loro collegamenti restano vivi (pw_stream_set_active false,
    // silenzio invece di distruzione), quindi il routing verso gli output
    // non va perso. No-op se non sta suonando nulla.
    Q_INVOKABLE void pause();

    // Collega (o scollega, se già collegata) la traccia in posizione
    // cueIndex verso l'output outputNodeId. A differenza del vecchio
    // toggleConnection(nodeId,nodeId) questo funziona anche se la traccia
    // NON è ancora in riproduzione: il routing voluto viene comunque
    // registrato (Cue::desiredOutputNames, per nome stabile del sink) e
    // verrà applicato automaticamente non appena la traccia parte (vedi
    // applyDesiredConnections). Se la traccia è già in riproduzione, il
    // collegamento PipeWire reale viene creato/rimosso subito.
    Q_INVOKABLE void toggleCueOutput(int cueIndex, uint32_t outputNodeId);

    // Rimuove dal routing "voluto" della traccia in posizione cueIndex
    // l'output dal nome stabile outputName, funziona ANCHE se quell'output
    // non è al momento scoperto/live (nodeId 0) — a differenza di
    // toggleCueOutput, che richiede un nodeId live e quindi non permette di
    // rimuovere un output desiderato ma attualmente disconnesso (il cavo
    // corrispondente non viene mai disegnato, quindi non c'è modo di
    // cliccarlo: segnalato dall'utente, "continua ad apparire questo
    // dispositivo che non funziona" per un dispositivo Bluetooth non più
    // connesso). Se l'output risulta comunque live in questo momento e la
    // traccia è in riproduzione, scollega anche il link PipeWire reale.
    Q_INVOKABLE void removeCueDesiredOutputByName(int cueIndex, const QString &outputName);

    // --- Modificatore audio (pannello "Trasforma") ---

    // Imposta quante volte la traccia in posizione cueIndex deve ripetersi
    // (-1 = loop continuo, N>0 = si ferma da sola dopo N ripetizioni). Se la
    // traccia è già in riproduzione l'effetto è immediato (vedi
    // PipeWireEngine::setFileStreamLoopCount); altrimenti si applica al
    // prossimo avvio (playCueAt).
    Q_INVOKABLE void setCueLoopCount(int cueIndex, int loopCount);

    // Attiva/disattiva la riproduzione all'indietro per la traccia in
    // posizione cueIndex. Se già in riproduzione l'effetto è immediato (la
    // direzione cambia dal punto corrente, senza saltare a inizio/fine).
    Q_INVOKABLE void setCueReverse(int cueIndex, bool reverse);

    // Attiva/disattiva la modalità "un output alla volta" per la traccia in
    // posizione cueIndex: vedi Cue::rotateOutputs. Riporta subito il routing
    // live a un solo output attivo (il primo in desiredOutputNames) se la
    // traccia è già in riproduzione e ha più di un output collegato.
    Q_INVOKABLE void setCueRotateOutputs(int cueIndex, bool rotate);

    // Imposta quanti giri completi della rotazione output fare prima di
    // considerare la traccia in posizione cueIndex "finita" e passare da
    // sola alla prossima in coda (-1 = infinito, mai da sola — vedi
    // Cue::rotationCycleCount). Effetto solo sui prossimi giri, non
    // retroattivo su un conteggio già in corso.
    Q_INVOKABLE void setCueRotationCycleCount(int cueIndex, int cycleCount);

    // --- Timing stile QLab (pannello di configurazione a tasto destro) ---

    // Ritardo (secondi, >= 0) tra Play e l'inizio vero dell'audio per la
    // traccia in posizione cueIndex. Effetto solo sui prossimi trigger, non
    // su un'attesa già in corso.
    Q_INVOKABLE void setCuePreWait(int cueIndex, double seconds);

    // Durata massima (secondi, >= 0) della traccia in posizione cueIndex
    // dopo che è partita — 0 = nessun limite. Se la traccia è già in
    // riproduzione l'effetto è immediato (il nuovo timer riparte da adesso,
    // non dall'inizio originale della riproduzione).
    Q_INVOKABLE void setCueDuration(int cueIndex, double seconds);

    // Quanti secondi (>= 0) la traccia in posizione cueIndex resta
    // collegata (silenziosa) dopo la fine naturale prima di scollegarsi del
    // tutto — 0 = smontaggio immediato.
    Q_INVOKABLE void setCuePostWait(int cueIndex, double seconds);

    // Rimuove tutti i collegamenti di routing attivi (qualunque input verso
    // qualunque output), senza toccare la playlist né fermare la
    // riproduzione: la traccia corrente continua a suonare, semplicemente
    // scollegata da tutte le uscite finché l'utente non la ricollega.
    Q_INVOKABLE void clearAllConnections();

    // --- Annulla/ripeti (richiesto esplicitamente dall'utente) ---
    //
    // Copre le operazioni STRUTTURALI sulla playlist (aggiungi/rimuovi/
    // sposta/rinomina traccia, collega/scollega un output "voluto") — non
    // le proprietà minori (loop/reverse/wait/rotazione) né lo stato di
    // riproduzione live, che restano legate ai nodi PipeWire reali e non
    // hanno senso "annullate" a posteriori. Ogni snapshot cattura solo i
    // campi persistiti di ogni cue (gli stessi salvati nel file di
    // progetto) più l'ordine e l'id stabile — mai lo stato di riproduzione
    // (nodeId, pausa, ecc.), che viene preservato per le cue ancora
    // presenti dopo un annulla/ripeti invece di essere sovrascritto.
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY undoRedoAvailabilityChanged)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY undoRedoAvailabilityChanged)
    bool canUndo() const { return !m_undoStack.isEmpty(); }
    bool canRedo() const { return !m_redoStack.isEmpty(); }
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

    QStringList recentProjects() const { return m_recentProjects; }
    QString currentProjectPath() const { return m_currentProjectPath; }

    // Salva sul percorso attualmente noto (currentProjectPath) senza
    // chiedere nulla — usata da Ctrl+S. Se non c'è ancora un percorso noto
    // (progetto nuovo mai salvato), non fa nulla e ritorna false: la UI
    // deve allora aprire "Salva con nome" (stesso comportamento del "Salva"
    // di praticamente ogni editor).
    Q_INVOKABLE bool saveProjectToCurrentPath();

    // --- Impostazioni ---

    // Se true (default), ogni sink Bluetooth in colonna Output riceve
    // automaticamente il ping periodico di PipeWireEngine::setKeepAliveEnabled
    // per non andare in stand-by per inattività. Disattivabile dal menu
    // Impostazioni per chi preferisce lasciare che le casse si spengano da
    // sole. Persistito tra un avvio e l'altro (QSettings).
    bool keepAliveEnabled() const { return m_keepAliveSettingEnabled; }
    void setKeepAliveEnabled(bool enabled);

    int keepAlivePingFrequencyHz() const { return m_keepAlivePingFrequencyHz; }
    void setKeepAlivePingFrequencyHz(int hz);
    int keepAlivePingAmplitudeUnits() const { return m_keepAlivePingAmplitudeUnits; }
    void setKeepAlivePingAmplitudeUnits(int units);
    int keepAlivePingDurationMs() const { return m_keepAlivePingDurationMs; }
    void setKeepAlivePingDurationMs(int ms);
    int keepAlivePingPeriodSeconds() const { return m_keepAlivePingPeriodSeconds; }
    void setKeepAlivePingPeriodSeconds(int seconds);

    // --- Colonna Output ---

    // Aggiunge l'uscita jack del computer (sink hardware di default o scelto).
    Q_INVOKABLE void addJackOutput(uint32_t hardwareSinkId);

    // Avvia la connessione a un dispositivo Bluetooth già accoppiato e,
    // una volta connesso, lo espone come nodo output nella griglia.
    Q_INVOKABLE void addBluetoothOutput(const QString &deviceObjectPath);

    Q_INVOKABLE void removeOutput(uint32_t nodeId);

    // Riproduce due brevi bip chiaramente udibili sull'output indicato, per
    // capire fisicamente a quale altoparlante corrisponde una voce della
    // colonna Output. Mentre suona, identifyingSinkId riporta nodeId (poi
    // torna a 0 da solo), così la UI può evidenziare quale pulsante è
    // stato premuto.
    Q_INVOKABLE void identifySink(uint32_t nodeId);
    uint32_t identifyingSinkId() const { return m_identifyingSinkId; }

    // Muta/smuta il sink Output indicato. Sostituisce un precedente
    // setOutputVolume (slider) rimosso su richiesta esplicita dell'utente:
    // il volume software del nodo agisce solo dentro il range già
    // limitato a monte dal mixer di sistema (non controllabile da qui,
    // confermato dall'utente confrontando con le impostazioni di sistema),
    // quindi un vero controllo del volume da qui non è affidabile — il
    // muto è un interruttore netto, non soggetto allo stesso problema. Lo
    // stato mostrato in colonna Output (AudioNode::muted, esposto da
    // PortModel) arriva per vie separate dallo stesso PipeWireEngine
    // (SPA_PROP_mute), quindi si aggiorna da solo poco dopo la chiamata —
    // non serve applicarlo qui a mano.
    Q_INVOKABLE void setOutputMuted(uint32_t nodeId, bool muted);

    // Assegna un nome personalizzato (nickname) a un sink Output, mostrato
    // al posto della descrizione PipeWire grezza sia nella riga della
    // colonna Output sia nelle etichette di rotazione/riepilogo del
    // pannello Trasforma (desiderata dall'utente: doppio click sul nome del
    // sink → rinomina, riflessa ovunque). Chiave di persistenza il nome
    // stabile del nodo (AudioNode::name), non nodeId (non stabile tra un
    // avvio e l'altro) — persistito via QSettings, non nel file di
    // progetto: è una preferenza legata al dispositivo fisico, non a una
    // playlist. Nickname vuoto = torna alla descrizione PipeWire originale.
    Q_INVOKABLE void setOutputNickname(uint32_t nodeId, const QString &nickname);

    // Imposta il ritardo (millisecondi, 0 = nessuno) applicato all'audio
    // instradato verso questo sink Output — richiesto esplicitamente
    // dall'utente per compensare la latenza maggiore di un output
    // Bluetooth (trasporto+decodifica A2DP) quando la stessa traccia suona
    // contemporaneamente sull'audio interno e su una cassa BT: ritardando
    // l'output più veloce (tipicamente quello interno) i due tornano in
    // sincrono. Persistito via QSettings per nome stabile del sink (come
    // setOutputNickname), non nel file di progetto: è una caratteristica
    // del dispositivo fisico (la sua catena audio interna), non della
    // playlist. Applicato subito al motore (PipeWireEngine::
    // setOutputDelayMs); sui backend che non lo supportano ancora
    // (CoreAudioEngine/WasapiEngine) il motore stesso segnala l'errore via
    // engineError.
    Q_INVOKABLE void setOutputDelayMs(uint32_t nodeId, int delayMs);

    // Misura acusticamente la differenza di latenza reale tra due output
    // (richiesto esplicitamente dall'utente: non riusciva a trovare a
    // orecchio il valore giusto dalla UI manuale) e applica da sola il
    // ritardo risultante — vedi AudioEngine::calibrateOutputDelay per il
    // meccanismo (un click di test in sequenza su ciascun output,
    // registrato con micNodeId). Il risultato arriva in modo asincrono
    // (~2.3s) tramite calibrationResult.
    Q_INVOKABLE void calibrateOutputDelay(uint32_t nodeIdA, uint32_t nodeIdB, uint32_t micNodeId);

    bool calibrationInProgress() const { return m_calibrationInProgress; }

    // Microfoni hardware attualmente scoperti, come QVariantList di
    // {nodeId, description} — per il picker di calibrateOutputDelay.
    QVariantList microphonesModel() const;

    // --- Routing ---

    // Crea (o rimuove, se già esistente) la connessione tra un input e un
    // output: pensato per essere chiamato al click su una cella della griglia.
    Q_INVOKABLE void toggleConnection(uint32_t inputNodeId, uint32_t outputNodeId);

    Q_INVOKABLE bool isConnected(uint32_t inputNodeId, uint32_t outputNodeId) const;

    QVector<PatchConnection> connections() const { return m_connections; }

    // Stessa lista di connections(), ma come QVariantList di {inputNodeId,
    // outputNodeId, linkId} in modo che QML possa leggerla e reagire a
    // connectionsChanged() — usata per disegnare i cavi nella patch bay.
    QVariantList connectionsModel() const;

    // Playlist come QVariantList di {displayName, nodeId, ...} (nodeId = 0
    // se non in riproduzione), più gli indici di stato per evidenziare la
    // UI. "In riproduzione" ora è per-cue (nodeId > 0), non più un indice
    // singolo: più righe possono avere nodeId > 0 in contemporanea.
    QVariantList cueModel() const;
    int armedCueIndex() const { return m_armedIndex; }

    // Stream applicativi attualmente selezionabili, come QVariantList di
    // {nodeId, description} — vedi addAppStreamCue.
    QVariantList appStreamsModel() const;

signals:
    void connectionsChanged();
    void cuesChanged();
    void recentProjectsChanged();
    void currentProjectPathChanged();
    void undoRedoAvailabilityChanged();
    void keepAliveEnabledChanged();
    void keepAlivePingSettingsChanged();
    void identifyingSinkIdChanged();
    void appStreamsChanged();
    void microphonesChanged();
    void calibrationStateChanged();
    // Esito finale (successo o fallimento con motivo) di
    // calibrateOutputDelay, per un messaggio nella UI. Il valore già
    // applicato/persistito ai due sink coinvolti viene ripetuto qui
    // (nodeIdA/delayMsA, nodeIdB/delayMsB) così il dialog di ritardo può
    // aggiornare da solo lo SpinBox del sink che ha aperto, senza dover
    // rileggere il modello degli output.
    void calibrationResult(bool success, const QString &message,
                            uint32_t nodeIdA, int delayMsA,
                            uint32_t nodeIdB, int delayMsB);
    void patchError(const QString &message);

private:
    void stopCueAt(int index);
    // Avvia la traccia in armo (m_armedIndex) SENZA fermare le altre già in
    // riproduzione — corpo storico della coda finale di advanceCue(),
    // estratto per essere riusabile anche da advanceOutputRotation() quando
    // Cue::rotationCycleCount viene raggiunto (auto-avanzamento richiesto
    // esplicitamente dall'utente per la modalità rotazione).
    void playArmedCue();
    void addToRecentProjects(const QString &filePath);
    // Scrittura vera e propria del file di progetto — corpo storico di
    // saveProject(QUrl), estratto per essere riusabile anche da
    // saveProjectToCurrentPath() (Ctrl+S) senza passare da una QUrl.
    bool writeProjectToPath(const QString &filePath);
    // Smista un nodo Sink/VirtualSink in colonna Output e, se è Bluetooth
    // (isBluetooth può diventare true solo con un evento successivo al
    // primo nodeAdded — vedi il commento sul segnale nodeUpdated nel
    // costruttore), lo aggancia al keepalive. Condiviso dagli handler di
    // nodeAdded e nodeUpdated per evitare di duplicare questa logica.
    void handleSinkNode(const AudioNode &node);
    // Traccia un nodo Kind::AppStream appena scoperto/scomparso in
    // m_availableAppStreams (escludendo i nostri stessi sink di cattura,
    // riconosciuti per nome — vedi kAppCaptureSinkPrefix nel .cpp), per
    // popolare il picker di addAppStreamCue.
    void handleAppStreamNode(const AudioNode &node);
    // Cerca, tra gli stream applicativi attualmente scoperti
    // (m_availableAppStreams), uno con questo appProcessId diverso da
    // excludeNodeId — usato per "seguire" un'app quando il suo stream
    // sparisce e ne appare uno nuovo dallo stesso processo (vedi il
    // commento su Cue::appProcessId). 0 se nessun sostituto disponibile
    // (appProcessId 0 non trova mai nulla: nessun aggancio automatico
    // possibile senza un PID noto).
    uint32_t findReplacementAppStreamNodeId(uint32_t appProcessId, uint32_t excludeNodeId) const;
    // Avvia davvero la cattura di una cue "sorgente app" (startCueNow la
    // chiama al posto di createFileStream quando Cue::isAppStream):
    // crea il sink virtuale di cattura dedicato; la correlazione del suo
    // nodeId (e il conseguente setStreamTarget) avviene in modo asincrono
    // nel gestore di nodeAdded, stesso schema di pendingStreamName per le
    // cue file.
    void beginAppStreamCapture(int index);
    // Muta/riattiva la cue in riproduzione (play/pause globali,
    // handleCueNaturalEnd): per una cue file richiama
    // PipeWireEngine::setFileStreamActive, per una cue "sorgente app"
    // reindirizza/rimuove il reindirizzamento dello stream applicativo
    // (silenzio ottenuto smettendo di alimentare il sink di cattura,
    // invece di una vera pausa — non esiste un "pausa" per uno stream che
    // non controlliamo noi).
    void setCueLiveActive(Cue &cue, bool active);
    // Collega la traccia cueIndex (deve essere già in riproduzione, nodeId
    // > 0) agli output "voluti": tutti quelli in Cue::desiredOutputNames
    // normalmente, oppure solo quello in rotateOutputIndex se
    // Cue::rotateOutputs è true — chiamato non appena PipeWire assegna il
    // nodeId a una traccia appena avviata (vedi il costruttore).
    void applyDesiredConnections(int cueIndex);

    // Scollega l'output attivo corrente (rotateOutputIndex) e collega quello
    // successivo in Cue::desiredOutputNames, ciclicamente — chiamato ad ogni
    // PipeWireEngine::fileStreamLooped ricevuto per la traccia in
    // riproduzione, solo se ha rotateOutputs abilitato e più di un output.
    void advanceOutputRotation(int cueIndex);
    // Risincronizza il routing live di una traccia in rotazione: scollega
    // ogni output "voluto" tranne quello in rotateOutputIndex, poi collega
    // quest'ultimo se non lo è già — usata sia da advanceOutputRotation sia
    // da toggleCueOutput/applyDesiredConnections quando rotateOutputs è
    // attivo, per non lasciare mai più di un output collegato insieme.
    void resyncRotationConnection(int cueIndex);

    // Avvia DAVVERO la riproduzione (createFileStream) della traccia in
    // posizione index, saltando l'eventuale pre wait — chiamata subito da
    // playCueAt se preWaitSeconds è 0, o dal timer di preWaitSeconds
    // altrimenti. No-op se index non è più valido (cue rimossa nel
    // frattempo, capita tramite l'id stabile, non un indice catturato).
    void startCueNow(int index);
    // Gestisce la fine "naturale" della traccia in posizione index (durata
    // massima scaduta, o PipeWireEngine::fileStreamFinished per loopCount
    // esaurito): silenzia lo stream, poi — se postWaitSeconds > 0 — aspetta
    // prima di scollegare davvero (stopCueAt), altrimenti scollega subito.
    // Idempotente (Cue::ended) per non essere eseguita due volte se
    // durata/loop scadono insieme.
    void handleCueNaturalEnd(int index);
    // Trova l'indice corrente in m_cues della cue con questo id stabile, o
    // -1 se non esiste più (rimossa) — usato da tutti i callback asincroni
    // (preWait/durata/postWait) per non fidarsi di un indice catturato
    // prima dell'attesa, che potrebbe essere cambiato o non essere più
    // valido nel frattempo.
    int findCueIndexById(quint64 id) const;

    // Solo i campi persistiti di una cue (gli stessi del file di
    // progetto) più l'id stabile — MAI lo stato di riproduzione live
    // (nodeId, pausa, waitingToStart...), che per una cue ancora presente
    // dopo un annulla/ripeti va preservato com'è, non sovrascritto da
    // uno snapshot preso in un momento diverso.
    struct CueSnapshotEntry
    {
        quint64 id = 0;
        QString filePath;
        QString displayName;
        QVector<QString> desiredOutputNames;
        int loopCount = 1;
        bool reverse = false;
        bool rotateOutputs = false;
        int rotationCycleCount = -1;
        double preWaitSeconds = 0.0;
        double durationSeconds = 0.0;
        double postWaitSeconds = 0.0;
        bool isAppStream = false;
        uint32_t appStreamNodeId = 0;
        QString appStreamMatchName;
    };
    QVector<CueSnapshotEntry> snapshotCues() const;
    // Riconcilia m_cues con lo snapshot: le cue ancora presenti (stesso id)
    // mantengono il loro stato di riproduzione live, quelle sparite dallo
    // snapshot vengono fermate e rimosse, quelle nello snapshot ma non più
    // in m_cues vengono ricreate (senza riproduzione, ovviamente: nessuno
    // stream PipeWire può essere "resuscitato"). Riapplica il routing
    // voluto delle cue live rimaste.
    void restoreCueSnapshot(const QVector<CueSnapshotEntry> &snapshot);
    // Da chiamare PRIMA di ogni mutazione strutturale della playlist
    // (aggiungi/rimuovi/sposta/rinomina/tocca l'output voluto) — salva lo
    // stato ATTUALE (pre-mutazione) in cima alla pila undo e svuota quella
    // redo (una nuova azione invalida sempre i "ripeti" precedenti).
    void pushUndoSnapshot();

    AudioEngine *m_engine;
    BluetoothManager *m_blueZ;
    PortModel *m_inputs;
    PortModel *m_outputs;
    QVector<PatchConnection> m_connections;
    QStringList m_recentProjects;
    QString m_currentProjectPath;
    bool m_keepAliveSettingEnabled = true;
    // Default coerenti con quelli di PipeWireEngine::Impl (18kHz, 0.15%,
    // 60ms, ogni 20s) — vedi il commento lì per il ragionamento dietro ai
    // valori.
    int m_keepAlivePingFrequencyHz = 18000;
    int m_keepAlivePingAmplitudeUnits = 150; // /100000 = 0.0015 lineare
    int m_keepAlivePingDurationMs = 60;
    int m_keepAlivePingPeriodSeconds = 20;
    QSet<uint32_t> m_bluetoothOutputNodeIds;
    uint32_t m_identifyingSinkId = 0; // 0 = nessuno in test
    // nodeId live -> nome stabile, per ogni sink attualmente scoperto:
    // usata sia per registrare il routing voluto per nome (toggleCueOutput)
    // sia per ritrovare il nodeId corrente di ogni nome quando si applica
    // il routing voluto di una traccia appena avviata.
    QMap<uint32_t, QString> m_outputNodeNames;
    // nodeId live -> descrizione leggibile (AudioNode::description, es.
    // "JBL Xtreme 3" invece del nome interno PipeWire): usata solo per la
    // UI (etichette di rotazione sotto una traccia, riepilogo nel pannello
    // Trasforma), mai per il routing/la persistenza (quello resta per nome
    // stabile, vedi sopra).
    QMap<uint32_t, QString> m_outputNodeDescriptions;
    // Nome stabile (AudioNode::name) -> nickname assegnato dall'utente
    // (doppio click sul nome in colonna Output), sovrascrive
    // m_outputNodeDescriptions ovunque un output venga mostrato in UI. Per
    // nome, non nodeId: sopravvive alla disconnessione/riconnessione del
    // dispositivo. Persistito via QSettings (non nel file di progetto: è
    // legato al dispositivo fisico, non a una playlist).
    QMap<QString, QString> m_outputNicknames;
    // Nome stabile -> ritardo (ms) impostato dall'utente per questo sink
    // (vedi setOutputDelayMs), stesso schema di persistenza per nome di
    // m_outputNicknames. Solo le voci >0 sono davvero utili (assenza dalla
    // mappa equivale a 0/nessun ritardo), ma non si toglie una voce
    // riportata a 0 esplicitamente: serve comunque a distinguere "utente ha
    // scelto 0" da "mai impostato", innocuo in entrambi i casi.
    QMap<QString, int> m_outputDelaysMs;
    // Stream applicativi (Kind::AppStream) attualmente scoperti,
    // selezionabili da addAppStreamCue — esposti a QML via appStreamsModel.
    QVector<AudioNode> m_availableAppStreams;
    // Sorgenti hardware (microfono/line-in, Kind::Source con nome che NON
    // inizia per "bluecue." — quel prefisso è sempre nostro: file stream,
    // stream di calibrazione, ecc., mai un dispositivo reale) —
    // selezionabili in calibrateOutputDelay per registrare il test
    // acustico.
    QVector<AudioNode> m_availableMicrophones;
    bool m_calibrationInProgress = false;
    // Nomi dei sink virtuali di cattura creati da noi (uno per cue
    // "sorgente app" attiva) — usato per impedire che vengano MAI trattati
    // come un output normale (colonna Output) quando li scopriamo tramite
    // il discovery generico di PipeWire, che non sa distinguerli da un
    // qualunque altro sink Audio/Sink.
    QSet<QString> m_appCaptureSinkNames;
    // Riafferma periodicamente il reindirizzamento (setStreamTarget) di
    // ogni cue "sorgente app" attualmente in cattura — auto-recovery
    // richiesta esplicitamente dall'utente: il target impostato sulla
    // metadata "default" di PipeWire è un suggerimento applicato alla
    // prossima rivalutazione del routing da parte del session manager, non
    // una garanzia permanente, quindi riproporlo di tanto in tanto è più
    // robusto che fidarsi ciecamente di un singolo tentativo iniziale.
    QTimer *m_appStreamReassertTimer;
    // Nome stabile -> ultima descrizione PipeWire leggibile vista per quel
    // dispositivo, MAI rimossa quando il nodo scompare (a differenza di
    // m_outputNodeDescriptions, svuotata da nodeRemoved): una cassa
    // Bluetooth può disconnettersi/riconnettersi spesso (per questo esiste
    // il keepalive), e senza questa cache il routing "voluto" di una
    // traccia mostrava un nome tecnico grezzo (es.
    // "bluez_output.9C_49_52_F8_9B_28.1") ogni volta che il dispositivo non
    // era al momento connesso — segnalato dall'utente con uno screenshot.
    QMap<QString, QString> m_lastKnownOutputDescriptionByName;
    // nodeId live -> indirizzo MAC Bluetooth (AudioNode::bluetoothMac,
    // popolato da PipeWireEngine da api.bluez5.address), solo per i sink
    // Bluetooth — usato per correlare un output con la percentuale
    // batteria del dispositivo BlueZ corrispondente (stesso indirizzo,
    // formato "AA:BB:CC:DD:EE:FF" da entrambe le fonti).
    QMap<uint32_t, QString> m_outputNodeMacs;
    // Indirizzo MAC -> percentuale batteria, ricostruita per intero ad ogni
    // BlueZManager::devicesChanged (quindi ad ogni refreshDevices — vedi il
    // pulsante "↻" in colonna Output). Solo dispositivi con
    // BluetoothDevice::batteryPercentage >= 0 (non tutte le casse
    // espongono org.bluez.Battery1).
    QMap<QString, int> m_batteryByMac;
    // Rilegge BlueZManager::devices(), ricostruisce m_batteryByMac e
    // applica la percentuale aggiornata ad ogni output Bluetooth
    // attualmente tracciato in m_outputNodeMacs — richiesto esplicitamente
    // dall'utente ("se arrivano informazioni sulla batteria... metti la
    // percentuale affianco al nome nella tabella dx").
    void refreshBatteryLevels();
    // Nome leggibile effettivo di un output: il nickname se presente,
    // altrimenti la descrizione PipeWire corrente (nodo scoperto adesso),
    // altrimenti l'ultima descrizione nota per quel nome stabile (nodo non
    // più scoperto), altrimenti fallbackName (di solito il nome stabile
    // stesso, mai visto prima). Unica fonte usata sia da cueModel()
    // (etichette di rotazione / pannello Trasforma) sia da
    // handleSinkNode/setOutputNickname per popolare la colonna Output —
    // evita che le due UI mostrino nomi diversi per lo stesso sink.
    QString effectiveOutputLabel(uint32_t nodeId, const QString &fallbackName) const;

    QVector<Cue> m_cues;
    int m_armedIndex = -1; // prossima traccia che partirà con advanceCue(), -1 = coda vuota
    static constexpr int kMaxUndoDepth = 50;
    QVector<QVector<CueSnapshotEntry>> m_undoStack;
    QVector<QVector<CueSnapshotEntry>> m_redoStack;
    quint64 m_nextCueId = 1; // contatore monotono per Cue::id, mai riassegnato
};
