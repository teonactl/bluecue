#include "WasapiEngine.h"

// NOTA GENERALE: vedi il commento in cima a WasapiEngine.h — mai compilato
// in locale (ambiente di sviluppo Linux), scritto seguendo la
// documentazione ufficiale; da validare/aggiustare tramite la CI Windows
// e infine su una macchina reale.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QMap>
#include <QSet>
#include <QString>
#include <QThread>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

namespace {

// RAII minimale per puntatori COM — evita di trascinarsi dietro ATL/WIL
// solo per questo file. Non thread-safe di per sé (come ogni oggetto COM
// non esplicitamente documentato altrimenti): ogni ComPtr vive sul thread
// che lo crea.
template <typename T>
struct ComPtr
{
    T *ptr = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;
    ComPtr(ComPtr &&other) noexcept : ptr(other.ptr) { other.ptr = nullptr; }
    ~ComPtr() { reset(); }
    void reset() { if (ptr) { ptr->Release(); ptr = nullptr; } }
    T **addr() { reset(); return &ptr; }
    T *operator->() const { return ptr; }
    operator T *() const { return ptr; }
    explicit operator bool() const { return ptr != nullptr; }
};

QString wstringToQString(LPCWSTR s)
{
    return s ? QString::fromWCharArray(s) : QString();
}

// Legge il nome leggibile e l'InstanceId (usato per riconoscere i device
// Bluetooth, vedi sotto) di un endpoint tramite il suo IPropertyStore.
void readEndpointProperties(IMMDevice *device, QString &friendlyName, QString &instanceId)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, store.addr())))
        return;

    PROPVARIANT nameVar;
    PropVariantInit(&nameVar);
    if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &nameVar)) && nameVar.vt == VT_LPWSTR)
        friendlyName = wstringToQString(nameVar.pwszVal);
    PropVariantClear(&nameVar);

    PROPVARIANT instVar;
    PropVariantInit(&instVar);
    if (SUCCEEDED(store->GetValue(PKEY_Device_InstanceId, &instVar)) && instVar.vt == VT_LPWSTR)
        instanceId = wstringToQString(instVar.pwszVal);
    PropVariantClear(&instVar);
}

// Windows non espone un flag "questo endpoint è Bluetooth" diretto sulla
// property store dell'endpoint audio — il trucco comunemente usato (anche
// da altri progetti open source) è ispezionare l'InstanceId del device PnP
// sottostante: i device Bluetooth Classic vengono enumerati sotto
// l'enumeratore "BTHENUM", quelli BLE sotto varianti con "BTHLE" — un
// controllo per sottostringa, non un'API dedicata. Euristica, non garanzia
// (vedi anche la nota sulla batteria più sotto: stesso genere di limite).
bool looksBluetooth(const QString &instanceId)
{
    return instanceId.contains(QStringLiteral("BTHENUM"), Qt::CaseInsensitive)
        || instanceId.contains(QStringLiteral("BTHLE"), Qt::CaseInsensitive)
        || instanceId.contains(QStringLiteral("BTH"), Qt::CaseInsensitive);
}

void writeSineTone(float *out, int frames, int channels, double &phase,
                    double frequencyHz, float amplitude, double sampleRate)
{
    const double step = 2.0 * std::numbers::pi * frequencyHz / sampleRate;
    for (int i = 0; i < frames; ++i) {
        const float sample = amplitude * static_cast<float>(std::sin(phase));
        for (int c = 0; c < channels; ++c)
            out[i * channels + c] = sample;
        phase += step;
        if (phase > 2.0 * std::numbers::pi)
            phase -= 2.0 * std::numbers::pi;
    }
}

} // namespace

// -----------------------------------------------------------------------
// Modello di routing: senza un equivalente Windows del grafo di link
// PipeWire o dell'Aggregate Device CoreAudio, un producer (uno stream
// file) collegato a N output viene servito da un IAudioClient/
// IAudioRenderClient PER OUTPUT, tutti scritti dallo stesso thread worker
// dedicato allo stream, allo stesso ritmo (la capacità libera minima tra
// tutti i target governa quanti frame avanzare ad ogni giro, per restare
// sincronizzati) — vedi FileStreamState::renderTargets e
// fileStreamWorkerLoop più sotto.
// -----------------------------------------------------------------------

struct WasapiEngine::Impl
{
    WasapiEngine *engine = nullptr;

    mutable QMutex mutex;
    QVector<AudioNode> nodes;
    QMap<uint32_t, QString> deviceIds;      // nodeId -> IMMDevice::GetId() stabile
    QMap<QString, uint32_t> deviceIdToNode; // inverso, per la discovery incrementale

