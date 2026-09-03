#pragma once

#include <QObject>
#include <QVector>
#include <memory>

#include "AudioEngine.h"
#include "AudioNode.h"

struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_registry;
struct spa_hook;

// Wrapper Qt attorno a libpipewire — backend AudioEngine per Linux.
//
// Gestisce un thread-loop PipeWire dedicato (pw_thread_loop) cosi' da non
// bloccare il thread della UI Qt. Le operazioni pubbliche marshalling
// verso/dal thread PipeWire avvengono tramite segnali Qt (queued connection).
//
// Questo e' solo lo scheletro: il ciclo di vita e il discovery dei nodi
// vengono implementati nel .cpp, ma i punti di estensione (creazione sink
// virtuali, link tra nodi) sono già dichiarati qui.
class PipeWireEngine : public AudioEngine
{
    Q_OBJECT

public:
    explicit PipeWireEngine(QObject *parent = nullptr);
    ~PipeWireEngine() override;

    // Avvia la connessione al demone PipeWire e il thread-loop dedicato.
    // Ritorna false se la connessione iniziale fallisce.
    bool start() override;

    // Ferma il thread-loop e rilascia le risorse PipeWire.
    void stop() override;

    // Snapshot corrente dei nodi conosciuti (thread-safe, protetto da mutex interno).
    QVector<AudioNode> nodes() const override;

    // Crea un sink virtuale con il nome indicato (es. "zona-cucina"), via
    // libpipewire-module-loopback: espone porte "playback_*" (dove arriva
    // l'audio, es. un'app reindirizzata con setStreamTarget) e "monitor_*"
    // (dove quell'audio è disponibile per essere collegato altrove con
    // linkNodes, come una qualunque sorgente). Il lato "riproduzione"
    // interno del modulo (che normalmente rimanderebbe l'audio catturato
    // sull'uscita di sistema di default) ha l'autoconnect disattivato
    // esplicitamente — altrimenti l'audio comparirebbe ANCHE lì,
    // duplicato, invece di restare confinato al sink virtuale in attesa che
    // linkNodes lo instradi esplicitamente. L'operazione è asincrona: il
    // nodo creato verrà notificato via nodeAdded() (Kind::VirtualSink,
    // correlato per nome).
    void createVirtualSink(const QString &name, const QString &description) override;

    // Rimuove un sink virtuale precedentemente creato (nodeId ottenuto da
    // nodeAdded dopo createVirtualSink) distruggendo il modulo
    // libpipewire-module-loopback sottostante.
    void removeVirtualSink(uint32_t nodeId) override;

    // Apre filePath e lo carica per intero in memoria (non più letto in
    // streaming da disco: la riproduzione all'indietro richiede accesso
    // casuale ai campioni, impossibile con la sola lettura sequenziale di
    // libsndfile), poi lo espone come pw_stream di playback (direzione
    // OUTPUT, media.class Audio/Source). loopCount regola quante volte va
    // ripetuto: -1 = in loop continuo (comportamento storico, default), N>0
    // = si ferma da sola dopo N ripetizioni (vedi fileStreamFinished).
    // reverse=true riproduce i campioni dalla fine verso l'inizio. Ritorna
    // il nome PipeWire univoco assegnato allo stream (es.
    // "bluecue.file.7") se avviato con successo, o una QString vuota in
    // caso di errore (file non apribile/decodificabile, stream non
    // avviabile) — il chiamante lo confronta con AudioNode::name in un
    // successivo nodeAdded() per scoprire il nodeId assegnato in modo
    // asincrono da PipeWire, correlazione necessaria quando più stream
    // possono essere in attesa del proprio nodeId in contemporanea (non è
    // più affidabile assumerne uno solo alla volta).
    //
    // ATTENZIONE: per file molto lunghi il caricamento in memoria può
    // richiedere qualche istante (bloccante, sul thread chiamante) e una
    // quantità di RAM proporzionale alla durata; per la musica di
    // sottofondo di una zona (tipicamente minuti, non ore) è accettabile —
    // vedi TODO in PROJECT_STATUS.md per un'eventuale variante a streaming.
    QString createFileStream(const QString &filePath, const QString &description,
                              int loopCount = -1, bool reverse = false) override;

    // Aggiorna il numero di loop di uno stream file già avviato, con effetto
    // immediato (il loop in corso finisce, poi si applica il nuovo conteggio
    // da capo). -1 = infinito. No-op se nodeId non è uno stream file attivo.
    void setFileStreamLoopCount(uint32_t nodeId, int loopCount) override;

    // Inverte (o ripristina) il verso di lettura di uno stream file già
    // avviato, con effetto immediato: la riproduzione continua dal punto
    // corrente ma cambiando direzione, senza saltare all'inizio/fine. No-op
    // se nodeId non è uno stream file attivo.
    void setFileStreamReverse(uint32_t nodeId, bool reverse) override;

    // Ferma e distrugge uno stream di playback file creato con
    // createFileStream(), una volta noto il suo nodeId. No-op se nodeId non
    // corrisponde a nessuno stream file attivo (es. è un sink o un nodo
    // hardware).
    void removeFileStream(uint32_t nodeId) override;

