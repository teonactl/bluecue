#include "CoreAudioEngine.h"

// NOTA GENERALE: vedi il commento in cima a CoreAudioEngine.h — questo file
// non è mai stato compilato (nessun SDK macOS disponibile in questo
// ambiente di sviluppo, che è Linux). Scritto seguendo la documentazione
// ufficiale delle API elencate sotto; da validare/aggiustare su un Mac
// reale (Xcode command line tools: `xcode-select --install`) prima di
// considerarlo pronto per l'uso.

#include <CoreAudio/CoreAudio.h>
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <Block.h>

#include <QDebug>
#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>
#include <QMap>
#include <QSet>
#include <QString>
#include <QTimer>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <vector>

namespace {

// Diagnostica per il primo giro di test su hardware reale (vedi la nota in
// cima al file: questo backend non era mai stato compilato prima). Molte
// chiamate CoreAudio/AudioToolbox qui sotto non controllavano l'OSStatus di
// ritorno, quindi un fallimento (es. l'aggregate device non accetta il sink
// Bluetooth, o il redirect dell'AudioQueue non si applica) restava
// completamente silenzioso — "si connette ma non si sente niente" senza
// nessun indizio nei log. logStatus stampa via qWarning il codice a 4
// caratteri (lo stesso formato che compare nei crash report/Console.app di
// Apple, es. '!obj'), solo quando lo stato NON è noErr.
void logStatus(const char *what, OSStatus status)
{
    if (status == noErr)
        return;
    char code[5] = { 0 };
    const uint32_t be = static_cast<uint32_t>(status);
    code[0] = static_cast<char>((be >> 24) & 0xFF);
    code[1] = static_cast<char>((be >> 16) & 0xFF);
    code[2] = static_cast<char>((be >> 8) & 0xFF);
    code[3] = static_cast<char>(be & 0xFF);
    const bool printable = std::isprint(static_cast<unsigned char>(code[0]))
        && std::isprint(static_cast<unsigned char>(code[1]))
        && std::isprint(static_cast<unsigned char>(code[2]))
        && std::isprint(static_cast<unsigned char>(code[3]));
    if (printable)
        qWarning("CoreAudioEngine: %s fallito, OSStatus=%d ('%s')", what, static_cast<int>(status), code);
    else
        qWarning("CoreAudioEngine: %s fallito, OSStatus=%d", what, static_cast<int>(status));
}

// Converte una QString in un CFStringRef posseduto dal chiamante (va
// rilasciato con CFRelease). Serve ovunque le API CoreAudio vogliono un
// CFStringRef (UID di device, nomi, chiavi di dizionario).
CFStringRef toCFString(const QString &s)
{
    return CFStringCreateWithCharacters(kCFAllocatorDefault,
                                         reinterpret_cast<const UniChar *>(s.utf16()),
                                         s.length());
}

QString fromCFString(CFStringRef s)
{
    if (!s)
        return {};
    const CFIndex length = CFStringGetLength(s);
    QString result(length, QChar());
    CFStringGetCharacters(s, CFRangeMake(0, length), reinterpret_cast<UniChar *>(result.data()));
    return result;
}

// Legge una property CoreAudio di dimensione nota a compile-time (il caso
// comune: UInt32, CFStringRef, AudioStreamBasicDescription, ...).
template <typename T>
bool getProperty(AudioObjectID object, AudioObjectPropertySelector selector,
                  AudioObjectPropertyScope scope, T *outValue)
{
    AudioObjectPropertyAddress addr{ selector, scope, kAudioObjectPropertyElementMain };
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(object, &addr, 0, nullptr, &size, outValue) == noErr;
}

QString deviceUidString(AudioObjectID device)
{
    CFStringRef uid = nullptr;
    if (!getProperty(device, kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, &uid) || !uid)
        return {};
    QString result = fromCFString(uid);
    CFRelease(uid);
    return result;
}

QString deviceNameString(AudioObjectID device)
{
    CFStringRef name = nullptr;
    if (!getProperty(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, &name) || !name)
        return {};
    QString result = fromCFString(name);
    CFRelease(name);
    return result;
}

// Numero di canali OUTPUT del device (0 = solo input, es. un microfono —
// va escluso dalla colonna Output). Somma i canali di ogni AudioBuffer
// nello stream-configuration lato output.
int deviceOutputChannelCount(AudioObjectID device)
{
    AudioObjectPropertyAddress addr{ kAudioDevicePropertyStreamConfiguration,
                                      kAudioDevicePropertyScopeOutput,
                                      kAudioObjectPropertyElementMain };
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(device, &addr, 0, nullptr, &size) != noErr || size == 0)
        return 0;

    std::vector<char> buffer(size);
    auto *list = reinterpret_cast<AudioBufferList *>(buffer.data());
    if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, list) != noErr)
        return 0;

    int channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
        channels += static_cast<int>(list->mBuffers[i].mNumberChannels);
    return channels;
}

bool deviceIsBluetooth(AudioObjectID device)
{
    UInt32 transportType = 0;
    if (!getProperty(device, kAudioDevicePropertyTransportType, kAudioObjectPropertyScopeGlobal, &transportType))
        return false;
    return transportType == kAudioDeviceTransportTypeBluetooth
        || transportType == kAudioDeviceTransportTypeBluetoothLE;
}