    ComPtr<IMMDeviceEnumerator> enumerator;
    uint32_t nextPhysicalId = 1;            // spazio riservato ai device fisici scoperti
    uint32_t nextFileStreamId = 0x40000000; // stessa convenzione di CoreAudioEngine
    uint32_t nextVirtualSinkId = 0x50000000;

    struct RenderTarget
    {
        QString deviceId;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> renderClient;
        UINT32 bufferFrameCount = 0;
        int channelCount = 2;
    };

    struct FileStreamState
    {
        QString name;
        uint32_t nodeId = 0;
        std::vector<float> samples; // interleaved, canale = channelCount
        int channelCount = 2;
        double sampleRate = 44100.0;
        int64_t framePos = 0;
        std::atomic<bool> reverse{false};
        std::atomic<int> loopCount{-1};
        std::atomic<bool> active{true};
        std::atomic<bool> stopRequested{false};

        // Protetti da targetsMutex: la lista dei device collegati può
        // cambiare (linkNodes/unlinkNodes) mentre il worker thread gira.
        QMutex targetsMutex;
        std::vector<std::unique_ptr<RenderTarget>> renderTargets;

        std::unique_ptr<QThread> workerThread; // il thread stesso vive qui, la funzione gira "manualmente" (vedi start())
    };
    std::map<uint32_t, std::unique_ptr<FileStreamState>> fileStreams;

    struct LinkEntry { uint32_t linkId; uint32_t outputNodeId; uint32_t inputNodeId; };
    QVector<LinkEntry> links;
    uint32_t nextLinkId = 1;

    std::atomic<double> keepAliveFrequencyHz{18000.0};
    std::atomic<float> keepAliveAmplitude{0.0015f};
    std::atomic<double> keepAliveDurationSeconds{0.06};
    std::atomic<double> keepAlivePeriodSeconds{20.0};

    struct KeepAliveStream
    {
        std::atomic<bool> stopRequested{false};
        std::unique_ptr<QThread> thread;
    };
    QMap<uint32_t, std::shared_ptr<KeepAliveStream>> keepAliveStreams;

    // Mai deregistrata in una prima stesura: senza tenerne un riferimento,
    // stop() non poteva chiamare UnregisterEndpointNotificationCallback
    // prima di rilasciare l'enumerator, lasciando la callback COM
    // potenzialmente invocabile da un dispositivo appena rimosso/aggiunto
    // dopo che Impl è già in fase di distruzione.
    IMMNotificationClient *notificationClient = nullptr;

    QString deviceIdFor(uint32_t nodeId) const
    {
        QMutexLocker locker(&mutex);
        return deviceIds.value(nodeId);
    }

    ComPtr<IMMDevice> openDevice(const QString &deviceId) const
    {
        ComPtr<IMMDevice> device;
        if (!enumerator)
            return device;
        enumerator->GetDevice(reinterpret_cast<LPCWSTR>(deviceId.utf16()), device.addr());
        return device;
    }

    // --- Discovery ---

    void refreshDeviceList(bool emitSignals)
    {
        if (!enumerator)
            return;

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.addr())))
            return;

        UINT count = 0;
        collection->GetCount(&count);

        QVector<AudioNode> discovered;
        QMap<uint32_t, QString> newDeviceIds;
        QMap<QString, uint32_t> newDeviceIdToNode;

        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.addr())))
                continue;

            LPWSTR idRaw = nullptr;
            if (FAILED(device->GetId(&idRaw)) || !idRaw)
                continue;
            const QString id = wstringToQString(idRaw);
            CoTaskMemFree(idRaw);

            QString friendlyName;
            QString instanceId;
            readEndpointProperties(device, friendlyName, instanceId);

            uint32_t nodeId;
            {
                QMutexLocker locker(&mutex);
                auto it = deviceIdToNode.find(id);
                nodeId = (it != deviceIdToNode.end()) ? it.value() : nextPhysicalId++;
            }

            AudioNode node;
            node.id = nodeId;
            node.name = id;
            node.description = friendlyName.isEmpty() ? id : friendlyName;
            node.kind = AudioNode::Kind::PhysicalSink;
            node.isBluetooth = looksBluetooth(instanceId);

            discovered.append(node);
            newDeviceIds.insert(nodeId, id);
            newDeviceIdToNode.insert(id, nodeId);
        }

        QVector<AudioNode> added;
        QVector<uint32_t> removed;
        {
            QMutexLocker locker(&mutex);
            QSet<uint32_t> previous;
            for (const AudioNode &n : nodes)
                previous.insert(n.id);

            QSet<uint32_t> current;
            for (const AudioNode &n : discovered)
                current.insert(n.id);

            for (const AudioNode &n : discovered) {
                if (!previous.contains(n.id))
                    added.append(n);
            }
            for (uint32_t id : previous) {
                if (!current.contains(id))
                    removed.append(id);
            }

            nodes = discovered;
            deviceIds = newDeviceIds;
            deviceIdToNode = newDeviceIdToNode;
        }

        if (!emitSignals)
            return;
        for (const AudioNode &n : added)
            QMetaObject::invokeMethod(engine, [this, n]() { emit engine->nodeAdded(n); }, Qt::QueuedConnection);
        for (uint32_t id : removed)
            QMetaObject::invokeMethod(engine, [this, id]() { emit engine->nodeRemoved(id); }, Qt::QueuedConnection);
    }
};

