#pragma once

#include <QVector>
#include <memory>

#include "AudioEngine.h"
#include "AudioNode.h"

// Backend AudioEngine per macOS (Apple Silicon incluso — nessuna dipendenza
// da libpipewire, solo framework di sistema Apple: CoreAudio/AudioToolbox).
//
// Differenza architetturale principale rispetto a PipeWireEngine: PipeWire
// espone un grafo di nodi/porte collegabili arbitrariamente (pw_link), mentre
// CoreAudio no — un AudioQueue scrive su UN SOLO AudioDeviceID alla volta.
// Per riprodurre "una traccia collegata a N output fisici insieme"
// (PatchManager::toggleCueOutput/desiredOutputNames) si usa il meccanismo
// nativo CoreAudio pensato apposta per questo: un Aggregate Device
// (AudioHardwareCreateAggregateDevice) il cui sub-device-list è l'insieme
// corrente degli output desiderati per quella traccia — un solo AudioQueue
// scrive sull'aggregate, e CoreAudio replica il flusso su ciascun
// sub-device in sincrono. Un aggregate viene creato/distrutto per ogni
// stream file che ha almeno un link attivo, e il suo sub-device-list viene
// aggiornato ad ogni linkNodes/unlinkNodes — vedi il commento in cima a
// CoreAudioEngine.cpp per i dettagli.
//
// NOTA IMPORTANTE: scritto seguendo la documentazione delle API CoreAudio/
// AudioToolbox (AudioHardware.h, AudioQueue.h, ExtAudioFile.h) ma MAI
// compilato né testato — questo ambiente di sviluppo è Linux, senza SDK
// macOS disponibile. Da compilare e verificare su un Mac (Xcode command
// line tools) prima di considerarlo funzionante; qualche dettaglio fine
// (nomi esatti di chiavi/costanti, comportamento di AudioQueue con reverse
// playback) potrebbe richiedere aggiustamenti una volta testato su
// hardware reale.
class CoreAudioEngine : public AudioEngine
{
    Q_OBJECT

public:
    explicit CoreAudioEngine(QObject *parent = nullptr);
    ~CoreAudioEngine() override;

    bool start() override;
    void stop() override;
    QVector<AudioNode> nodes() const override;

    void createVirtualSink(const QString &name, const QString &description) override;
    void removeVirtualSink(uint32_t nodeId) override;

    QString createFileStream(const QString &filePath, const QString &description,
                              int loopCount = -1, bool reverse = false) override;
    void setFileStreamLoopCount(uint32_t nodeId, int loopCount) override;
    void setFileStreamReverse(uint32_t nodeId, bool reverse) override;
    void removeFileStream(uint32_t nodeId) override;
    void setFileStreamActive(uint32_t nodeId, bool active) override;

    uint32_t linkNodes(uint32_t outputNodeId, uint32_t inputNodeId) override;
    void unlinkNodes(uint32_t linkId) override;

    void setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled) override;
    void identifySink(uint32_t sinkNodeId) override;
    void setSinkMuted(uint32_t sinkNodeId, bool muted) override;
    // Non ancora implementato su macOS (vedi PipeWireEngine per il backend
    // Linux, l'unico su cui è stato realizzato finora) — no-op che segnala
    // l'errore via engineError invece di fallire silenziosamente.
    void setOutputDelayMs(uint32_t sinkNodeId, int delayMs) override;
    // Non ancora implementato su macOS (nessun equivalente diretto del
    // meccanismo target.object di WirePlumber usato dal backend Linux).
    void setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName) override;
    void clearStreamTarget(uint32_t streamNodeId) override;
    // Non ancora implementato su macOS.
    void calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId) override;

    void setKeepAlivePingFrequency(double hz) override;
    void setKeepAlivePingAmplitude(float amplitude) override;
    void setKeepAlivePingDuration(double seconds) override;
    void setKeepAlivePingPeriod(double seconds) override;

    // Pubblico (non un dettaglio nascosto dietro "private"): le callback
    // AudioQueue/property-listener in CoreAudioEngine.cpp sono funzioni
    // libere (namespace anonimo, non member/friend) e devono poter
    // nominare CoreAudioEngine::Impl — solo l'ISTANZA "d" sotto resta
    // privata.
    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