// Genera un tono sinusoidale nel buffer indicato (Float32 interleaved),
// avanzando *phase di 2*pi*frequency/sampleRate ad ogni campione — stessa
// idea del generatore keepalive di PipeWireEngine (fase persistente tra
// una callback e l'altra, per non avere discontinuità/click quando i
// parametri cambiano a runtime).
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
// Modello di routing: PipeWire espone un grafo di link arbitrari; CoreAudio
// no (un AudioQueue scrive su UN SOLO AudioDeviceID). Un "producer" (uno
// stream file, o in futuro un sink virtuale — vedi createVirtualSink) che
// deve raggiungere N output fisici insieme usa un Aggregate Device
// (AudioHardwareCreateAggregateDevice) creato al volo, il cui
// sub-device-list è l'insieme corrente degli output collegati — vedi
// syncAggregateDevice sotto. L'AudioQueue del producer punta SEMPRE
// all'aggregate (mai direttamente a un device fisico), anche quando c'è un
// solo output collegato: un solo code-path invece di due.
// -----------------------------------------------------------------------

struct CoreAudioEngine::Impl
{
    CoreAudioEngine *engine = nullptr;

    mutable QMutex mutex;
    QVector<AudioNode> nodes;           // snapshot pubblico (fisici + virtuali)
    QMap<uint32_t, QString> deviceUids; // nodeId -> UID stabile (chiave anche per il routing)
    QSet<uint32_t> ownAggregateIds;     // aggregate creati da noi: da NON far comparire come "nuovo hardware"

    dispatch_queue_t listenerQueue = nullptr;
    // Va tenuto in vita e riusato IDENTICO tra Add e Remove:
    // AudioObjectRemovePropertyListenerBlock deregistra per identità del
    // block, non per equivalenza funzionale — un secondo block literal
    // "vuoto" creato al momento in stop() (come faceva prima) non
    // deregistra nulla. Copiato sull'heap con Block_copy perché questo è un
    // file .cpp puro: senza ARC (che vale solo per Objective-C++), un
    // block literal locale non sopravvive di suo oltre la funzione che lo
    // crea (start()).
    AudioObjectPropertyListenerBlock deviceListChangeBlock = nullptr;

    struct FileStreamState
    {
        QString name;
        uint32_t nodeId = 0;
        AudioQueueRef queue = nullptr;
        std::vector<float> samples; // interleaved, canale = channelCount
        int channelCount = 1;
        double sampleRate = 44100.0;
        int64_t framePos = 0;
        std::atomic<bool> reverse{false};
        std::atomic<int> loopCount{-1}; // -1 = infinito, altrimenti ripetizioni residue
        std::atomic<bool> active{true};
        void *callbackContext = nullptr; // FileStreamCallbackContext*, da liberare con delete alla rimozione
    };
    // std::map, non QMap: il valore è move-only (std::unique_ptr) e QMap
    // (a differenza di std::map) richiede che il valore sia
    // copy-assegnabile anche solo per compilare insert() — scoperto in CI
    // su macOS (vedi il commento generale in cima al file).
    std::map<uint32_t, std::unique_ptr<FileStreamState>> fileStreams;
    uint32_t nextFileStreamId = 0x40000000; // ben oltre lo spazio dei veri AudioObjectID
    uint32_t nextVirtualSinkId = 0x50000000;

    struct LinkEntry { uint32_t linkId; uint32_t outputNodeId; uint32_t inputNodeId; };
    QVector<LinkEntry> links;
    uint32_t nextLinkId = 1;

    struct AggregateRouting { AudioDeviceID aggregateId = 0; };
    QMap<uint32_t, AggregateRouting> routing; // producerNodeId -> aggregate

    // Parametri keepalive condivisi (letti da ogni AudioQueue di keepalive
    // attiva, uno per sink — vedi setKeepAliveEnabled) — stessi default di
    // PipeWireEngine (18kHz, 0.15%, 60ms, ogni 20s).
    std::atomic<double> keepAliveFrequencyHz{18000.0};
    std::atomic<float> keepAliveAmplitude{0.0015f};
    std::atomic<double> keepAliveDurationSeconds{0.06};
    std::atomic<double> keepAlivePeriodSeconds{20.0};

    struct KeepAliveStream
    {
        AudioQueueRef queue = nullptr;
        double phase = 0.0;
        double elapsedSeconds = 0.0;
        double sampleRate = 44100.0;
        int channelCount = 2;
        Impl *impl = nullptr;
    };
    std::map<uint32_t, std::unique_ptr<KeepAliveStream>> keepAliveStreams; // vedi la nota su fileStreams sopra

    // --- Discovery ---