// -----------------------------------------------------------------------
// Notifiche di endpoint aggiunti/rimossi/cambiati — arrivano su un thread
// COM qualunque, mai quello Qt: ogni reazione va rimbalzata con
// QMetaObject::invokeMethod (stessa disciplina di CoreAudioEngine per i
// suoi property-listener block).
// -----------------------------------------------------------------------
namespace {
class EndpointNotificationClient : public IMMNotificationClient
{
public:
    explicit EndpointNotificationClient(WasapiEngine::Impl *impl) : m_impl(impl) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_refCount); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0)
            delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { refresh(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { refresh(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { refresh(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    void refresh()
    {
        WasapiEngine::Impl *impl = m_impl;
        QMetaObject::invokeMethod(impl->engine, [impl]() { impl->refreshDeviceList(true); }, Qt::QueuedConnection);
    }

    WasapiEngine::Impl *m_impl;
    ULONG m_refCount = 1;
};
} // namespace

// -----------------------------------------------------------------------
// Thread worker per uno stream file: legge dal buffer decodificato e
// scrive su tutti i RenderTarget collegati in quel momento, avanzando la
// posizione condivisa in base alla capacità libera MINIMA tra i target
// (così restano sincronizzati anche se un device drena più lentamente di
// un altro). Gira finché stopRequested non diventa true.
// -----------------------------------------------------------------------
namespace {
void fileStreamWorkerLoop(WasapiEngine::Impl *impl, WasapiEngine::Impl::FileStreamState *state)
{
    // Ogni thread che invoca metodi COM (qui: GetCurrentPadding/GetBuffer/
    // ReleaseBuffer/Stop su IAudioClient/IAudioRenderClient, anche se le
    // interfacce sono state create da un altro thread in linkNodes()) deve
    // inizializzare COM su se stesso — non basta che l'abbia fatto il thread
    // chiamante di createFileStream/linkNodes. Omesso in una prima stesura
    // mai eseguita su Windows reale: causa quasi certa di riproduzione
    // silenziosamente muta (chiamate che falliscono con CO_E_NOTINITIALIZED)
    // o di comportamento non definito, non di un crash sempre riproducibile.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    const int channels = state->channelCount;
    const int64_t totalFrames = channels > 0 ? static_cast<int64_t>(state->samples.size() / channels) : 0;

    while (!state->stopRequested.load()) {
        QThread::msleep(10);
        if (totalFrames <= 0)
            continue;

        QMutexLocker locker(&state->targetsMutex);
        if (state->renderTargets.empty())
            continue;

        UINT32 minAvailable = 0xFFFFFFFFu;
        for (auto &target : state->renderTargets) {
            UINT32 padding = 0;
            if (FAILED(target->client->GetCurrentPadding(&padding)))
                continue;
            const UINT32 available = target->bufferFrameCount > padding ? target->bufferFrameCount - padding : 0;
            minAvailable = std::min(minAvailable, available);
        }
        if (minAvailable == 0xFFFFFFFFu || minAvailable == 0)
            continue;

        const int framesToWrite = static_cast<int>(minAvailable);
        const bool reverse = state->reverse.load();
        const bool active = state->active.load();
        bool looped = false;
        bool finished = false;

        // Prepara il chunk una sola volta (stesso identico audio per ogni
        // device collegato), poi lo scrive su ciascun render client.
        std::vector<float> chunk(static_cast<size_t>(framesToWrite) * channels, 0.0f);
        if (active) {
            for (int i = 0; i < framesToWrite; ++i) {
                if (state->framePos < 0 || state->framePos >= totalFrames) {
                    state->framePos = reverse ? totalFrames - 1 : 0;
                    looped = true;
                    int remaining = state->loopCount.load();
                    if (remaining > 0) {
                        remaining -= 1;
                        state->loopCount.store(remaining);
                        if (remaining == 0)
                            finished = true;
                    }
                }
                if (finished)
                    break; // il resto del chunk resta a zero (silenzio)
                const float *src = &state->samples[state->framePos * channels];
                for (int c = 0; c < channels; ++c)
                    chunk[i * channels + c] = src[c];
                state->framePos += reverse ? -1 : 1;
            }
        }

        for (auto &target : state->renderTargets) {
            BYTE *buffer = nullptr;
            if (FAILED(target->renderClient->GetBuffer(static_cast<UINT32>(framesToWrite), &buffer)))
                continue;
            std::memcpy(buffer, chunk.data(), chunk.size() * sizeof(float));
            target->renderClient->ReleaseBuffer(static_cast<UINT32>(framesToWrite), 0);
        }

        if (looped) {
            const uint32_t nodeId = state->nodeId;
            QMetaObject::invokeMethod(impl->engine, [impl, nodeId]() { emit impl->engine->fileStreamLooped(nodeId); },
                                       Qt::QueuedConnection);
        }
        if (finished) {
            const uint32_t nodeId = state->nodeId;
            QMetaObject::invokeMethod(impl->engine, [impl, nodeId]() { emit impl->engine->fileStreamFinished(nodeId); },
                                       Qt::QueuedConnection);
        }
    }

    {
        QMutexLocker locker(&state->targetsMutex);
        for (auto &target : state->renderTargets)
            target->client->Stop();
    }

    if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
        CoUninitialize();
}

// Crea e avvia un IAudioClient in modalità condivisa per il device
// indicato, con un IAudioRenderClient pronto a ricevere campioni Float32
// interleaved a channelCount canali. Ritorna nullptr in caso di errore.
std::unique_ptr<WasapiEngine::Impl::RenderTarget> createRenderTarget(
    WasapiEngine::Impl *impl, const QString &deviceId, double sampleRate, int channelCount)
{
    auto device = impl->openDevice(deviceId);
    if (!device)
        return nullptr;

    auto target = std::make_unique<WasapiEngine::Impl::RenderTarget>();
    target->deviceId = deviceId;
    target->channelCount = channelCount;

    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void **>(target->client.addr()))))
        return nullptr;

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(channelCount);
    format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(channelCount * 4);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200ms, unità di 100ns
    if (FAILED(target->client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, kBufferDuration, 0, &format, nullptr)))
        return nullptr;

    if (FAILED(target->client->GetBufferSize(&target->bufferFrameCount)))
        return nullptr;
    if (FAILED(target->client->GetService(__uuidof(IAudioRenderClient),
                                           reinterpret_cast<void **>(target->renderClient.addr()))))
        return nullptr;

    // Pre-riempie il buffer di silenzio prima di Start(), come da prassi
    // WASAPI (evita un primo istante udibile di sottoflusso).
    BYTE *buffer = nullptr;
    if (SUCCEEDED(target->renderClient->GetBuffer(target->bufferFrameCount, &buffer)))
        target->renderClient->ReleaseBuffer(target->bufferFrameCount, AUDCLNT_BUFFERFLAGS_SILENT);

    target->client->Start();
    return target;
}
} // namespace

