#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <cstdint>

#include "AudioNode.h"

// Interfaccia astratta del motore audio: tutto ciò che PatchManager usa per
// scoprire nodi, instradarli e generare il ping keepalive, senza sapere se
// dietro c'è PipeWire (Linux, vedi PipeWireEngine), CoreAudio (macOS, vedi
// CoreAudioEngine) o un futuro backend WASAPI (Windows).
//
// Estratta a partire dall'API pubblica di PipeWireEngine — l'unico backend
// esistente finché il porting multipiattaforma non introduce gli altri — a
// scopo di preparazione: PatchManager e il resto dell'app parlano solo con
// questa interfaccia, mai col tipo concreto, così un nuovo backend si
// aggiunge senza toccare nient'altro.
class AudioEngine : public QObject
{
    Q_OBJECT

public:
    explicit AudioEngine(QObject *parent = nullptr) : QObject(parent) {}
    ~AudioEngine() override = default;

    // Avvia il backend audio. Ritorna false se la connessione iniziale fallisce.
    virtual bool start() = 0;

    // Ferma il backend e rilascia le risorse.
    virtual void stop() = 0;

    // Snapshot corrente dei nodi conosciuti (thread-safe).
    virtual QVector<AudioNode> nodes() const = 0;

    // Crea un sink virtuale con il nome indicato (es. "zona-cucina").
    // L'operazione è asincrona: il nodo creato verrà notificato via nodeAdded().
    virtual void createVirtualSink(const QString &name, const QString &description) = 0;

    // Rimuove un sink virtuale precedentemente creato.
    virtual void removeVirtualSink(uint32_t nodeId) = 0;

    // Apre filePath e lo espone come stream di playback (direzione OUTPUT).
    // loopCount: -1 = loop continuo, N>0 = si ferma da sola dopo N ripetizioni
    // (vedi fileStreamFinished). reverse=true riproduce dalla fine verso
    // l'inizio. Ritorna il nome univoco assegnato allo stream se avviato con
    // successo, o una QString vuota in caso di errore — il chiamante lo
    // confronta con AudioNode::name in un successivo nodeAdded() per
    // scoprire il nodeId assegnato in modo asincrono.
    virtual QString createFileStream(const QString &filePath, const QString &description,
                                      int loopCount = -1, bool reverse = false) = 0;

    // Aggiorna il numero di loop di uno stream file già avviato, con effetto
    // immediato. -1 = infinito. No-op se nodeId non è uno stream file attivo.
    virtual void setFileStreamLoopCount(uint32_t nodeId, int loopCount) = 0;

    // Inverte (o ripristina) il verso di lettura di uno stream file già
    // avviato, con effetto immediato. No-op se nodeId non è uno stream file attivo.
    virtual void setFileStreamReverse(uint32_t nodeId, bool reverse) = 0;

    // Ferma e distrugge uno stream di playback file. No-op se nodeId non
    // corrisponde a nessuno stream file attivo.
    virtual void removeFileStream(uint32_t nodeId) = 0;

    // Mette in pausa (active=false) o riattiva (active=true) uno stream di
    // playback file già avviato, SENZA distruggerlo. No-op se nodeId non
    // corrisponde a nessuno stream file attivo.
    virtual void setFileStreamActive(uint32_t nodeId, bool active) = 0;

    // Collega l'output di un sink virtuale (o sorgente) all'input di un sink
    // fisico. Ritorna l'id del link creato, o 0 in caso di errore immediato.
    virtual uint32_t linkNodes(uint32_t outputNodeId, uint32_t inputNodeId) = 0;

    // Rimuove un link precedentemente creato.
    virtual void unlinkNodes(uint32_t linkId) = 0;