    void refreshDeviceList(bool emitSignals)
    {
        AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        UInt32 size = 0;
        if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
            return;
        const int count = static_cast<int>(size / sizeof(AudioObjectID));
        std::vector<AudioObjectID> ids(count);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size, ids.data()) != noErr)
            return;

        QSet<uint32_t> seen;
        QVector<AudioNode> discovered;
        QMap<uint32_t, QString> newUids;

        for (AudioObjectID id : ids) {
            if (ownAggregateIds.contains(id))
                continue; // non ri-annunciare i nostri stessi aggregate come hardware "nuovo"
            if (deviceOutputChannelCount(id) <= 0)
                continue; // device solo-input (microfoni, ...): fuori dalla colonna Output

            AudioNode node;
            node.id = static_cast<uint32_t>(id);
            node.name = deviceUidString(id);
            node.description = deviceNameString(id);
            node.kind = AudioNode::Kind::PhysicalSink;
            node.isBluetooth = deviceIsBluetooth(id);
            // CoreAudio non espone l'indirizzo MAC di un device Bluetooth
            // (a differenza di PipeWire/api.bluez5.address) — bluetoothMac
            // resta vuoto qui. La correlazione con la batteria esposta da
            // AppleBluetoothManager (quando disponibile) andrà quindi fatta
            // per NOME dispositivo invece che per MAC — vedi TODO in
            // PatchManager una volta integrato il backend Apple.

            discovered.append(node);
            newUids.insert(node.id, node.name);
            seen.insert(node.id);
        }

        QVector<AudioNode> added;
        QVector<uint32_t> removed;
        {
            QMutexLocker locker(&mutex);
            QSet<uint32_t> previous;
            for (const AudioNode &n : nodes)
                previous.insert(n.id);

            for (const AudioNode &n : discovered) {
                if (!previous.contains(n.id))
                    added.append(n);
            }
            for (uint32_t id : previous) {
                if (!seen.contains(id))
                    removed.append(id);
            }

            nodes = discovered;
            deviceUids = newUids;
        }

        if (!emitSignals)
            return;
        for (const AudioNode &n : added) {
            QMetaObject::invokeMethod(engine, [this, n]() { emit engine->nodeAdded(n); },
                                       Qt::QueuedConnection);
        }
        for (uint32_t id : removed) {
            QMetaObject::invokeMethod(engine, [this, id]() { emit engine->nodeRemoved(id); },
                                       Qt::QueuedConnection);
        }
    }

    QString uidFor(uint32_t nodeId) const
    {
        QMutexLocker locker(&mutex);
        return deviceUids.value(nodeId);
    }

    // --- Aggregate device (routing di un producer verso N output) ---

    // Ricostruisce l'insieme deduplicato di output collegati al producer
    // (da m_links) e sincronizza l'aggregate device corrispondente: lo crea
    // se non esiste ancora e serve, aggiorna il suo sub-device-list se
    // esiste già, lo distrugge se l'insieme è tornato vuoto.
    void syncAggregateForProducer(uint32_t producerNodeId)
    {
        QVector<uint32_t> targets;
        for (const LinkEntry &l : links) {
            if (l.outputNodeId == producerNodeId && !targets.contains(l.inputNodeId))
                targets.append(l.inputNodeId);
        }

        auto it = routing.find(producerNodeId);
        if (targets.isEmpty()) {
            if (it != routing.end() && it->aggregateId != 0) {
                ownAggregateIds.remove(it->aggregateId);
                AudioHardwareDestroyAggregateDevice(it->aggregateId);
                routing.erase(it);
            }
            return;
        }

        QVector<QString> uids;
        for (uint32_t t : targets) {
            const QString uid = uidFor(t);
            if (!uid.isEmpty())
                uids.append(uid);
        }
        if (uids.isEmpty()) {
            qWarning("CoreAudioEngine: nessun UID risolto per i %d output collegati al producer %u, "
                     "routing abbandonato silenziosamente", static_cast<int>(targets.size()), producerNodeId);
            return;
        }

        CFMutableArrayRef subDeviceList = CFArrayCreateMutable(kCFAllocatorDefault, uids.size(), &kCFTypeArrayCallBacks);
        for (const QString &uid : uids) {
            CFStringRef uidRef = toCFString(uid);
            CFMutableDictionaryRef sub = CFDictionaryCreateMutable(kCFAllocatorDefault, 1,
                                                                     &kCFTypeDictionaryKeyCallBacks,
                                                                     &kCFTypeDictionaryValueCallBacks);
            CFDictionarySetValue(sub, CFSTR(kAudioSubDeviceUIDKey), uidRef);
            CFArrayAppendValue(subDeviceList, sub);
            CFRelease(sub);
            CFRelease(uidRef);
        }

        if (it != routing.end() && it->aggregateId != 0) {
            qDebug("CoreAudioEngine: aggiorno sub-device-list dell'aggregate %u per producer %u (%d target)",
                   it->aggregateId, producerNodeId, static_cast<int>(uids.size()));
            // Aggregate già esistente: basta aggiornare il sub-device-list
            // "a caldo" — kAudioAggregateDevicePropertyFullSubDeviceList è
            // documentata come impostabile su un aggregate già creato.
            AudioObjectPropertyAddress addr{ kAudioAggregateDevicePropertyFullSubDeviceList,
                                              kAudioObjectPropertyScopeGlobal,
                                              kAudioObjectPropertyElementMain };
            const OSStatus status = AudioObjectSetPropertyData(it->aggregateId, &addr, 0, nullptr,
                                                                 sizeof(CFArrayRef), &subDeviceList);
            logStatus("AudioObjectSetPropertyData(FullSubDeviceList)", status);
            CFRelease(subDeviceList);
            return;
        }

        qDebug("CoreAudioEngine: creo un nuovo aggregate device per producer %u (%d target: %s)",
               producerNodeId, static_cast<int>(uids.size()), qPrintable(uids.join(", ")));

        // Nuovo aggregate.
        const QString aggregateUid = QStringLiteral("com.bluecue.route.%1").arg(producerNodeId);
        const QString aggregateName = QStringLiteral("BlueCue Route %1").arg(producerNodeId);
        CFStringRef uidRef = toCFString(aggregateUid);
        CFStringRef nameRef = toCFString(aggregateName);
        CFStringRef mainSubUidRef = toCFString(uids.first());

        CFMutableDictionaryRef description = CFDictionaryCreateMutable(kCFAllocatorDefault, 4,
                                                                         &kCFTypeDictionaryKeyCallBacks,
                                                                         &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey), uidRef);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), nameRef);
        // ATTENZIONE: il nome di questa chiave è quello che Apple usa
        // nell'SDK più recente (rinominato da "Master" a "Main", stesso
        // spirito di kAudioObjectPropertyElementMain sopra) — su un SDK
        // meno recente potrebbe servire kAudioAggregateDeviceMasterSubDeviceKey
        // invece. Verificare quale esiste nell'header AudioHardware.h
        // effettivamente installato al primo tentativo di build su Mac.
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), mainSubUidRef);
        const int isPrivate = 1;
        CFNumberRef isPrivateRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &isPrivate);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), isPrivateRef);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), subDeviceList);

        AudioDeviceID aggregateId = 0;
        const OSStatus status = AudioHardwareCreateAggregateDevice(description, &aggregateId);

        CFRelease(description);
        CFRelease(subDeviceList);
        CFRelease(isPrivateRef);
        CFRelease(mainSubUidRef);
        CFRelease(nameRef);
        CFRelease(uidRef);

        logStatus("AudioHardwareCreateAggregateDevice", status);
        if (status != noErr || aggregateId == 0) {
            QMetaObject::invokeMethod(engine, [this]() {
                emit engine->engineError(QStringLiteral("Creazione aggregate device fallita"));
            }, Qt::QueuedConnection);
            return;
        }
        qDebug("CoreAudioEngine: aggregate device %u creato per producer %u", aggregateId, producerNodeId);

        ownAggregateIds.insert(aggregateId);
        routing[producerNodeId] = AggregateRouting{ aggregateId };

        // Ripunta l'AudioQueue del producer (se è uno stream file) sul
        // nuovo aggregate — necessario solo alla creazione: il sub-device-
        // list può poi cambiare senza dover ripetere questo passo, perché
        // l'AudioDeviceID dell'aggregate stesso resta lo stesso.
        auto streamIt = fileStreams.find(producerNodeId);
        if (streamIt != fileStreams.end() && streamIt->second->queue) {
            // kAudioQueueProperty_CurrentDevice è impostabile SOLO a coda
            // ferma (documentato da Apple) — ma createFileStream() avvia
            // già la coda subito dopo averla creata, prima ancora che
            // esista un routing (il primo collegamento di un cue appena
            // aggiunto arriva sempre dopo, da qui). Senza lo Stop()/Start()
            // qui sotto questa AudioQueueSetProperty verrebbe
            // silenziosamente ignorata dal sistema (nessun controllo del
            // valore di ritorno la segnalerebbe) e il cue continuerebbe a
            // suonare sul device di default invece che sull'aggregate
            // appena creato: instradamento che non si applica mai al primo
            // collegamento.
            AudioQueueStop(streamIt->second->queue, true);
            CFStringRef aggregateUidRef = toCFString(aggregateUid);
            const OSStatus setDeviceStatus = AudioQueueSetProperty(streamIt->second->queue, kAudioQueueProperty_CurrentDevice,
                                                                     &aggregateUidRef, sizeof(aggregateUidRef));
            logStatus("AudioQueueSetProperty(CurrentDevice -> aggregate)", setDeviceStatus);
            CFRelease(aggregateUidRef);
            const OSStatus restartStatus = AudioQueueStart(streamIt->second->queue, nullptr);
            logStatus("AudioQueueStart (dopo redirect verso aggregate)", restartStatus);
            qDebug("CoreAudioEngine: producer %u redirezionato sull'aggregate %u", producerNodeId, aggregateId);
        } else {
            qWarning("CoreAudioEngine: producer %u non ha (più) uno stream file attivo: "
                     "l'aggregate %u resta collegato ai sink ma nessuna AudioQueue vi scrive sopra",
                     producerNodeId, aggregateId);
        }
    }
};