WasapiEngine::WasapiEngine(QObject *parent)
    : AudioEngine(parent)
    , d(std::make_unique<Impl>())
{
    d->engine = this;
}

WasapiEngine::~WasapiEngine()
{
    stop();
}

bool WasapiEngine::start()
{
    // COINIT_MULTITHREADED: i thread worker per-stream e il thread di
    // notifica endpoint chiamano tutti API COM concorrentemente — serve
    // l'apartment multi-thread, non quello single-thread (STA) di default
    // per app GUI.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(comResult) && comResult != RPC_E_CHANGED_MODE) {
        emit engineError(QStringLiteral("CoInitializeEx fallita"));
        return false;
    }

    if (FAILED(MFStartup(MF_VERSION))) {
        emit engineError(QStringLiteral("MFStartup (Media Foundation) fallita"));
        return false;
    }

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                 __uuidof(IMMDeviceEnumerator),
                                 reinterpret_cast<void **>(d->enumerator.addr())))) {
        emit engineError(QStringLiteral("Creazione di IMMDeviceEnumerator fallita"));
        return false;
    }

    // emitSignals=true: PatchManager scopre OGNI sink (fisico o già
    // connesso via Bluetooth) esclusivamente tramite nodeAdded, non con una
    // chiamata separata tipo "dammi la lista attuale" — vedi il commento in
    // PatchManager.cpp sopra la connessione a nodeAdded ("copre sia i jack
    // hardware sia i sink Bluetooth già connessi al momento dell'avvio").
    // Con false (come qui prima) i device già presenti all'avvio (praticamente
    // sempre almeno l'output di default) restavano scoperti solo
    // internamente in Impl::nodes, mai annunciati alla UI: colonna Output
    // vuota per sempre finché un device non veniva ricollegato DOPO l'avvio
    // (unico modo in cui prima scattava un refreshDeviceList(true) reale).
    // Bug reale, mai visto finché il backend non è girato su Windows vero.
    d->refreshDeviceList(true);

    auto *notificationClient = new EndpointNotificationClient(d.get());
    d->enumerator->RegisterEndpointNotificationCallback(notificationClient);
    d->notificationClient = notificationClient;

    return true;
}