    // Marca (enabled=true) o smarca (enabled=false) un sink come "da tenere
    // sveglio": se marcato, riceve periodicamente un breve tono a volume
    // molto basso per evitare che i dispositivi Bluetooth vadano in
    // stand-by per inattività audio.
    virtual void setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled) = 0;

    // Riproduce due brevi bip chiaramente udibili sul sink indicato, per
    // capire fisicamente a quale altoparlante corrisponde una voce della
    // colonna Output.
    virtual void identifySink(uint32_t sinkNodeId) = 0;

    // Muta/smuta il sink indicato.
    virtual void setSinkMuted(uint32_t sinkNodeId, bool muted) = 0;

    // Applica un ritardo (millisecondi, 0 = nessuno) all'audio instradato
    // verso questo sink, per compensare la latenza maggiore di un altro
    // output (tipicamente Bluetooth) quando la stessa traccia suona su
    // entrambi in contemporanea. Effetto immediato sui collegamenti già
    // attivi verso questo sink. Backend che non supportano ancora questa
    // funzione possono limitarsi a segnalare l'errore via engineError.
    virtual void setOutputDelayMs(uint32_t sinkNodeId, int delayMs) = 0;

    // Reindirizza lo stream di riproduzione di un'altra applicazione
    // (nodeId di un nodo Kind::AppStream) verso il sink indicato,
    // "spostando" il suo audio nella patch bay invece di lasciarlo
    // sull'uscita di sistema di default. targetSinkNodeId e targetSinkName
    // devono riferirsi allo stesso sink (id live e nome stabile): il primo
    // per il collegamento diretto delle porte, il secondo per la metadata
    // di routing del session manager. Va ripetuto periodicamente
    // (auto-recovery) finché la cattura resta attiva — non è garantito che
    // resti collegato per sempre da un singolo tentativo. Nessun effetto
    // se lo stream o il sink target non esistono (più).
    virtual void setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName) = 0;

    // Rimuove il reindirizzamento impostato da setStreamTarget, riportando
    // lo stream al routing di default del sistema — usato quando una cattura
    // viene interrotta (rimozione della cue dalla playlist).
    virtual void clearStreamTarget(uint32_t streamNodeId) = 0;

    // Misura acusticamente la differenza di latenza reale tra due sink
    // (richiesto esplicitamente dall'utente: non riusciva a trovare a
    // orecchio il ritardo giusto dalla UI) riproducendo un breve click su
    // ciascuno in sequenza e registrandolo con il microfono indicato,
    // invece di stimarlo — nessuna proprietà PipeWire espone in modo
    // affidabile la latenza reale di un sink Bluetooth (dipende da
    // codec/dispositivo/condizioni radio). Asincrona (~2.3s): il risultato
    // arriva via calibrationFinished. Il filtro di ritardo eventualmente
    // già presente su uno dei due sink viene bypassato durante la misura
    // (link diretto al sink reale), per non contaminare la misura con un
    // ritardo già applicato in precedenza.
    virtual void calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId) = 0;

    // --- Parametri del ping keepalive, regolabili a runtime (menu
    // Impostazioni) senza dover ricompilare.
    virtual void setKeepAlivePingFrequency(double hz) = 0;
    virtual void setKeepAlivePingAmplitude(float amplitude) = 0;
    virtual void setKeepAlivePingDuration(double seconds) = 0;
    virtual void setKeepAlivePingPeriod(double seconds) = 0;

signals:
    void nodeAdded(const AudioNode &node);
    void nodeRemoved(uint32_t nodeId);
    void nodeUpdated(const AudioNode &node);
    void linkStateChanged(uint32_t linkId, bool active);
    void engineError(const QString &message);
    // Emesso non appena uno stream file con loopCount finito esaurisce le
    // sue ripetizioni e smette da solo di produrre audio.
    void fileStreamFinished(uint32_t nodeId);
    // Emesso ad ogni "giro" completo dello stream file (fine raggiunta e
    // ripartenza dall'altro estremo), incluso l'ultimo giro prima di un
    // eventuale fileStreamFinished.
    void fileStreamLooped(uint32_t nodeId);

    // Esito di calibrateOutputDelay(). deltaMsAtoB = latenza(B) -
    // latenza(A) in millisecondi: se positivo, sinkNodeIdA è quello più
    // veloce e va ritardato di deltaMsAtoB; se negativo, è sinkNodeIdB a
    // dover essere ritardato di -deltaMsAtoB (l'altro va riportato a 0 —
    // decisione lasciata a PatchManager, che è già proprietario di
    // setOutputDelayMs/della persistenza). success=false se il click non è
    // stato rilevato chiaramente nella registrazione (rumore ambientale,
    // microfono troppo lontano, ecc.), message ne spiega il motivo.
    void calibrationFinished(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, int deltaMsAtoB,
                              bool success, const QString &message);
};