// -----------------------------------------------------------------------
// AudioQueue callback: rifornisce un buffer di uno stream file, onorando
// reverse/loop/pausa. Gira sul thread interno di AudioToolbox — NON quello
// Qt: tocca solo i campi atomici di FileStreamState e il buffer, mai
// direttamente strutture Qt (i segnali fileStreamLooped/fileStreamFinished
// vengono inoltrati via invokeMethod in coda, come in PipeWireEngine).
// -----------------------------------------------------------------------
namespace {
struct FileStreamCallbackContext
{
    CoreAudioEngine::Impl *impl = nullptr;
    CoreAudioEngine::Impl::FileStreamState *state = nullptr;
};

void fileStreamOutputCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
    auto *ctx = static_cast<FileStreamCallbackContext *>(inUserData);
    auto *state = ctx->state;
    auto *impl = ctx->impl;

    const int channels = state->channelCount;
    const int64_t totalFrames = state->channelCount > 0
        ? static_cast<int64_t>(state->samples.size() / channels) : 0;
    const int framesCapacity = static_cast<int>(inBuffer->mAudioDataBytesCapacity / (sizeof(float) * channels));
    auto *out = static_cast<float *>(inBuffer->mAudioData);

    bool looped = false;
    bool finished = false;

    if (totalFrames <= 0 || !state->active.load()) {
        std::fill(out, out + framesCapacity * channels, 0.0f);
    } else {
        const bool reverse = state->reverse.load();
        for (int i = 0; i < framesCapacity; ++i) {
            if (state->framePos < 0 || state->framePos >= totalFrames) {
                // Fine raggiunta (in un verso o nell'altro): riparte
                // dall'altro estremo, come PipeWireEngine.
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
            const float *src = &state->samples[state->framePos * channels];
            for (int c = 0; c < channels; ++c)
                out[i * channels + c] = src[c];
            state->framePos += reverse ? -1 : 1;

            if (finished) {
                // Silenzio per il resto di QUESTO buffer dopo l'ultimo giro.
                for (int j = i + 1; j < framesCapacity; ++j)
                    for (int c = 0; c < channels; ++c)
                        out[j * channels + c] = 0.0f;
                break;
            }
        }
    }

    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);

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

// Callback per il tono keepalive: attivo solo per i primi
// keepAliveDurationSeconds di ogni periodo keepAlivePeriodSeconds,
// silenzio nel resto — stesso concept del ping periodico di PipeWireEngine.
void keepAliveOutputCallback(void *inUserData, AudioQueueRef inAQ, AudioQueueBufferRef inBuffer)
{
    auto *stream = static_cast<CoreAudioEngine::Impl::KeepAliveStream *>(inUserData);
    auto *impl = stream->impl;
    const int channels = stream->channelCount;
    const int frames = static_cast<int>(inBuffer->mAudioDataBytesCapacity / (sizeof(float) * channels));
    auto *out = static_cast<float *>(inBuffer->mAudioData);

    const double period = impl->keepAlivePeriodSeconds.load();
    const double duration = impl->keepAliveDurationSeconds.load();
    const double freq = impl->keepAliveFrequencyHz.load();
    const float amp = impl->keepAliveAmplitude.load();
    const double dt = 1.0 / stream->sampleRate;

    for (int i = 0; i < frames; ++i) {
        const bool tonePhase = std::fmod(stream->elapsedSeconds, period) < duration;
        float sample = 0.0f;
        if (tonePhase) {
            sample = amp * static_cast<float>(std::sin(stream->phase));
            stream->phase += 2.0 * std::numbers::pi * freq / stream->sampleRate;
            if (stream->phase > 2.0 * std::numbers::pi)
                stream->phase -= 2.0 * std::numbers::pi;
        }
        for (int c = 0; c < channels; ++c)
            out[i * channels + c] = sample;
        stream->elapsedSeconds += dt;
    }

    inBuffer->mAudioDataByteSize = inBuffer->mAudioDataBytesCapacity;
    AudioQueueEnqueueBuffer(inAQ, inBuffer, 0, nullptr);
}
} // namespace

