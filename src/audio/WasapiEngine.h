#pragma once

#include <QVector>
#include <memory>

#include "AudioEngine.h"
#include "AudioNode.h"

// Backend AudioEngine per Windows: WASAPI per l'audio (device discovery,
// playback, mute), Media Foundation per decodificare i file (nessuna
// libreria esterna da installare, come ExtAudioFile su macOS) — vedi
// WindowsBluetoothManager.h per il lato Bluetooth.
//
// Differenza architetturale rispetto a PipeWire (grafo di link arbitrari)
// e a CoreAudio (Aggregate Device, vedi CoreAudioEngine.h): WASAPI non ha
// NESSUNO dei due meccanismi — non esiste un modo nativo di "fondere" un
// singolo stream verso N device fisici. Per instradare una traccia verso
// più output insieme (PatchManager::toggleCueOutput/desiredOutputNames) si
// usa un fan-out manuale: un IAudioClient/IAudioRenderClient dedicato PER
// OUTPUT collegato allo stesso producer, tutti alimentati dalla stessa
// posizione di lettura nel buffer decodificato da un unico thread per
// stream — vedi il commento in cima a WasapiEngine.cpp.
//
// NOTA IMPORTANTE: scritto seguendo la documentazione WASAPI/Media
// Foundation (audioclient.h, mmdeviceapi.h, mfreadwrite.h, bluetoothapis.h)
// ma mai compilato in locale — l'ambiente di sviluppo di questo progetto è
// Linux. Verificato/iterato tramite la CI GitHub Actions (runner
// windows-latest), non su una macchina Windows reale con hardware
// audio/Bluetooth vero — da testare dal vivo prima di considerarlo pronto.
class WasapiEngine : public AudioEngine
{
    Q_OBJECT

public:
    explicit WasapiEngine(QObject *parent = nullptr);
    ~WasapiEngine() override;

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

    void setKeepAlivePingFrequency(double hz) override;
    void setKeepAlivePingAmplitude(float amplitude) override;
    void setKeepAlivePingDuration(double seconds) override;
    void setKeepAlivePingPeriod(double seconds) override;

    // Pubblico per lo stesso motivo di CoreAudioEngine::Impl (vedi
    // CoreAudioEngine.h): i thread worker per-stream in WasapiEngine.cpp
    // sono funzioni libere, non member/friend.
    struct Impl;

private:
    std::unique_ptr<Impl> d;
};