    // Mette in pausa (active=false) o riattiva (active=true) uno stream di
    // playback file già avviato, SENZA distruggerlo: il nodo PipeWire e i
    // suoi collegamenti restano vivi, semplicemente smette di produrre
    // audio (silenzio) finché non viene riattivato — a differenza di
    // removeFileStream(), che invece distrugge il nodo e con esso ogni
    // collegamento. No-op se nodeId non corrisponde a nessuno stream file
    // attivo.
    void setFileStreamActive(uint32_t nodeId, bool active) override;

    // Collega l'output di un sink virtuale (o sorgente) all'input di un sink fisico.
    // Ritorna l'id del link creato, o 0 in caso di errore immediato.
    uint32_t linkNodes(uint32_t outputNodeId, uint32_t inputNodeId) override;

    // Rimuove un link precedentemente creato.
    void unlinkNodes(uint32_t linkId) override;

    // Marca (enabled=true) o smarca (enabled=false) un sink come "da tenere
    // sveglio": se marcato, riceve periodicamente un breve tono a volume
    // molto basso (impercettibile) per evitare che i dispositivi Bluetooth
    // vadano in stand-by per inattività audio. Il ping viaggia su un link
    // separato, indipendente da qualunque routing utente creato con
    // linkNodes/toggleConnection: non tocca lo stato di PatchManager. Il
    // generatore è unico e condiviso, creato pigramente alla prima
    // richiesta; non compare come nodo Source selezionabile nella UI.
    void setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled) override;

    // Riproduce due brevi bip chiaramente udibili sul sink indicato, per
    // capire fisicamente a quale altoparlante corrisponde una voce della
    // colonna Output ("quale cassa è questa?"). A differenza del keepalive,
    // qui va sentito: crea un nuovo stream dedicato, lo collega al sink,
    // suona la sequenza e si autodistrugge da solo subito dopo — non
    // richiede una removeIdentifySink esplicita.
    void identifySink(uint32_t sinkNodeId) override;

    // Applica un ritardo (millisecondi, 0 = nessuno, max 2000) all'audio
    // instradato verso questo sink. Realizzato interponendo un
    // pw_filter dedicato (creato pigramente alla prima richiesta con
    // delayMs>0, mai distrutto per il resto della sessione anche se
    // riportato a 0 — evita di dover ricollegare la topologia ad ogni
    // cambio) tra le sorgenti collegate e il sink reale: linkNodes() verso
    // un sink che ha già un filtro attivo collega automaticamente alla sua
    // porta di ingresso invece che al sink direttamente. Effetto quasi
    // immediato sui collegamenti già attivi (il buffer di ritardo si
    // riempie gradualmente, nessun ricollegamento necessario). Vedi
    // PipeWireEngine::Impl::DelayFilter nel .cpp.
    void setOutputDelayMs(uint32_t sinkNodeId, int delayMs) override;

    // Reindirizza (o ripristina) il routing di un'altra applicazione già
    // in esecuzione (uno stream Kind::AppStream) tramite la metadata
    // "default" di PipeWire (chiave "target.object", stessa tecnica usata
    // da strumenti come pavucontrol/wpctl per "spostare" uno stream) —
    // richiede che il nodo abbia un stream con permessi M (normale per una
    // sessione utente). Va richiamato di tanto in tanto mentre la cattura è
    // attiva (PatchManager tiene un timer periodico): il target è un hint
    // che il session manager applica alla prossima rivalutazione del
    // routing, non una garanzia permanente.
    void setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName) override;
    void clearStreamTarget(uint32_t streamNodeId) override;
    void calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId) override;

    // Muta/smuta il sink indicato (SPA_PROP_mute) — stesso parametro che
    // manipolano pw-cli/wpctl. Sostituisce un precedente setSinkVolume
    // (SPA_PROP_channelVolumes): rimosso su richiesta esplicita
    // dell'utente, il volume software del nodo agisce solo dentro il
    // range già limitato a monte dal mixer di sistema, quindi un vero
    // controllo del volume da qui non è affidabile — il muto invece è
    // un interruttore netto, non soggetto allo stesso problema. Richiede
    // che il nodo sia già tracciato da un SinkWatch (qualunque
    // PhysicalSink lo è, vedi onGlobal); silenziosamente no-op se il nodo
    // non è (più) noto.
    void setSinkMuted(uint32_t sinkNodeId, bool muted) override;

    // --- Parametri del ping keepalive, regolabili a runtime (menu
    // Impostazioni) senza dover ricompilare — richiesto esplicitamente
    // dall'utente per poter sperimentare quanto breve/silenzioso può
    // essere restando comunque efficace, incluso provare una frequenza
    // ultrasonica (inaudibile all'orecchio umano) invece di un rumore
    // udibile a basso volume. Applicati alla prossima callback "process"
    // dello stream keepalive, quindi quasi immediati; se lo stream non è
    // ancora stato creato (nessun sink Bluetooth mai agganciato) restano
    // comunque memorizzati e si applicano non appena parte.
    void setKeepAlivePingFrequency(double hz) override;
    void setKeepAlivePingAmplitude(float amplitude) override;
    void setKeepAlivePingDuration(double seconds) override;
    void setKeepAlivePingPeriod(double seconds) override;

    // nodeAdded/nodeRemoved/nodeUpdated/linkStateChanged/engineError/
    // fileStreamFinished/fileStreamLooped sono ereditati da AudioEngine
    // (stesso set di segnali per qualunque backend, vedi AudioEngine.h) —
    // un segnale QObject appartiene alla classe che lo dichiara e non va
    // ridichiarato qui: "emit" da questa classe li invoca comunque, per
    // ereditarietà.

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