CoreAudioEngine::CoreAudioEngine(QObject *parent)
    : AudioEngine(parent)
    , d(std::make_unique<Impl>())
{
    d->engine = this;
}

CoreAudioEngine::~CoreAudioEngine()
{
    stop();
}

bool CoreAudioEngine::start()
{
    d->listenerQueue = dispatch_queue_create("com.bluecue.coreaudio.listener", DISPATCH_QUEUE_SERIAL);

    // emitSignals=true, non false: PatchManager scopre OGNI sink (incluso
    // quelli già presenti all'avvio) esclusivamente tramite nodeAdded, non
    // con una chiamata separata tipo "dammi la lista attuale" (vedi il
    // commento in PatchManager.cpp sopra la connessione a nodeAdded). Il
    // "nessun listener QML ancora collegato" di prima era una motivazione
    // sbagliata: l'emit passa comunque per QMetaObject::invokeMethod con
    // Qt::QueuedConnection, quindi la consegna avviene solo dopo l'avvio del
    // loop eventi Qt — quando PatchManager si è già collegato da un pezzo.
    // Stesso identico bug reale corretto in WasapiEngine::start().
    d->refreshDeviceList(true);

    AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                      kAudioObjectPropertyScopeGlobal,
                                      kAudioObjectPropertyElementMain };
    Impl *impl = d.get();
    d->deviceListChangeBlock = Block_copy(^(UInt32, const AudioObjectPropertyAddress *) {
        impl->refreshDeviceList(true);
    });
    AudioObjectAddPropertyListenerBlock(kAudioObjectSystemObject, &addr, d->listenerQueue,
                                         d->deviceListChangeBlock);
    return true;
}

void CoreAudioEngine::stop()
{
    if (d->listenerQueue) {
        AudioObjectPropertyAddress addr{ kAudioHardwarePropertyDevices,
                                          kAudioObjectPropertyScopeGlobal,
                                          kAudioObjectPropertyElementMain };
        if (d->deviceListChangeBlock) {
            AudioObjectRemovePropertyListenerBlock(kAudioObjectSystemObject, &addr, d->listenerQueue,
                                                    d->deviceListChangeBlock);
            Block_release(d->deviceListChangeBlock);
            d->deviceListChangeBlock = nullptr;
        }
        dispatch_release(d->listenerQueue);
        d->listenerQueue = nullptr;
    }

    for (auto &[id, entry] : d->fileStreams) {
        if (entry->queue)
            AudioQueueDispose(entry->queue, true);
        delete static_cast<FileStreamCallbackContext *>(entry->callbackContext);
    }
    d->fileStreams.clear();

    for (auto &[id, entry] : d->keepAliveStreams) {
        if (entry->queue)
            AudioQueueDispose(entry->queue, true);
    }
    d->keepAliveStreams.clear();

    for (auto it = d->routing.begin(); it != d->routing.end(); ++it) {
        if (it->aggregateId != 0)
            AudioHardwareDestroyAggregateDevice(it->aggregateId);
    }
    d->routing.clear();
}

QVector<AudioNode> CoreAudioEngine::nodes() const
{
    QMutexLocker locker(&d->mutex);
    return d->nodes;
}

void CoreAudioEngine::createVirtualSink(const QString &name, const QString &description)
{
    // Non usato dall'attuale PatchManager (routing sempre diretto
    // sorgente->output fisici, mai passando da un sink virtuale — vedi
    // il commento in cima a CoreAudioEngine.h) — implementato per
    // completezza dell'interfaccia e per un eventuale uso futuro, MA MAI
    // ESERCITATO: un aggregate device con sub-device-list vuoto potrebbe
    // essere rifiutato dal sistema finché non gli si collega almeno un
    // output reale (vedi linkNodes) — da verificare su Mac reale.
    const uint32_t nodeId = d->nextVirtualSinkId++;
    AudioNode node;
    node.id = nodeId;
    node.name = QStringLiteral("bluecue.virtual.%1").arg(nodeId);
    node.description = description.isEmpty() ? name : description;
    node.kind = AudioNode::Kind::VirtualSink;
    {
        QMutexLocker locker(&d->mutex);
        d->nodes.append(node);
        d->deviceUids.insert(nodeId, node.name);
    }
    QMetaObject::invokeMethod(this, [this, node]() { emit nodeAdded(node); }, Qt::QueuedConnection);
}

