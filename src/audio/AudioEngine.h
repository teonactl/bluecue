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
};