void WasapiEngine::stop()
{
    // Deregistrare PRIMA di rilasciare l'enumerator: altrimenti una notifica
    // di device in arrivo nel frattempo (su un thread COM qualunque) potrebbe
    // invocare EndpointNotificationClient::refresh() -> Impl::refreshDeviceList
    // mentre Impl è a metà distruzione più sotto.
    if (d->enumerator && d->notificationClient) {
        d->enumerator->UnregisterEndpointNotificationCallback(d->notificationClient);
        d->notificationClient->Release();
        d->notificationClient = nullptr;
    }

    for (auto &[id, entry] : d->fileStreams) {
        entry->stopRequested.store(true);
        if (entry->workerThread)
            entry->workerThread->wait();
    }
    d->fileStreams.clear();

    for (auto it = d->keepAliveStreams.begin(); it != d->keepAliveStreams.end(); ++it) {
        it.value()->stopRequested.store(true);
        if (it.value()->thread)
            it.value()->thread->wait();
    }
    d->keepAliveStreams.clear();

    d->enumerator.reset();
    MFShutdown();
    CoUninitialize();
}

QVector<AudioNode> WasapiEngine::nodes() const
{
    QMutexLocker locker(&d->mutex);
    return d->nodes;
}

void WasapiEngine::createVirtualSink(const QString &name, const QString &description)
{
    // Non usato dall'attuale PatchManager (routing sempre diretto
    // sorgente->output fisici — vedi la stessa nota in CoreAudioEngine.cpp)
    // — implementato solo per completezza dell'interfaccia.
    const uint32_t nodeId = d->nextVirtualSinkId++;
    AudioNode node;
    node.id = nodeId;
    node.name = QStringLiteral("bluecue.virtual.%1").arg(nodeId);
    node.description = description.isEmpty() ? name : description;
    node.kind = AudioNode::Kind::VirtualSink;
    {
        QMutexLocker locker(&d->mutex);
        d->nodes.append(node);
    }
    QMetaObject::invokeMethod(this, [this, node]() { emit nodeAdded(node); }, Qt::QueuedConnection);
}

void WasapiEngine::removeVirtualSink(uint32_t nodeId)
{
    {
        QMutexLocker locker(&d->mutex);
        d->nodes.removeIf([nodeId](const AudioNode &n) { return n.id == nodeId; });
    }
    QMetaObject::invokeMethod(this, [this, nodeId]() { emit nodeRemoved(nodeId); }, Qt::QueuedConnection);
}