void CoreAudioEngine::removeVirtualSink(uint32_t nodeId)
{
    auto it = d->routing.find(nodeId);
    if (it != d->routing.end()) {
        if (it->aggregateId != 0) {
            d->ownAggregateIds.remove(it->aggregateId);
            AudioHardwareDestroyAggregateDevice(it->aggregateId);
        }
        d->routing.erase(it);
    }
    {
        QMutexLocker locker(&d->mutex);
        d->nodes.removeIf([nodeId](const AudioNode &n) { return n.id == nodeId; });
        d->deviceUids.remove(nodeId);
    }
    QMetaObject::invokeMethod(this, [this, nodeId]() { emit nodeRemoved(nodeId); }, Qt::QueuedConnection);
}

QString CoreAudioEngine::createFileStream(const QString &filePath, const QString &description,
                                           int loopCount, bool reverse)
{
    CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
                                                  toCFString(filePath), kCFURLPOSIXPathStyle, false);
    ExtAudioFileRef audioFile = nullptr;
    if (ExtAudioFileOpenURL(url, &audioFile) != noErr || !audioFile) {
        CFRelease(url);
        emit engineError(QStringLiteral("Impossibile aprire il file audio: %1").arg(filePath));
        return {};
    }
    CFRelease(url);

    AudioStreamBasicDescription fileFormat{};
    UInt32 size = sizeof(fileFormat);
    logStatus("ExtAudioFileGetProperty(FileDataFormat)",
              ExtAudioFileGetProperty(audioFile, kExtAudioFileProperty_FileDataFormat, &size, &fileFormat));

    AudioStreamBasicDescription clientFormat{};
    clientFormat.mSampleRate = fileFormat.mSampleRate;
    clientFormat.mFormatID = kAudioFormatLinearPCM;
    clientFormat.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    clientFormat.mChannelsPerFrame = fileFormat.mChannelsPerFrame > 0 ? fileFormat.mChannelsPerFrame : 2;
    clientFormat.mBitsPerChannel = 32;
    clientFormat.mFramesPerPacket = 1;
    clientFormat.mBytesPerFrame = sizeof(float) * clientFormat.mChannelsPerFrame;
    clientFormat.mBytesPerPacket = clientFormat.mBytesPerFrame;
    logStatus("ExtAudioFileSetProperty(ClientDataFormat)",
              ExtAudioFileSetProperty(audioFile, kExtAudioFileProperty_ClientDataFormat, sizeof(clientFormat), &clientFormat));

    SInt64 totalFrames = 0;
    size = sizeof(totalFrames);
    logStatus("ExtAudioFileGetProperty(FileLengthFrames)",
              ExtAudioFileGetProperty(audioFile, kExtAudioFileProperty_FileLengthFrames, &size, &totalFrames));
    qDebug("CoreAudioEngine: file '%s' aperto — %lld frame, %d canali, %.0f Hz",
           qPrintable(filePath), static_cast<long long>(totalFrames),
           static_cast<int>(clientFormat.mChannelsPerFrame), clientFormat.mSampleRate);

    auto state = std::make_unique<Impl::FileStreamState>();
    state->channelCount = static_cast<int>(clientFormat.mChannelsPerFrame);
    state->sampleRate = clientFormat.mSampleRate;
    state->samples.resize(static_cast<size_t>(totalFrames) * state->channelCount);

    // Caricamento per intero in memoria (non streaming): stessa scelta di
    // PipeWireEngine, necessaria per supportare la riproduzione
    // all'indietro (accesso casuale ai campioni) — vedi il commento in
    // AudioEngine::createFileStream.
    UInt32 framesRemaining = static_cast<UInt32>(totalFrames);
    UInt32 framesReadSoFar = 0;
    while (framesRemaining > 0) {
        AudioBufferList bufferList;
        bufferList.mNumberBuffers = 1;
        bufferList.mBuffers[0].mNumberChannels = clientFormat.mChannelsPerFrame;
        bufferList.mBuffers[0].mDataByteSize = static_cast<UInt32>(framesRemaining * clientFormat.mBytesPerFrame);
        bufferList.mBuffers[0].mData = state->samples.data() + static_cast<size_t>(framesReadSoFar) * state->channelCount;

        UInt32 framesToRead = framesRemaining;
        if (ExtAudioFileRead(audioFile, &framesToRead, &bufferList) != noErr || framesToRead == 0)
            break;
        framesReadSoFar += framesToRead;
        framesRemaining -= framesToRead;
    }
    ExtAudioFileDispose(audioFile);

    const uint32_t nodeId = d->nextFileStreamId++;
    state->nodeId = nodeId;
    state->name = QStringLiteral("bluecue.file.%1").arg(nodeId);
    state->loopCount.store(loopCount);
    state->reverse.store(reverse);
    state->framePos = reverse ? static_cast<int64_t>(totalFrames) - 1 : 0;

    auto *ctx = new FileStreamCallbackContext{ d.get(), state.get() };
    AudioQueueRef queue = nullptr;
    if (AudioQueueNewOutput(&clientFormat, fileStreamOutputCallback, ctx, nullptr, nullptr, 0, &queue) != noErr) {
        delete ctx;
        emit engineError(QStringLiteral("Creazione AudioQueue fallita per: %1").arg(filePath));
        return {};
    }
    state->queue = queue;
    state->callbackContext = ctx;

    constexpr int kBufferCount = 3;
    constexpr UInt32 kBufferFrames = 4096;
    const UInt32 bufferByteSize = kBufferFrames * clientFormat.mBytesPerFrame;
    for (int i = 0; i < kBufferCount; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        logStatus("AudioQueueAllocateBuffer", AudioQueueAllocateBuffer(queue, bufferByteSize, &buffer));
        fileStreamOutputCallback(ctx, queue, buffer);
    }
    logStatus("AudioQueueStart (device di default, prima di un eventuale routing)", AudioQueueStart(queue, nullptr));

    const QString streamName = state->name;
    {
        QMutexLocker locker(&d->mutex);
        d->deviceUids.insert(nodeId, streamName); // usato solo internamente per coerenza, non è un output collegabile per UID
    }
    d->fileStreams.emplace(nodeId, std::move(state));

    AudioNode node;
    node.id = nodeId;
    node.name = streamName;
    node.description = description;
    node.kind = AudioNode::Kind::Source;
    QMetaObject::invokeMethod(this, [this, node]() { emit nodeAdded(node); }, Qt::QueuedConnection);

    return streamName;
}

void CoreAudioEngine::setFileStreamLoopCount(uint32_t nodeId, int loopCount)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->loopCount.store(loopCount);
}

void CoreAudioEngine::setFileStreamReverse(uint32_t nodeId, bool reverse)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->reverse.store(reverse);
}

void CoreAudioEngine::removeFileStream(uint32_t nodeId)
{
    auto it = d->fileStreams.find(nodeId);
    if (it == d->fileStreams.end())
        return;

    // Rimuove prima ogni link/aggregate che puntava a questo producer.
    QVector<uint32_t> linkIdsToRemove;
    for (const auto &l : d->links) {
        if (l.outputNodeId == nodeId)
            linkIdsToRemove.append(l.linkId);
    }
    for (uint32_t linkId : linkIdsToRemove)
        unlinkNodes(linkId);

    if (it->second->queue)
        AudioQueueDispose(it->second->queue, true);
    delete static_cast<FileStreamCallbackContext *>(it->second->callbackContext);
    d->fileStreams.erase(it);

    {
        QMutexLocker locker(&d->mutex);
        d->deviceUids.remove(nodeId);
    }
    emit nodeRemoved(nodeId);
}

void CoreAudioEngine::setFileStreamActive(uint32_t nodeId, bool active)
{
    auto it = d->fileStreams.find(nodeId);
    if (it != d->fileStreams.end())
        it->second->active.store(active);
}

uint32_t CoreAudioEngine::linkNodes(uint32_t outputNodeId, uint32_t inputNodeId)
{
    qDebug("CoreAudioEngine::linkNodes producer=%u -> sink=%u", outputNodeId, inputNodeId);
    const uint32_t linkId = d->nextLinkId++;
    d->links.append(Impl::LinkEntry{ linkId, outputNodeId, inputNodeId });
    d->syncAggregateForProducer(outputNodeId);
    emit linkStateChanged(linkId, true);
    return linkId;
}

void CoreAudioEngine::unlinkNodes(uint32_t linkId)
{
    for (int i = 0; i < d->links.size(); ++i) {
        if (d->links[i].linkId == linkId) {
            const uint32_t producer = d->links[i].outputNodeId;
            d->links.removeAt(i);
            d->syncAggregateForProducer(producer);
            emit linkStateChanged(linkId, false);
            return;
        }
    }
}

void CoreAudioEngine::setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled)
{
    if (!enabled) {
        auto it = d->keepAliveStreams.find(sinkNodeId);
        if (it != d->keepAliveStreams.end()) {
            if (it->second->queue)
                AudioQueueDispose(it->second->queue, true);
            d->keepAliveStreams.erase(it);
        }
        return;
    }

    if (d->keepAliveStreams.contains(sinkNodeId))
        return; // già attivo

    const QString uid = d->uidFor(sinkNodeId);
    if (uid.isEmpty())
        return;

    auto stream = std::make_unique<Impl::KeepAliveStream>();
    stream->impl = d.get();
    stream->sampleRate = 44100.0;
    stream->channelCount = 2;

    AudioStreamBasicDescription format{};
    format.mSampleRate = stream->sampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = stream->channelCount;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * format.mChannelsPerFrame;
    format.mBytesPerPacket = format.mBytesPerFrame;

    AudioQueueRef queue = nullptr;
    if (AudioQueueNewOutput(&format, keepAliveOutputCallback, stream.get(), nullptr, nullptr, 0, &queue) != noErr) {
        emit engineError(QStringLiteral("Creazione keepalive fallita per il sink %1").arg(sinkNodeId));
        return;
    }
    stream->queue = queue;

    CFStringRef uidRef = toCFString(uid);
    AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &uidRef, sizeof(uidRef));
    CFRelease(uidRef);

    constexpr int kBufferCount = 2;
    constexpr UInt32 kBufferFrames = 4096;
    const UInt32 bufferByteSize = kBufferFrames * format.mBytesPerFrame;
    for (int i = 0; i < kBufferCount; ++i) {
        AudioQueueBufferRef buffer = nullptr;
        AudioQueueAllocateBuffer(queue, bufferByteSize, &buffer);
        keepAliveOutputCallback(stream.get(), queue, buffer);
    }
    AudioQueueStart(queue, nullptr);

    d->keepAliveStreams.emplace(sinkNodeId, std::move(stream));
}