QString WasapiEngine::createFileStream(const QString &filePath, const QString &description,
                                        int loopCount, bool reverse)
{
    ComPtr<IMFSourceReader> reader;
    if (FAILED(MFCreateSourceReaderFromURL(reinterpret_cast<LPCWSTR>(filePath.utf16()), nullptr, reader.addr()))) {
        emit engineError(QStringLiteral("Impossibile aprire il file audio: %1").arg(filePath));
        return {};
    }

    // Forza l'output del reader a PCM Float32 — Media Foundation applica
    // da solo la conversione/decodifica necessaria (WAV/MP3/AAC/WMA nativi
    // in Windows, senza dipendenze esterne).
    ComPtr<IMFMediaType> partialType;
    MFCreateMediaType(partialType.addr());
    partialType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    partialType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (FAILED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, partialType))) {
        emit engineError(QStringLiteral("Formato audio non supportato: %1").arg(filePath));
        return {};
    }

    ComPtr<IMFMediaType> actualType;
    reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, actualType.addr());
    UINT32 channelCount = 2;
    UINT32 sampleRate = 44100;
    actualType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channelCount);
    actualType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);

    auto state = std::make_unique<Impl::FileStreamState>();
    state->channelCount = static_cast<int>(channelCount);
    state->sampleRate = sampleRate;

    // Caricamento per intero in memoria (non streaming): stessa scelta di
    // PipeWireEngine/CoreAudioEngine, necessaria per la riproduzione
    // all'indietro (accesso casuale ai campioni).
    while (true) {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr, &flags, nullptr, sample.addr())))
            break;
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
            break;
        if (!sample)
            continue;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.addr())))
            continue;

        BYTE *data = nullptr;
        DWORD dataLength = 0;
        if (FAILED(buffer->Lock(&data, nullptr, &dataLength)))
            continue;

        const size_t sampleCount = dataLength / sizeof(float);
        const size_t offset = state->samples.size();
        state->samples.resize(offset + sampleCount);
        std::memcpy(state->samples.data() + offset, data, dataLength);
        buffer->Unlock();
    }

    const uint32_t nodeId = d->nextFileStreamId++;
    state->nodeId = nodeId;
    state->name = QStringLiteral("bluecue.file.%1").arg(nodeId);
    state->loopCount.store(loopCount);
    state->reverse.store(reverse);
    const int64_t totalFrames = state->channelCount > 0
        ? static_cast<int64_t>(state->samples.size() / state->channelCount) : 0;
    state->framePos = reverse ? totalFrames - 1 : 0;

    Impl::FileStreamState *rawState = state.get();
    Impl *impl = d.get();
    // QThread::create incapsula una funzione libera come thread — non
    // serve una sottoclasse QThread dedicata solo per questo worker.
    state->workerThread.reset(QThread::create([impl, rawState]() { fileStreamWorkerLoop(impl, rawState); }));
    state->workerThread->start();

    const QString streamName = state->name;
    d->fileStreams.emplace(nodeId, std::move(state));

    AudioNode node;
    node.id = nodeId;
    node.name = streamName;
    node.description = description;
    node.kind = AudioNode::Kind::Source;
    QMetaObject::invokeMethod(this, [this, node]() { emit nodeAdded(node); }, Qt::QueuedConnection);

    return streamName;
}

void WasapiEngine::setFileStreamLoopCount(uint32_t nodeId, int loopCount)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->loopCount.store(loopCount);
}

void WasapiEngine::setFileStreamReverse(uint32_t nodeId, bool reverse)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->reverse.store(reverse);
}

void WasapiEngine::removeFileStream(uint32_t nodeId)
{
    auto it = d->fileStreams.find(nodeId);
    if (it == d->fileStreams.end())
        return;

    QVector<uint32_t> linkIdsToRemove;
    for (const auto &l : d->links) {
        if (l.outputNodeId == nodeId)
            linkIdsToRemove.append(l.linkId);
    }
    for (uint32_t linkId : linkIdsToRemove)
        unlinkNodes(linkId);

    it->second->stopRequested.store(true);
    if (it->second->workerThread)
        it->second->workerThread->wait();
    d->fileStreams.erase(it);

    emit nodeRemoved(nodeId);
}

void WasapiEngine::setFileStreamActive(uint32_t nodeId, bool active)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->active.store(active);
}

uint32_t WasapiEngine::linkNodes(uint32_t outputNodeId, uint32_t inputNodeId)
{
    auto it = d->fileStreams.find(outputNodeId);
    if (it == d->fileStreams.end()) {
        // Solo gli stream file sono producer collegabili in questa prima
        // versione (createVirtualSink non è mai esercitato — vedi sopra).
        emit engineError(QStringLiteral("linkNodes: producer %1 non è uno stream attivo").arg(outputNodeId));
        return 0;
    }

    const QString deviceId = d->deviceIdFor(inputNodeId);
    if (deviceId.isEmpty()) {
        emit engineError(QStringLiteral("linkNodes: output %1 sconosciuto").arg(inputNodeId));
        return 0;
    }

    auto target = createRenderTarget(d.get(), deviceId, it->second->sampleRate, it->second->channelCount);
    if (!target) {
        emit engineError(QStringLiteral("Impossibile aprire il device %1 in modalità condivisa").arg(inputNodeId));
        return 0;
    }

    const uint32_t linkId = d->nextLinkId++;
    {
        QMutexLocker locker(&it->second->targetsMutex);
        it->second->renderTargets.push_back(std::move(target));
    }
    d->links.append(Impl::LinkEntry{ linkId, outputNodeId, inputNodeId });
    emit linkStateChanged(linkId, true);
    return linkId;
}