void CoreAudioEngine::identifySink(uint32_t sinkNodeId)
{
    const QString uid = d->uidFor(sinkNodeId);
    if (uid.isEmpty())
        return;

    // Stream dedicato, due bip udibili, si autodistrugge subito dopo —
    // stessa idea di PipeWireEngine::identifySink. Implementazione minima:
    // genera qui in memoria due brevi toni sinusoidali separati da un
    // silenzio, poi li riproduce con un AudioQueue che si smonta da solo a
    // fine riproduzione (AudioQueueAddPropertyListener su
    // kAudioQueueProperty_IsRunning, che passa a false a riproduzione
    // conclusa).
    constexpr double kBeepSeconds = 0.15;
    constexpr double kGapSeconds = 0.1;
    constexpr double kSampleRate = 44100.0;
    constexpr int kChannels = 2;
    const int beepFrames = static_cast<int>(kBeepSeconds * kSampleRate);
    const int gapFrames = static_cast<int>(kGapSeconds * kSampleRate);
    const int totalFrames = beepFrames * 2 + gapFrames;

    auto samples = std::make_shared<std::vector<float>>(static_cast<size_t>(totalFrames) * kChannels, 0.0f);
    double phase = 0.0;
    writeSineTone(samples->data(), beepFrames, kChannels, phase, 1000.0, 0.2f, kSampleRate);
    phase = 0.0;
    writeSineTone(samples->data() + static_cast<size_t>(beepFrames + gapFrames) * kChannels,
                   beepFrames, kChannels, phase, 1000.0, 0.2f, kSampleRate);

    AudioStreamBasicDescription format{};
    format.mSampleRate = kSampleRate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    format.mChannelsPerFrame = kChannels;
    format.mBitsPerChannel = 32;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float) * kChannels;
    format.mBytesPerPacket = format.mBytesPerFrame;

    AudioQueueRef queue = nullptr;
    if (AudioQueueNewOutput(&format, [](void *, AudioQueueRef q, AudioQueueBufferRef) {
            // Un solo buffer, una sola riproduzione: alla callback
            // successiva (buffer già consumato) ferma e distrugge tutto —
            // MA non da dentro la callback stessa (thread interno di
            // AudioToolbox): Stop/Dispose sincroni lì dentro sono un
            // pattern noto per rischiare un deadlock, vanno spostati su
            // un'altra coda.
            dispatch_async(dispatch_get_main_queue(), ^{
                AudioQueueStop(q, true);
                AudioQueueDispose(q, true);
            });
        }, nullptr, nullptr, nullptr, 0, &queue) != noErr) {
        return;
    }

    CFStringRef uidRef = toCFString(uid);
    AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice, &uidRef, sizeof(uidRef));
    CFRelease(uidRef);

    AudioQueueBufferRef buffer = nullptr;
    const UInt32 byteSize = static_cast<UInt32>(totalFrames * format.mBytesPerFrame);
    AudioQueueAllocateBuffer(queue, byteSize, &buffer);
    std::memcpy(buffer->mAudioData, samples->data(), byteSize);
    buffer->mAudioDataByteSize = byteSize;
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
    AudioQueueStart(queue, nullptr);
}

void CoreAudioEngine::setSinkMuted(uint32_t sinkNodeId, bool muted)
{
    AudioObjectPropertyAddress addr{ kAudioDevicePropertyMute, kAudioDevicePropertyScopeOutput,
                                      kAudioObjectPropertyElementMain };
    UInt32 value = muted ? 1 : 0;
    if (AudioObjectSetPropertyData(sinkNodeId, &addr, 0, nullptr, sizeof(value), &value) != noErr) {
        emit engineError(QStringLiteral("Impossibile mutare il sink %1 (il device potrebbe non supportare il muto hardware)")
                          .arg(sinkNodeId));
    }
}

void CoreAudioEngine::setOutputDelayMs(uint32_t sinkNodeId, int delayMs)
{
    Q_UNUSED(sinkNodeId);
    Q_UNUSED(delayMs);
    emit engineError(QStringLiteral("Il ritardo di output non è ancora implementato su macOS"));
}

void CoreAudioEngine::setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName)
{
    Q_UNUSED(streamNodeId);
    Q_UNUSED(targetSinkNodeId);
    Q_UNUSED(targetSinkName);
    emit engineError(QStringLiteral("Spostare l'audio di un'app non è ancora implementato su macOS"));
}

void CoreAudioEngine::clearStreamTarget(uint32_t streamNodeId)
{
    Q_UNUSED(streamNodeId);
}

void CoreAudioEngine::calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId)
{
    Q_UNUSED(sinkNodeIdA);
    Q_UNUSED(sinkNodeIdB);
    Q_UNUSED(micNodeId);
    emit calibrationFinished(sinkNodeIdA, sinkNodeIdB, 0, false,
                              QStringLiteral("Calibrazione automatica non ancora implementata su macOS"));
}

void CoreAudioEngine::setKeepAlivePingFrequency(double hz)
{
    d->keepAliveFrequencyHz.store(hz);
}

void CoreAudioEngine::setKeepAlivePingAmplitude(float amplitude)
{
    d->keepAliveAmplitude.store(amplitude);
}

void CoreAudioEngine::setKeepAlivePingDuration(double seconds)
{
    d->keepAliveDurationSeconds.store(seconds);
}

void CoreAudioEngine::setKeepAlivePingPeriod(double seconds)
{
    d->keepAlivePeriodSeconds.store(seconds);
}