void WasapiEngine::unlinkNodes(uint32_t linkId)
{
    for (int i = 0; i < d->links.size(); ++i) {
        if (d->links[i].linkId != linkId)
            continue;

        const uint32_t producer = d->links[i].outputNodeId;
        const uint32_t target = d->links[i].inputNodeId;
        d->links.removeAt(i);

        auto it = d->fileStreams.find(producer);
        if (it != d->fileStreams.end()) {
            const QString deviceId = d->deviceIdFor(target);
            QMutexLocker locker(&it->second->targetsMutex);
            auto &targets = it->second->renderTargets;
            targets.erase(std::remove_if(targets.begin(), targets.end(),
                                          [&](const std::unique_ptr<Impl::RenderTarget> &t) {
                                              return t->deviceId == deviceId;
                                          }),
                          targets.end());
        }
        emit linkStateChanged(linkId, false);
        return;
    }
}

void WasapiEngine::setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled)
{
    if (!enabled) {
        auto it = d->keepAliveStreams.find(sinkNodeId);
        if (it != d->keepAliveStreams.end()) {
            it.value()->stopRequested.store(true);
            if (it.value()->thread)
                it.value()->thread->wait();
            d->keepAliveStreams.erase(it);
        }
        return;
    }

    if (d->keepAliveStreams.contains(sinkNodeId))
        return;

    const QString deviceId = d->deviceIdFor(sinkNodeId);
    if (deviceId.isEmpty())
        return;

    auto stream = std::make_shared<Impl::KeepAliveStream>();
    Impl *impl = d.get();
    auto keepAliveStream = stream;
    stream->thread.reset(QThread::create([impl, deviceId, keepAliveStream]() {
        // Vedi la stessa nota in fileStreamWorkerLoop: questo thread crea
        // esso stesso l'IAudioClient (Activate, dentro createRenderTarget)
        // e senza CoInitializeEx qui l'Activate fallisce con
        // CO_E_NOTINITIALIZED — createRenderTarget torna nullptr e il
        // keepalive smette silenziosamente di funzionare.
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto target = createRenderTarget(impl, deviceId, 44100.0, 2);
        if (!target) {
            if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        double phase = 0.0;
        double elapsed = 0.0;
        const double dt = 0.01; // stesso passo del ciclo (10ms di sleep)
        while (!keepAliveStream->stopRequested.load()) {
            QThread::msleep(10);
            UINT32 padding = 0;
            if (FAILED(target->client->GetCurrentPadding(&padding)))
                continue;
            const UINT32 available = target->bufferFrameCount > padding ? target->bufferFrameCount - padding : 0;
            if (available == 0)
                continue;

            std::vector<float> chunk(static_cast<size_t>(available) * target->channelCount, 0.0f);
            const double period = impl->keepAlivePeriodSeconds.load();
            const double duration = impl->keepAliveDurationSeconds.load();
            if (std::fmod(elapsed, period) < duration) {
                writeSineTone(chunk.data(), static_cast<int>(available), target->channelCount, phase,
                              impl->keepAliveFrequencyHz.load(), impl->keepAliveAmplitude.load(), 44100.0);
            }
            elapsed += dt * available / 441.0; // proporzionale ai frame realmente scritti

            BYTE *buffer = nullptr;
            if (SUCCEEDED(target->renderClient->GetBuffer(available, &buffer))) {
                std::memcpy(buffer, chunk.data(), chunk.size() * sizeof(float));
                target->renderClient->ReleaseBuffer(available, 0);
            }
        }
        target->client->Stop();
        if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
            CoUninitialize();
    }));
    stream->thread->start();

    d->keepAliveStreams.insert(sinkNodeId, stream);
}

void WasapiEngine::identifySink(uint32_t sinkNodeId)
{
    const QString deviceId = d->deviceIdFor(sinkNodeId);
    if (deviceId.isEmpty())
        return;

    Impl *impl = d.get();
    // Thread fire-and-forget, si distrugge da solo — stessa idea di
    // PipeWireEngine::identifySink/CoreAudioEngine::identifySink.
    QThread *thread = QThread::create([impl, deviceId]() {
        // Stesso motivo delle altre due note CoInitializeEx in questo file:
        // createRenderTarget chiama Activate() su questo thread stesso.
        const HRESULT comResult = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto target = createRenderTarget(impl, deviceId, 44100.0, 2);
        if (!target) {
            if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
                CoUninitialize();
            return;
        }

        constexpr double kBeepSeconds = 0.15;
        constexpr double kGapSeconds = 0.1;
        constexpr double kSampleRate = 44100.0;
        const int beepFrames = static_cast<int>(kBeepSeconds * kSampleRate);
        const int gapFrames = static_cast<int>(kGapSeconds * kSampleRate);
        const int totalFrames = beepFrames * 2 + gapFrames;

        std::vector<float> samples(static_cast<size_t>(totalFrames) * target->channelCount, 0.0f);
        double phase = 0.0;
        writeSineTone(samples.data(), beepFrames, target->channelCount, phase, 1000.0, 0.2f, kSampleRate);
        phase = 0.0;
        writeSineTone(samples.data() + static_cast<size_t>(beepFrames + gapFrames) * target->channelCount,
                       beepFrames, target->channelCount, phase, 1000.0, 0.2f, kSampleRate);

        int framesWritten = 0;
        while (framesWritten < totalFrames) {
            QThread::msleep(10);
            UINT32 padding = 0;
            if (FAILED(target->client->GetCurrentPadding(&padding)))
                break;
            const UINT32 available = target->bufferFrameCount > padding ? target->bufferFrameCount - padding : 0;
            const int toWrite = std::min<int>(static_cast<int>(available), totalFrames - framesWritten);
            if (toWrite <= 0)
                continue;

            BYTE *buffer = nullptr;
            if (FAILED(target->renderClient->GetBuffer(static_cast<UINT32>(toWrite), &buffer)))
                break;
            std::memcpy(buffer, samples.data() + static_cast<size_t>(framesWritten) * target->channelCount,
                        static_cast<size_t>(toWrite) * target->channelCount * sizeof(float));
            target->renderClient->ReleaseBuffer(static_cast<UINT32>(toWrite), 0);
            framesWritten += toWrite;
        }
        // Lascia esaurire il buffer già inviato prima di fermare il client.
        QThread::msleep(200);
        target->client->Stop();
        if (SUCCEEDED(comResult) || comResult == RPC_E_CHANGED_MODE)
            CoUninitialize();
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void WasapiEngine::setSinkMuted(uint32_t sinkNodeId, bool muted)
{
    const QString deviceId = d->deviceIdFor(sinkNodeId);
    if (deviceId.isEmpty())
        return;

    auto device = d->openDevice(deviceId);
    if (!device)
        return;

    ComPtr<IAudioEndpointVolume> volume;
    if (FAILED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void **>(volume.addr())))) {
        emit engineError(QStringLiteral("Impossibile mutare il sink %1").arg(sinkNodeId));
        return;
    }
    volume->SetMute(muted ? TRUE : FALSE, nullptr);
}

void WasapiEngine::setOutputDelayMs(uint32_t sinkNodeId, int delayMs)
{
    Q_UNUSED(sinkNodeId);
    Q_UNUSED(delayMs);
    emit engineError(QStringLiteral("Il ritardo di output non è ancora implementato su Windows"));
}

void WasapiEngine::setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName)
{
    Q_UNUSED(streamNodeId);
    Q_UNUSED(targetSinkNodeId);
    Q_UNUSED(targetSinkName);
    emit engineError(QStringLiteral("Spostare l'audio di un'app non è ancora implementato su Windows"));
}

void WasapiEngine::clearStreamTarget(uint32_t streamNodeId)
{
    Q_UNUSED(streamNodeId);
}

void WasapiEngine::calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId)
{
    Q_UNUSED(sinkNodeIdA);
    Q_UNUSED(sinkNodeIdB);
    Q_UNUSED(micNodeId);
    emit calibrationFinished(sinkNodeIdA, sinkNodeIdB, 0, false,
                              QStringLiteral("Calibrazione automatica non ancora implementata su Windows"));
}

void WasapiEngine::setKeepAlivePingFrequency(double hz) { d->keepAliveFrequencyHz.store(hz); }
void WasapiEngine::setKeepAlivePingAmplitude(float amplitude) { d->keepAliveAmplitude.store(amplitude); }
void WasapiEngine::setKeepAlivePingDuration(double seconds) { d->keepAliveDurationSeconds.store(seconds); }
void WasapiEngine::setKeepAlivePingPeriod(double seconds) { d->keepAlivePeriodSeconds.store(seconds); }
