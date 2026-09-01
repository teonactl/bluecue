#include "PipeWireEngine.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <spa/utils/hook.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <sndfile.h>

#include <QMutex>
#include <QMutexLocker>
#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QSet>
#include <QMap>
#include <algorithm>
#include <atomic>
#include <numbers>
#include <cmath>

// -----------------------------------------------------------------------
// Discovery: nodi E porte
// -----------------------------------------------------------------------
// Oltre ai nodi (Audio/Sink, Audio/Source) tracciamo ora anche le loro
// PORTE (PW_TYPE_INTERFACE_Port). Ogni porta appartiene a un nodo (tramite
// la proprietà PW_KEY_NODE_ID) ed ha una direzione (PW_KEY_PORT_DIRECTION:
// "in" o "out"). Per collegare due nodi bisogna collegare le loro porte a
// coppie: l'uscita di un nodo Source con l'ingresso di un nodo Sink.
//
// Il link vero e proprio si crea con pw_core_create_object(..., "link",
// PW_TYPE_INTERFACE_Link, ...), passando gli id di porta (non di nodo)
// come "link.output.port" e "link.input.port" nelle properties.
//
// Le callback arrivano SUL THREAD DI PIPEWIRE: ogni emit verso l'esterno
// passa da QMetaObject::invokeMethod(..., Qt::QueuedConnection).
// -----------------------------------------------------------------------

namespace {

AudioNode::Kind kindFromMediaClass(const char *mediaClass)
{
    if (!mediaClass)
        return AudioNode::Kind::Unknown;
    if (std::strcmp(mediaClass, "Audio/Sink") == 0)
        return AudioNode::Kind::PhysicalSink;
    if (std::strcmp(mediaClass, "Audio/Source") == 0)
        return AudioNode::Kind::Source;
    return AudioNode::Kind::Unknown;
}

} // namespace

// Porta PipeWire tracciata internamente: non esposta all'esterno di
// PipeWireEngine, serve solo per la creazione dei link.
struct TrackedPort
{
    uint32_t id = 0;         // id globale della porta
    uint32_t nodeId = 0;     // nodo a cui appartiene
    bool isOutput = false;   // true = "out" (sorgente), false = "in" (destinazione)
    int channelIndex = 0;    // ordine di scoperta, usato per abbinare FL<->FL, FR<->FR
};

struct PipeWireEngine::Impl
{
    PipeWireEngine *q = nullptr;

    pw_thread_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;

    spa_hook registryListener{};

    mutable QMutex nodesMutex;
    QVector<AudioNode> nodes;
    QVector<TrackedPort> ports;

    // L'annuncio iniziale (registry "global") di un sink Bluetooth NON
    // include ancora device.api/api.bluez5.* — queste proprietà vengono
    // aggiunte al nodo solo un istante dopo, tramite un aggiornamento delle
    // sue info (non un nuovo "global"). Senza ascoltare quell'aggiornamento,
    // isBluetooth risultava sempre false per ogni sink Bluetooth, anche già
    // connesso al momento dell'avvio — bug scoperto perché il keepalive non
    // si agganciava mai a nessuna cassa reale. Per ogni nodo Sink ci si
    // aggancia quindi anche alle sue info (pw_node_add_listener) per
    // scoprire device.api quando arriva ed emettere nodeUpdated di
    // conseguenza. Solo per il thread PipeWire: non serve nodesMutex.
    struct SinkWatch
    {
        pw_proxy *proxy = nullptr;
        spa_hook listener{};
        // A differenza di pw_node_info (che porta il proprio "id"), la
        // callback "param" dei pw_node_events NON riceve l'id del nodo —
        // serve quindi passare il contesto per-watch (questa struct) come
        // dato utente del listener invece del solo Impl condiviso, per
        // sapere QUALE nodo ha generato l'evento arrivato.
        Impl *owner = nullptr;
        uint32_t nodeId = 0;
    };
    QMap<uint32_t, SinkWatch *> sinkWatches;

    // Traccia i link creati da noi: linkId locale (assegnato da noi in modo
    // incrementale) -> id dei pw_proxy dei link PipeWire reali sottostanti
    // (un collegamento stereo produce 2 link fisici: FL->FL, FR->FR).
    struct ActiveLink
    {
        uint32_t localId = 0;
        QVector<pw_proxy *> proxies;
    };
    QVector<ActiveLink> activeLinks;
    uint32_t nextLocalLinkId = 1;

    QVector<uint32_t> ownedVirtualSinkIds;

    // Stream di playback file attivo (uno per ogni input "file audio"
    // aggiunto dall'utente). nodeId resta 0 finché PipeWire non lo comunica
    // tramite pw_stream_get_node_id() nella callback state_changed: prima di
    // allora removeFileStream() non può ancora individuarlo.
    struct FileStream
    {
        Impl *engineImpl = nullptr;
        pw_stream *stream = nullptr;
        spa_hook streamListener{};
        int channels = 0;
        uint32_t nodeId = 0;

        // File intero decodificato in memoria (interleaved) al momento della
        // creazione: necessario per poter leggere all'indietro, cosa che
        // libsndfile non supporta in streaming sequenziale.
        QVector<float> samples;
        uint64_t totalFrames = 0;
        uint64_t playPosition = 0; // frame corrente, letto/scritto SOLO dal thread audio

        // reverseRequested/loopCountRequested sono impostati dal thread Qt
        // (setFileStreamReverse/setFileStreamLoopCount) e letti dal thread
        // audio ad ogni process(): std::atomic invece di nodesMutex per non
        // introdurre un lock nel percorso realtime.
        std::atomic<bool> reverseRequested{false};
        std::atomic<int> loopCountRequested{-1}; // -1 = infinito

        int loopsPlayed = 0;              // letto/scritto SOLO dal thread audio
        std::atomic<bool> finished{false}; // true quando loopCountRequested è stato raggiunto
        bool finishedNotified = false;     // letto/scritto SOLO dal thread audio: evita notifiche ripetute
    };
    QVector<FileStream *> fileStreams;
    uint32_t nextFileStreamId = 1;

    // --- Keepalive Bluetooth: generatore unico e condiviso, un breve tono
    // periodico a volume molto basso verso ogni sink "da tenere sveglio",
    // per evitare che i dispositivi Bluetooth vadano in stand-by per
    // inattività audio. Niente PW_KEY_MEDIA_CLASS in fase di creazione: il
    // nodo risultante viene classificato Kind::Unknown da onGlobal() e non
    // emette mai nodeAdded, quindi non compare come sorgente selezionabile
    // nella UI (PatchManager non lo vede).
    static constexpr const char *kKeepAliveNodeName = "bluecue.keepalive";

    pw_stream *keepAliveStream = nullptr;
    spa_hook keepAliveStreamListener{};
    int keepAliveChannels = 2;
    uint32_t keepAliveRate = 48000;
    uint64_t keepAliveSampleClock = 0;
    uint32_t keepAliveGeneratorNodeId = 0; // protetto da nodesMutex, 0 = non ancora noto

    // Parametri del ping regolabili a runtime dal thread Qt (Impostazioni),
    // letti dal thread audio in onKeepAliveProcess — std::atomic invece di
    // nodesMutex per non introdurre un lock nel percorso realtime, stesso
    // principio già usato per reverseRequested/loopCountRequested di
    // FileStream. Richiesto esplicitamente dall'utente: un modo di
    // sperimentare durata/ampiezza/frequenza senza dover ricompilare ad
    // ogni tentativo, e provare una frequenza ultrasonica (inaudibile)
    // invece di rumore bianco udibile a basso volume.
    std::atomic<double> keepAlivePingFrequencyHz{18000.0};
    std::atomic<float> keepAlivePingAmplitude{0.0015f};
    std::atomic<double> keepAlivePingDurationSeconds{0.06};
    std::atomic<double> keepAlivePingPeriodSeconds{20.0};
    double keepAlivePingPhase = 0.0; // fase corrente del tono in radianti — SOLO thread audio

    QSet<uint32_t> keepAliveDesiredSinks;      // stato voluto (Qt thread)
    QMap<uint32_t, uint32_t> keepAliveLinkIds; // sinkNodeId -> localLinkId (Qt thread)

    // --- Identify: due bip chiaramente udibili su un sink specifico, per
    // capire fisicamente a quale altoparlante corrisponde una voce della
    // colonna Output. A differenza del keepalive non è condiviso/persistente:
    // ogni chiamata crea un proprio stream breve che si autodistrugge da
    // solo (via QTimer sul thread Qt) subito dopo aver suonato — una lista
    // perché più chiamate ravvicinate su sink diversi devono poter
    // convivere.
    struct IdentifyStream
    {
        Impl *engineImpl = nullptr;
        pw_stream *stream = nullptr;
        spa_hook listener{};
        uint32_t nodeId = 0;       // protetto da nodesMutex, 0 = non ancora noto
        uint32_t targetSinkId = 0;
        uint64_t sampleClock = 0;
        int channels = 2;
        uint32_t rate = 48000;
        bool linked = false;       // protetto da nodesMutex
    };
    QVector<IdentifyStream *> identifyStreams;

    void ensureKeepAliveStreamStarted()
    {
        if (keepAliveStream || !loop || !core)
            return;

        pw_thread_loop_lock(loop);

        pw_properties *props = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Music",
            PW_KEY_NODE_NAME, kKeepAliveNodeName,
            PW_KEY_NODE_DESCRIPTION, "BT Multizone keepalive",
            nullptr);

        keepAliveStream = pw_stream_new(core, kKeepAliveNodeName, props);
        if (!keepAliveStream) {
            pw_thread_loop_unlock(loop);
            emit q->engineError(QStringLiteral("Impossibile creare lo stream di keepalive Bluetooth"));
            return;
        }

        pw_stream_add_listener(keepAliveStream, &keepAliveStreamListener, &keepAliveStreamEvents, this);

        uint8_t podBuffer[1024];
        spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
        spa_audio_info_raw audioInfo{};
        audioInfo.format = SPA_AUDIO_FORMAT_F32;
        audioInfo.channels = static_cast<uint32_t>(keepAliveChannels);
        audioInfo.rate = keepAliveRate;
        const spa_pod *params[1];
        params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audioInfo);

        const int res = pw_stream_connect(
            keepAliveStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

        pw_thread_loop_unlock(loop);

        if (res < 0) {
            pw_thread_loop_lock(loop);
            pw_stream_destroy(keepAliveStream);
            pw_thread_loop_unlock(loop);
            keepAliveStream = nullptr;
            emit q->engineError(QStringLiteral("Impossibile avviare lo stream di keepalive Bluetooth"));
        }
    }

    void linkKeepAliveSink(uint32_t generatorNodeId, uint32_t sinkNodeId)
    {
        const uint32_t linkId = q->linkNodes(generatorNodeId, sinkNodeId);
        if (linkId != 0)
            keepAliveLinkIds.insert(sinkNodeId, linkId);
        else
            emit q->engineError(QStringLiteral("Impossibile avviare il keepalive per il sink Bluetooth"));
    }

    // Chiamato (sul thread Qt, via invokeMethod queued) non appena
    // onKeepAliveStateChanged scopre il nodeId del generatore: collega tutti
    // i sink richiesti nel frattempo che non hanno ancora un link attivo.
    void flushPendingKeepAliveLinks()
    {
        uint32_t generatorId = 0;
        {
            QMutexLocker locker(&nodesMutex);
            generatorId = keepAliveGeneratorNodeId;
        }
        if (generatorId == 0)
            return;

        for (uint32_t sinkNodeId : std::as_const(keepAliveDesiredSinks)) {
            if (!keepAliveLinkIds.contains(sinkNodeId))
                linkKeepAliveSink(generatorId, sinkNodeId);
        }
    }

    static void onKeepAliveStateChanged(void *data, pw_stream_state /*old*/,
                                         pw_stream_state state, const char * /*error*/)
    {
        auto *impl = static_cast<Impl *>(data);
        if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
            return;

        bool justDiscovered = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            if (impl->keepAliveGeneratorNodeId == 0) {
                const uint32_t nodeId = pw_stream_get_node_id(impl->keepAliveStream);
                if (nodeId != SPA_ID_INVALID) {
                    impl->keepAliveGeneratorNodeId = nodeId;
                    justDiscovered = true;
                }
            }
        }

        if (justDiscovered) {
            QMetaObject::invokeMethod(
                impl->q, [impl]() { impl->flushPendingKeepAliveLinks(); }, Qt::QueuedConnection);
        }
    }

    // Genera silenzio continuo tranne un breve tono ogni
    // keepAlivePingPeriodSeconds: sufficiente a mantenere attiva la
    // connessione A2DP e a resettare l'eventuale timer di spegnimento per
    // inattività (che scatta rilevando silenzio digitale vero — inviare
    // silenzio reale non funzionerebbe). Frequenza/ampiezza/durata/
    // periodo sono regolabili a runtime (vedi PipeWireEngine::
    // setKeepAlivePing*) — un tono puro invece del precedente rumore
    // bianco per poter sperimentare una frequenza ultrasonica (inaudibile
    // all'orecchio umano, richiesto esplicitamente dall'utente) invece di
    // limitarsi ad abbassarne il volume.
    static void onKeepAliveProcess(void *data)
    {
        auto *impl = static_cast<Impl *>(data);

        pw_buffer *b = pw_stream_dequeue_buffer(impl->keepAliveStream);
        if (!b)
            return;

        spa_buffer *buf = b->buffer;
        auto *dst = static_cast<float *>(buf->datas[0].data);
        if (!dst) {
            pw_stream_queue_buffer(impl->keepAliveStream, b);
            return;
        }

        const auto channels = static_cast<uint32_t>(impl->keepAliveChannels);
        const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * channels;
        uint32_t maxFrames = buf->datas[0].maxsize / stride;
        if (b->requested && b->requested < maxFrames)
            maxFrames = static_cast<uint32_t>(b->requested);

        const double rate = impl->keepAliveRate;
        const double frequencyHz = impl->keepAlivePingFrequencyHz.load(std::memory_order_relaxed);
        const float amplitude = impl->keepAlivePingAmplitude.load(std::memory_order_relaxed);
        const double pingDurationSeconds = impl->keepAlivePingDurationSeconds.load(std::memory_order_relaxed);
        const double periodSeconds = impl->keepAlivePingPeriodSeconds.load(std::memory_order_relaxed);
        constexpr double kFadeSeconds = 0.010;

        const auto periodSamples = static_cast<uint64_t>(std::max(0.001, periodSeconds) * rate);
        const auto pingSamples = static_cast<uint64_t>(std::max(0.0, pingDurationSeconds) * rate);
        const auto fadeSamples = static_cast<uint64_t>(kFadeSeconds * rate);
        const double phaseIncrement = 2.0 * std::numbers::pi * frequencyHz / rate;

        for (uint32_t i = 0; i < maxFrames; ++i) {
            const uint64_t phase = periodSamples > 0 ? impl->keepAliveSampleClock % periodSamples : 0;
            if (phase < pingSamples) {
                float envelope = 1.0f;
                if (phase < fadeSamples)
                    envelope = static_cast<float>(phase) / static_cast<float>(fadeSamples);
                else if (phase >= pingSamples - fadeSamples)
                    envelope = static_cast<float>(pingSamples - phase) / static_cast<float>(fadeSamples);
                const float sample = amplitude * envelope * static_cast<float>(std::sin(impl->keepAlivePingPhase));
                for (uint32_t c = 0; c < channels; ++c)
                    dst[i * channels + c] = sample;
            } else {
                for (uint32_t c = 0; c < channels; ++c)
                    dst[i * channels + c] = 0.0f;
            }
            impl->keepAlivePingPhase += phaseIncrement;
            if (impl->keepAlivePingPhase > 2.0 * std::numbers::pi)
                impl->keepAlivePingPhase -= 2.0 * std::numbers::pi;
            ++impl->keepAliveSampleClock;
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
        buf->datas[0].chunk->size = maxFrames * stride;

        pw_stream_queue_buffer(impl->keepAliveStream, b);
    }

    static constexpr pw_stream_events keepAliveStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onKeepAliveStateChanged,
        .process = onKeepAliveProcess,
    };

    // Distrugge uno IdentifyStream una volta noto il suo nodeId (chiamato
    // solo dal thread Qt, via QTimer, dopo che la sequenza di bip è finita
    // di suonare).
    void destroyIdentifyStream(uint32_t nodeId)
    {
        IdentifyStream *toRemove = nullptr;
        const auto it = std::find_if(identifyStreams.begin(), identifyStreams.end(),
                                      [&](IdentifyStream *s) { return s->nodeId == nodeId; });
        if (it == identifyStreams.end())
            return;
        toRemove = *it;
        identifyStreams.erase(it);

        pw_thread_loop_lock(loop);
        pw_stream_destroy(toRemove->stream);
        pw_thread_loop_unlock(loop);
        delete toRemove;
    }

    static void onIdentifyStateChanged(void *data, pw_stream_state /*old*/,
                                        pw_stream_state state, const char * /*error*/)
    {
        auto *stream = static_cast<IdentifyStream *>(data);
        if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
            return;

        Impl *impl = stream->engineImpl;
        bool justLinked = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            if (stream->nodeId == 0) {
                const uint32_t nodeId = pw_stream_get_node_id(stream->stream);
                if (nodeId != SPA_ID_INVALID)
                    stream->nodeId = nodeId;
            }
            if (stream->nodeId != 0 && !stream->linked) {
                stream->linked = true;
                justLinked = true;
            }
        }

        if (!justLinked)
            return;

        PipeWireEngine *q = impl->q;
        const uint32_t nodeId = stream->nodeId;
        const uint32_t targetSinkId = stream->targetSinkId;
        QMetaObject::invokeMethod(
            q,
            [q, impl, nodeId, targetSinkId]() {
                q->linkNodes(nodeId, targetSinkId);
                // La sequenza dura poco più di mezzo secondo (vedi
                // onIdentifyProcess): con un margine, distrugge lo stream
                // una volta finita di suonare.
                QTimer::singleShot(700, q, [impl, nodeId]() { impl->destroyIdentifyStream(nodeId); });
            },
            Qt::QueuedConnection);
    }

    // Due bip chiaramente udibili (880Hz, 150ms, separati da 120ms di
    // silenzio) con una breve dissolvenza per evitare click, poi silenzio
    // fino a quando lo stream non viene distrutto dal timer.
    static void onIdentifyProcess(void *data)
    {
        auto *stream = static_cast<IdentifyStream *>(data);

        pw_buffer *b = pw_stream_dequeue_buffer(stream->stream);
        if (!b)
            return;

        spa_buffer *buf = b->buffer;
        auto *dst = static_cast<float *>(buf->datas[0].data);
        if (!dst) {
            pw_stream_queue_buffer(stream->stream, b);
            return;
        }

        const auto channels = static_cast<uint32_t>(stream->channels);
        const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * channels;
        uint32_t maxFrames = buf->datas[0].maxsize / stride;
        if (b->requested && b->requested < maxFrames)
            maxFrames = static_cast<uint32_t>(b->requested);

        constexpr double kBeepDurationSeconds = 0.15;
        constexpr double kGapSeconds = 0.12;
        constexpr float kBeepAmplitude = 0.35f;
        constexpr double kBeepFrequencyHz = 880.0;
        constexpr double kFadeSeconds = 0.01;
        const double beep2Start = kBeepDurationSeconds + kGapSeconds;
        const double beep2End = beep2Start + kBeepDurationSeconds;

        const double rate = stream->rate;

        for (uint32_t i = 0; i < maxFrames; ++i) {
            const double t = static_cast<double>(stream->sampleClock) / rate;
            float sample = 0.0f;
            double beepPos = -1.0; // posizione dentro il bip corrente, se ce n'è uno
            if (t < kBeepDurationSeconds)
                beepPos = t;
            else if (t >= beep2Start && t < beep2End)
                beepPos = t - beep2Start;

            if (beepPos >= 0.0) {
                float envelope = 1.0f;
                if (beepPos < kFadeSeconds)
                    envelope = static_cast<float>(beepPos / kFadeSeconds);
                else if (beepPos >= kBeepDurationSeconds - kFadeSeconds)
                    envelope = static_cast<float>((kBeepDurationSeconds - beepPos) / kFadeSeconds);
                sample = kBeepAmplitude * envelope
                    * static_cast<float>(std::sin(2.0 * std::numbers::pi * kBeepFrequencyHz * t));
            }
            for (uint32_t c = 0; c < channels; ++c)
                dst[i * channels + c] = sample;
            ++stream->sampleClock;
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
        buf->datas[0].chunk->size = maxFrames * stride;

        pw_stream_queue_buffer(stream->stream, b);
    }

    static constexpr pw_stream_events identifyStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onIdentifyStateChanged,
        .process = onIdentifyProcess,
    };

    // --- Callback pw_stream_events per la riproduzione file ---

    static void onFileStreamStateChanged(void *data, pw_stream_state /*old*/,
                                          pw_stream_state state, const char * /*error*/)
    {
        auto *fs = static_cast<FileStream *>(data);
        if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
            return;

        QMutexLocker locker(&fs->engineImpl->nodesMutex);
        if (fs->nodeId == 0) {
            const uint32_t nodeId = pw_stream_get_node_id(fs->stream);
            if (nodeId != SPA_ID_INVALID)
                fs->nodeId = nodeId;
        }
    }

    // Chiamata dal thread PipeWire ogni volta che lo stream ha bisogno di un
    // buffer riempito. Copia i campioni dal buffer già decodificato in
    // memoria (fs->samples, caricato una volta sola in createFileStream),
    // avanzando playPosition in avanti o all'indietro secondo
    // reverseRequested. Ad ogni "giro" (fine raggiunta nella direzione
    // corrente) si torna all'estremo opposto e si conta un loop: se
    // loopCountRequested è positivo e i loop svolti lo raggiungono, lo
    // stream smette di produrre audio (silenzio) e notifica una sola volta
    // il thread Qt via fileStreamFinished, così PatchManager può fermare la
    // cue come farebbe un utente col tasto Stop.
    static void onFileStreamProcess(void *data)
    {
        auto *fs = static_cast<FileStream *>(data);

        pw_buffer *b = pw_stream_dequeue_buffer(fs->stream);
        if (!b)
            return;

        spa_buffer *buf = b->buffer;
        auto *dst = static_cast<float *>(buf->datas[0].data);
        if (!dst) {
            pw_stream_queue_buffer(fs->stream, b);
            return;
        }

        const uint32_t stride = static_cast<uint32_t>(sizeof(float) * fs->channels);
        uint32_t maxFrames = buf->datas[0].maxsize / stride;
        if (b->requested && b->requested < maxFrames)
            maxFrames = static_cast<uint32_t>(b->requested);

        bool wrappedThisCallback = false;

        if (fs->totalFrames == 0) {
            // File vuoto o non ancora caricato: silenzio.
            std::fill_n(dst, static_cast<size_t>(maxFrames) * fs->channels, 0.0f);
        } else {
            const bool reverse = fs->reverseRequested.load(std::memory_order_relaxed);
            const int loopCount = fs->loopCountRequested.load(std::memory_order_relaxed);
            const float *src = fs->samples.constData();

            for (uint32_t i = 0; i < maxFrames; ++i) {
                if (fs->finished.load(std::memory_order_relaxed)) {
                    std::fill_n(dst + i * fs->channels, fs->channels, 0.0f);
                    continue;
                }

                std::copy_n(src + fs->playPosition * fs->channels, fs->channels, dst + i * fs->channels);

                bool wrapped = false;
                if (!reverse) {
                    ++fs->playPosition;
                    if (fs->playPosition >= fs->totalFrames) {
                        fs->playPosition = 0;
                        wrapped = true;
                    }
                } else {
                    if (fs->playPosition == 0) {
                        fs->playPosition = fs->totalFrames - 1;
                        wrapped = true;
                    } else {
                        --fs->playPosition;
                    }
                }

                if (wrapped) {
                    wrappedThisCallback = true;
                    ++fs->loopsPlayed;
                    if (loopCount >= 0 && fs->loopsPlayed >= loopCount)
                        fs->finished.store(true, std::memory_order_relaxed);
                }
            }
        }

        // Notifiche verso il thread Qt: sempre via invocazione in coda,
        // niente di bloccante nel percorso realtime. wrappedThisCallback
        // viene notificato ad ogni giro (anche l'ultimo, prima di finished)
        // così PatchManager può far ruotare l'output attivo; finished solo
        // una volta, quando il conteggio di loop è esaurito.
        if (wrappedThisCallback) {
            Impl *impl = fs->engineImpl;
            const uint32_t nodeId = fs->nodeId;
            QMetaObject::invokeMethod(impl->q, [impl, nodeId]() {
                emit impl->q->fileStreamLooped(nodeId);
            }, Qt::QueuedConnection);
        }

        if (fs->finished.load(std::memory_order_relaxed) && !fs->finishedNotified) {
            fs->finishedNotified = true;
            Impl *impl = fs->engineImpl;
            const uint32_t nodeId = fs->nodeId;
            QMetaObject::invokeMethod(impl->q, [impl, nodeId]() {
                emit impl->q->fileStreamFinished(nodeId);
            }, Qt::QueuedConnection);
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
        buf->datas[0].chunk->size = maxFrames * stride;

        pw_stream_queue_buffer(fs->stream, b);
    }

    static constexpr pw_stream_events fileStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onFileStreamStateChanged,
        .process = onFileStreamProcess,
    };

    // --- Callback statiche pw_registry_events ---

    static void onGlobal(void *data, uint32_t id, uint32_t /*permissions*/,
                          const char *type, uint32_t /*version*/,
                          const struct spa_dict *props)
    {
        auto *impl = static_cast<Impl *>(data);
        if (!props)
            return;

        if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
            onGlobalPort(impl, id, props);
            return;
        }

        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
            return;

        const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
        const AudioNode::Kind kind = kindFromMediaClass(mediaClass);
        if (kind == AudioNode::Kind::Unknown)
            return; // non un nodo audio sink/source: ignoralo (es. Video/Sink)

        AudioNode node;
        node.id = id;
        node.kind = kind;

        if (impl->ownedVirtualSinkIds.contains(id))
            node.kind = AudioNode::Kind::VirtualSink;

        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        node.name = name ? QString::fromUtf8(name) : QString();
        node.description = description ? QString::fromUtf8(description) : node.name;

        const char *deviceApi = spa_dict_lookup(props, "device.api");
        if (deviceApi && std::strcmp(deviceApi, "bluez5") == 0) {
            node.isBluetooth = true;
            const char *addr = spa_dict_lookup(props, "api.bluez5.address");
            if (addr)
                node.bluetoothMac = QString::fromUtf8(addr);
        }

        {
            QMutexLocker locker(&impl->nodesMutex);
            const auto it = std::find_if(impl->nodes.begin(), impl->nodes.end(),
                                          [&](const AudioNode &n) { return n.id == node.id; });
            if (it != impl->nodes.end())
                *it = node;
            else
                impl->nodes.append(node);
        }

        PipeWireEngine *q = impl->q;
        QMetaObject::invokeMethod(
            q, [q, node]() { emit q->nodeAdded(node); }, Qt::QueuedConnection);

        // Un sink Bluetooth arriva quasi sempre SENZA device.api/api.bluez5.*
        // in questo primo annuncio (vedi commento su SinkWatch): ci
        // agganciamo alle sue info per scoprirlo quando PipeWire lo
        // aggiunge, un istante dopo.
        if (kind == AudioNode::Kind::PhysicalSink && !impl->sinkWatches.contains(id)) {
            auto *watch = new SinkWatch();
            watch->owner = impl;
            watch->nodeId = id;
            watch->proxy = static_cast<pw_proxy *>(pw_registry_bind(
                impl->registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
            if (watch->proxy) {
                pw_node_add_listener(reinterpret_cast<pw_node *>(watch->proxy),
                                      &watch->listener, &sinkNodeEvents, watch);
                // Senza questa sottoscrizione esplicita, l'evento "param"
                // con il volume corrente non arriva mai — l'evento "info"
                // da solo segnala solo QUALI parametri esistono (change
                // mask), non i loro valori.
                uint32_t volumeParamId = SPA_PARAM_Props;
                pw_node_subscribe_params(reinterpret_cast<pw_node *>(watch->proxy),
                                          &volumeParamId, 1);
                impl->sinkWatches.insert(id, watch);
            } else {
                delete watch;
            }
        }
    }

    // Chiamata quando arrivano le info aggiornate di un nodo Sink a cui
    // siamo agganciati (vedi SinkWatch): se ora compare device.api=bluez5 e
    // prima non lo sapevamo, aggiorna il nodo memorizzato ed emette
    // nodeUpdated — PatchManager lo usa per agganciare il keepalive anche ai
    // sink Bluetooth scoperti/negoziati dopo il primo annuncio.
    static void onSinkNodeInfo(void *data, const struct pw_node_info *info)
    {
        auto *watch = static_cast<SinkWatch *>(data);
        Impl *impl = watch->owner;
        if (!info || !info->props)
            return;

        const char *deviceApi = spa_dict_lookup(info->props, "device.api");
        if (!deviceApi || std::strcmp(deviceApi, "bluez5") != 0)
            return;

        const char *addr = spa_dict_lookup(info->props, "api.bluez5.address");
        const QString mac = addr ? QString::fromUtf8(addr) : QString();

        AudioNode updated;
        bool changed = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            const auto it = std::find_if(impl->nodes.begin(), impl->nodes.end(),
                                          [&](const AudioNode &n) { return n.id == info->id; });
            if (it == impl->nodes.end())
                return;
            if (!it->isBluetooth) {
                it->isBluetooth = true;
                changed = true;
            }
            // bluetoothMac va aggiornato ogni volta che arriva un indirizzo
            // valido, non solo alla transizione isBluetooth false->true qui
            // sopra: device.api e api.bluez5.address possono arrivare in
            // eventi onSinkNodeInfo separati, quindi la primissima volta che
            // isBluetooth diventa true l'indirizzo può non essere ancora
            // presente — con il vecchio codice (mac catturato SOLO dentro
            // l'if sopra) restava per sempre vuoto, dato che quell'if non
            // scatta mai più per lo stesso nodo. Bug latente da prima di
            // questa sessione, mai emerso perché finora solo isBluetooth
            // veniva usato (keepalive) — la correlazione batteria
            // (PatchManager) è la prima funzionalità che dipende davvero da
            // bluetoothMac, e con un indirizzo vuoto non trova mai il
            // dispositivo BlueZ corrispondente.
            if (!mac.isEmpty() && it->bluetoothMac != mac) {
                it->bluetoothMac = mac;
                changed = true;
            }
            updated = *it;
        }

        if (changed) {
            PipeWireEngine *q = impl->q;
            QMetaObject::invokeMethod(
                q, [q, updated]() { emit q->nodeUpdated(updated); }, Qt::QueuedConnection);
        }
    }

    // Chiamata quando arriva il valore corrente (o un aggiornamento) dello
    // stato muto di un sink a cui siamo agganciati — solo dopo
    // pw_node_subscribe_params(SPA_PARAM_Props), vedi il punto di
    // registrazione del SinkWatch. A differenza di pw_node_info, questa
    // callback non porta l'id del nodo: viene da "data" (il SinkWatch
    // stesso, non il solo Impl condiviso — vedi il commento su SinkWatch).
    //
    // NOTA: leggeva in origine anche SPA_PROP_channelVolumes per un volume
    // impostabile dallo slider — rimosso su richiesta esplicita
    // dell'utente dopo aver verificato che il volume software del nodo
    // agisce solo DENTRO il range già limitato a monte dal mixer di
    // sistema (non controllabile da qui): un semplice muto/non muto è
    // l'unico controllo che l'app può offrire in modo affidabile.
    static void onSinkNodeParam(void *data, int /*seq*/, uint32_t id, uint32_t /*index*/,
                                 uint32_t /*next*/, const struct spa_pod *param)
    {
        if (id != SPA_PARAM_Props || !param)
            return;

        auto *watch = static_cast<SinkWatch *>(data);
        Impl *impl = watch->owner;

        bool haveMute = false;
        bool muted = false;

        const auto *obj = reinterpret_cast<const struct spa_pod_object *>(param);
        const struct spa_pod_prop *prop;
        SPA_POD_OBJECT_FOREACH(obj, prop) {
            if (prop->key == SPA_PROP_mute) {
                haveMute = spa_pod_get_bool(&prop->value, &muted) == 0;
            }
        }

        if (!haveMute)
            return; // nessun campo mute in questo aggiornamento

        AudioNode updated;
        bool changed = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            const auto it = std::find_if(impl->nodes.begin(), impl->nodes.end(),
                                          [&](const AudioNode &n) { return n.id == watch->nodeId; });
            if (it == impl->nodes.end())
                return;
            if (it->muted != muted) {
                it->muted = muted;
                changed = true;
            }
            updated = *it;
        }

        if (changed) {
            PipeWireEngine *q = impl->q;
            QMetaObject::invokeMethod(
                q, [q, updated]() { emit q->nodeUpdated(updated); }, Qt::QueuedConnection);
        }
    }

    static constexpr pw_node_events sinkNodeEvents = {
        .version = PW_VERSION_NODE_EVENTS,
        .info = onSinkNodeInfo,
        .param = onSinkNodeParam,
    };

    // Una porta PipeWire ha sempre PW_KEY_NODE_ID (a quale nodo appartiene)
    // e PW_KEY_PORT_DIRECTION ("in" o "out"). Le teniamo in una lista piatta
    // protetta dallo stesso mutex dei nodi, per semplicità.
    static void onGlobalPort(Impl *impl, uint32_t id, const struct spa_dict *props)
    {
        const char *nodeIdStr = spa_dict_lookup(props, PW_KEY_NODE_ID);
        const char *direction = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION);
        if (!nodeIdStr || !direction)
            return;

        TrackedPort port;
        port.id = id;
        port.nodeId = static_cast<uint32_t>(std::strtoul(nodeIdStr, nullptr, 10));
        port.isOutput = (std::strcmp(direction, "out") == 0);

        QMutexLocker locker(&impl->nodesMutex);
        const auto existing = std::count_if(impl->ports.begin(), impl->ports.end(),
                                             [&](const TrackedPort &p) {
                                                 return p.nodeId == port.nodeId && p.isOutput == port.isOutput;
                                             });
        port.channelIndex = static_cast<int>(existing);
        impl->ports.append(port);
    }

    static void onGlobalRemove(void *data, uint32_t id)
    {
        auto *impl = static_cast<Impl *>(data);

        const auto watchIt = impl->sinkWatches.find(id);
        if (watchIt != impl->sinkWatches.end()) {
            pw_proxy_destroy(watchIt.value()->proxy);
            delete watchIt.value();
            impl->sinkWatches.erase(watchIt);
        }

        bool nodeExisted = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            const auto nodeIt = std::find_if(impl->nodes.begin(), impl->nodes.end(),
                                              [&](const AudioNode &n) { return n.id == id; });
            if (nodeIt != impl->nodes.end()) {
                impl->nodes.erase(nodeIt);
                nodeExisted = true;
            }

            impl->ports.erase(
                std::remove_if(impl->ports.begin(), impl->ports.end(),
                                [&](const TrackedPort &p) { return p.id == id || p.nodeId == id; }),
                impl->ports.end());
        }

        if (!nodeExisted)
            return;

        PipeWireEngine *q = impl->q;
        QMetaObject::invokeMethod(
            q, [q, id]() { emit q->nodeRemoved(id); }, Qt::QueuedConnection);
    }

    static constexpr pw_registry_events registryEvents = {
        .version = PW_VERSION_REGISTRY_EVENTS,
        .global = onGlobal,
        .global_remove = onGlobalRemove,
    };

    // Ritorna le porte di un nodo per la direzione richiesta, ordinate per
    // channelIndex (così FL si abbina a FL, FR a FR, assumendo che l'ordine
    // di scoperta rispecchi l'ordine dei canali, vero nella grande
    // maggioranza dei casi con PipeWire/WirePlumber).
    QVector<TrackedPort> portsForNode(uint32_t nodeId, bool wantOutput) const
    {
        QVector<TrackedPort> result;
        for (const TrackedPort &p : ports) {
            if (p.nodeId == nodeId && p.isOutput == wantOutput)
                result.append(p);
        }
        std::sort(result.begin(), result.end(),
                  [](const TrackedPort &a, const TrackedPort &b) { return a.channelIndex < b.channelIndex; });
        return result;
    }
};

PipeWireEngine::PipeWireEngine(QObject *parent)
    : AudioEngine(parent)
    , d(std::make_unique<Impl>())
{
    d->q = this;
    pw_init(nullptr, nullptr);
}

PipeWireEngine::~PipeWireEngine()
{
    stop();
    pw_deinit();
}

bool PipeWireEngine::start()
{
    d->loop = pw_thread_loop_new("bluecue-pw-loop", nullptr);
    if (!d->loop) {
        emit engineError(QStringLiteral("Impossibile creare pw_thread_loop"));
        return false;
    }

    pw_thread_loop_lock(d->loop);

    d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
    if (!d->context) {
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile creare pw_context"));
        return false;
    }

    d->core = pw_context_connect(d->context, nullptr, 0);
    if (!d->core) {
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile connettersi al demone PipeWire"));
        return false;
    }

    d->registry = pw_core_get_registry(d->core, PW_VERSION_REGISTRY, 0);
    if (!d->registry) {
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile ottenere il registry PipeWire"));
        return false;
    }

    pw_registry_add_listener(d->registry, &d->registryListener,
                              &Impl::registryEvents, d.get());

    pw_thread_loop_unlock(d->loop);

    if (pw_thread_loop_start(d->loop) != 0) {
        emit engineError(QStringLiteral("Impossibile avviare il thread PipeWire"));
        return false;
    }

    return true;
}

void PipeWireEngine::stop()
{
    if (d->loop) {
        pw_thread_loop_lock(d->loop);

        for (auto &link : d->activeLinks) {
            for (pw_proxy *proxy : link.proxies)
                pw_proxy_destroy(proxy);
        }
        d->activeLinks.clear();

        for (Impl::FileStream *fs : d->fileStreams)
            pw_stream_destroy(fs->stream);

        for (Impl::IdentifyStream *is : d->identifyStreams)
            pw_stream_destroy(is->stream);

        if (d->keepAliveStream)
            pw_stream_destroy(d->keepAliveStream);

        for (Impl::SinkWatch *watch : std::as_const(d->sinkWatches))
            pw_proxy_destroy(watch->proxy);

        if (d->registry) {
            spa_hook_remove(&d->registryListener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy *>(d->registry));
            d->registry = nullptr;
        }
        pw_thread_loop_unlock(d->loop);

        pw_thread_loop_stop(d->loop);
    }

    qDeleteAll(d->fileStreams);
    d->fileStreams.clear();

    qDeleteAll(d->identifyStreams);
    d->identifyStreams.clear();

    qDeleteAll(d->sinkWatches);
    d->sinkWatches.clear();

    d->keepAliveStream = nullptr;
    d->keepAliveGeneratorNodeId = 0;
    d->keepAliveDesiredSinks.clear();
    d->keepAliveLinkIds.clear();

    if (d->core) {
        pw_core_disconnect(d->core);
        d->core = nullptr;
    }
    if (d->context) {
        pw_context_destroy(d->context);
        d->context = nullptr;
    }
    if (d->loop) {
        pw_thread_loop_destroy(d->loop);
        d->loop = nullptr;
    }
}

QVector<AudioNode> PipeWireEngine::nodes() const
{
    QMutexLocker locker(&d->nodesMutex);
    return d->nodes;
}

void PipeWireEngine::createVirtualSink(const QString &name, const QString &description)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return;
    }

    pw_thread_loop_lock(d->loop);

    const QByteArray nameUtf8 = name.toUtf8();
    const QByteArray descUtf8 = description.toUtf8();

    const QByteArray args = QByteArray(
        "{ node.description = \"") + descUtf8 + QByteArray("\" "
        "capture.props = { node.name = \"") + nameUtf8 + QByteArray("\" "
        "media.class = Audio/Sink } "
        "playback.props = { node.name = \"") + nameUtf8 + QByteArray("-in\" } }");

    pw_impl_module *module = pw_context_load_module(
        d->context, "libpipewire-module-loopback", args.constData(), nullptr);

    pw_thread_loop_unlock(d->loop);

    if (!module) {
        emit engineError(QStringLiteral("Impossibile creare il sink virtuale '%1'").arg(name));
        return;
    }

    // Il nodo risultante arriva tramite la normale callback onGlobal() del
    // registry; il suo id numerico si scopre solo li'. ownedVirtualSinkIds
    // andra' popolato intercettando nodeAdded() per nome, da rifinire
    // quando servira' distinguere con certezza sink virtuali da fisici.
}

void PipeWireEngine::removeVirtualSink(uint32_t nodeId)
{
    // TODO: serve tracciare il pw_impl_module* restituito da
    // createVirtualSink() insieme al relativo nodeId per poterlo scaricare
    // con pw_impl_module_destroy() qui. Per ora no-op.
    Q_UNUSED(nodeId);
}

QString PipeWireEngine::createFileStream(const QString &filePath, const QString &description,
                                          int loopCount, bool reverse)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return QString();
    }

    SF_INFO sfinfo{};
    SNDFILE *sndfile = sf_open(filePath.toUtf8().constData(), SFM_READ, &sfinfo);
    if (!sndfile) {
        emit engineError(QStringLiteral("Impossibile aprire il file audio: %1").arg(QString::fromUtf8(sf_strerror(nullptr))));
        return QString();
    }

    auto *fs = new Impl::FileStream();
    fs->engineImpl = d.get();
    fs->channels = sfinfo.channels;
    fs->reverseRequested.store(reverse, std::memory_order_relaxed);
    fs->loopCountRequested.store(loopCount, std::memory_order_relaxed);

    // Caricamento in memoria una tantum: sf_readf_float può restituire meno
    // frame di sfinfo.frames per file troncati/corrotti, quindi il conteggio
    // reale (totalFrames) è quello effettivamente letto, non quello dichiarato.
    if (sfinfo.frames > 0) {
        fs->samples.resize(static_cast<qsizetype>(sfinfo.frames) * fs->channels);
        const sf_count_t framesRead = sf_readf_float(sndfile, fs->samples.data(), sfinfo.frames);
        fs->totalFrames = static_cast<uint64_t>(std::max<sf_count_t>(framesRead, 0));
        fs->samples.resize(static_cast<qsizetype>(fs->totalFrames) * fs->channels);
    }
    sf_close(sndfile);

    if (fs->totalFrames == 0) {
        delete fs;
        emit engineError(QStringLiteral("File audio vuoto o illeggibile: %1").arg(filePath));
        return QString();
    }
    // In riproduzione all'indietro si parte dall'ultimo frame, non dal primo.
    fs->playPosition = reverse ? fs->totalFrames - 1 : 0;

    pw_thread_loop_lock(d->loop);

    const QByteArray descUtf8 = description.toUtf8();
    const QString streamName = QStringLiteral("bluecue.file.") + QString::number(d->nextFileStreamId++);
    const QByteArray nameUtf8 = streamName.toUtf8();

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_MEDIA_CLASS, "Audio/Source",
        PW_KEY_NODE_NAME, nameUtf8.constData(),
        PW_KEY_NODE_DESCRIPTION, descUtf8.constData(),
        nullptr);

    fs->stream = pw_stream_new(d->core, nameUtf8.constData(), props);
    if (!fs->stream) {
        pw_thread_loop_unlock(d->loop);
        delete fs;
        emit engineError(QStringLiteral("Impossibile creare lo stream di playback"));
        return QString();
    }

    pw_stream_add_listener(fs->stream, &fs->streamListener, &Impl::fileStreamEvents, fs);

    uint8_t podBuffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_audio_info_raw audioInfo{};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    audioInfo.channels = static_cast<uint32_t>(sfinfo.channels);
    audioInfo.rate = static_cast<uint32_t>(sfinfo.samplerate);
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audioInfo);

    // Niente PW_STREAM_FLAG_AUTOCONNECT: il collegamento agli output è
    // deciso esplicitamente dall'utente tramite PatchManager::toggleConnection,
    // non automaticamente al sink di default.
    const int res = pw_stream_connect(
        fs->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

    pw_thread_loop_unlock(d->loop);

    if (res < 0) {
        pw_thread_loop_lock(d->loop);
        pw_stream_destroy(fs->stream);
        pw_thread_loop_unlock(d->loop);
        delete fs;
        emit engineError(QStringLiteral("Impossibile avviare lo stream di playback"));
        return QString();
    }

    QMutexLocker locker(&d->nodesMutex);
    d->fileStreams.append(fs);
    return streamName;
}

void PipeWireEngine::removeFileStream(uint32_t nodeId)
{
    if (!d->loop)
        return;

    Impl::FileStream *toRemove = nullptr;
    {
        QMutexLocker locker(&d->nodesMutex);
        const auto it = std::find_if(d->fileStreams.begin(), d->fileStreams.end(),
                                      [&](Impl::FileStream *fs) { return fs->nodeId == nodeId; });
        if (it == d->fileStreams.end())
            return; // non è uno stream file: no-op (es. sink/hardware)
        toRemove = *it;
        d->fileStreams.erase(it);
    }

    pw_thread_loop_lock(d->loop);
    pw_stream_destroy(toRemove->stream);
    pw_thread_loop_unlock(d->loop);

    delete toRemove;
}

void PipeWireEngine::setFileStreamLoopCount(uint32_t nodeId, int loopCount)
{
    QMutexLocker locker(&d->nodesMutex);
    const auto it = std::find_if(d->fileStreams.begin(), d->fileStreams.end(),
                                  [&](Impl::FileStream *fs) { return fs->nodeId == nodeId; });
    if (it == d->fileStreams.end())
        return; // non è uno stream file: no-op
    (*it)->loopCountRequested.store(loopCount, std::memory_order_relaxed);
}

void PipeWireEngine::setFileStreamReverse(uint32_t nodeId, bool reverse)
{
    QMutexLocker locker(&d->nodesMutex);
    const auto it = std::find_if(d->fileStreams.begin(), d->fileStreams.end(),
                                  [&](Impl::FileStream *fs) { return fs->nodeId == nodeId; });
    if (it == d->fileStreams.end())
        return; // non è uno stream file: no-op
    (*it)->reverseRequested.store(reverse, std::memory_order_relaxed);
}

void PipeWireEngine::setFileStreamActive(uint32_t nodeId, bool active)
{
    if (!d->loop)
        return;

    pw_stream *stream = nullptr;
    {
        QMutexLocker locker(&d->nodesMutex);
        const auto it = std::find_if(d->fileStreams.begin(), d->fileStreams.end(),
                                      [&](Impl::FileStream *fs) { return fs->nodeId == nodeId; });
        if (it == d->fileStreams.end())
            return; // non è uno stream file: no-op
        stream = (*it)->stream;
    }

    pw_thread_loop_lock(d->loop);
    pw_stream_set_active(stream, active);
    pw_thread_loop_unlock(d->loop);
}

uint32_t PipeWireEngine::linkNodes(uint32_t outputNodeId, uint32_t inputNodeId)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return 0;
    }

    // Le porte di un nodo arrivano dal pw_registry come eventi separati e
    // asincroni rispetto al nodo stesso: se questo metodo viene chiamato a
    // ridosso della comparsa di un nodo nuovo, le sue porte potrebbero non
    // essere ancora note. Ritentiamo per una finestra breve, rilasciando il
    // lock tra un tentativo e l'altro (altrimenti il thread PipeWire non può
    // consegnare gli eventi in sospeso), prima di arrenderci.
    constexpr int kMaxPortLookupAttempts = 5;
    constexpr unsigned long kPortLookupRetryDelayMs = 40;

    QVector<TrackedPort> outPorts;
    QVector<TrackedPort> inPorts;
    for (int attempt = 0; attempt < kMaxPortLookupAttempts; ++attempt) {
        pw_thread_loop_lock(d->loop);
        {
            QMutexLocker locker(&d->nodesMutex);
            outPorts = d->portsForNode(outputNodeId, /*wantOutput=*/true);
            inPorts = d->portsForNode(inputNodeId, /*wantOutput=*/false);
        }
        pw_thread_loop_unlock(d->loop);

        if (!outPorts.isEmpty() && !inPorts.isEmpty())
            break;
        if (attempt + 1 < kMaxPortLookupAttempts)
            QThread::msleep(kPortLookupRetryDelayMs);
    }

    if (outPorts.isEmpty() || inPorts.isEmpty()) {
        emit engineError(QStringLiteral("Porte non trovate per uno dei due nodi (discovery incompleto?)"));
        return 0;
    }

    pw_thread_loop_lock(d->loop);

    // Abbiniamo le porte per indice di canale: se un lato ha meno canali
    // dell'altro (es. sorgente mono verso sink stereo), duplichiamo l'unica
    // porta di output su tutte le porte di input rimanenti, cosi' il mono
    // viene inviato a entrambi i canali invece di lasciarne uno muto.
    const int pairCount = static_cast<int>(std::max(outPorts.size(), inPorts.size()));
    QVector<pw_proxy *> createdProxies;

    for (int i = 0; i < pairCount; ++i) {
        const int outIdx = std::min(i, static_cast<int>(outPorts.size()) - 1);
        const int inIdx = std::min(i, static_cast<int>(inPorts.size()) - 1);
        const TrackedPort &outPort = outPorts[outIdx];
        const TrackedPort &inPort = inPorts[inIdx];

        pw_properties *linkProps = pw_properties_new(
            PW_KEY_LINK_OUTPUT_PORT, std::to_string(outPort.id).c_str(),
            PW_KEY_LINK_INPUT_PORT, std::to_string(inPort.id).c_str(),
            PW_KEY_OBJECT_LINGER, "false",
            nullptr);

        pw_proxy *proxy = static_cast<pw_proxy *>(pw_core_create_object(
            d->core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
            &linkProps->dict, 0));

        pw_properties_free(linkProps);

        if (!proxy) {
            // Rollback dei link gia' creati in questa chiamata, per non
            // lasciare un routing parziale (es. solo il canale sinistro).
            for (pw_proxy *created : createdProxies)
                pw_proxy_destroy(created);
            pw_thread_loop_unlock(d->loop);
            emit engineError(QStringLiteral("Creazione del link PipeWire fallita"));
            return 0;
        }

        createdProxies.append(proxy);
    }

    pw_thread_loop_unlock(d->loop);

    const uint32_t localId = d->nextLocalLinkId++;
    d->activeLinks.append(Impl::ActiveLink{ localId, createdProxies });

    QMetaObject::invokeMethod(
        this, [this, localId]() { emit linkStateChanged(localId, true); }, Qt::QueuedConnection);

    return localId;
}

void PipeWireEngine::unlinkNodes(uint32_t linkId)
{
    if (!d->loop)
        return;

    const auto it = std::find_if(d->activeLinks.begin(), d->activeLinks.end(),
                                  [&](const Impl::ActiveLink &l) { return l.localId == linkId; });
    if (it == d->activeLinks.end())
        return;

    pw_thread_loop_lock(d->loop);
    for (pw_proxy *proxy : it->proxies)
        pw_proxy_destroy(proxy);
    pw_thread_loop_unlock(d->loop);

    d->activeLinks.erase(it);

    QMetaObject::invokeMethod(
        this, [this, linkId]() { emit linkStateChanged(linkId, false); }, Qt::QueuedConnection);
}

void PipeWireEngine::setKeepAliveEnabled(uint32_t sinkNodeId, bool enabled)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return;
    }

    if (enabled) {
        if (d->keepAliveDesiredSinks.contains(sinkNodeId))
            return;
        d->keepAliveDesiredSinks.insert(sinkNodeId);
        d->ensureKeepAliveStreamStarted();

        uint32_t generatorId = 0;
        {
            QMutexLocker locker(&d->nodesMutex);
            generatorId = d->keepAliveGeneratorNodeId;
        }
        if (generatorId != 0)
            d->linkKeepAliveSink(generatorId, sinkNodeId);
        // Altrimenti il link verrà creato da flushPendingKeepAliveLinks()
        // non appena il generatore scopre il proprio nodeId (asincrono).
    } else {
        d->keepAliveDesiredSinks.remove(sinkNodeId);
        const auto it = d->keepAliveLinkIds.find(sinkNodeId);
        if (it != d->keepAliveLinkIds.end()) {
            unlinkNodes(it.value());
            d->keepAliveLinkIds.erase(it);
        }
    }
}

void PipeWireEngine::identifySink(uint32_t sinkNodeId)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return;
    }

    auto *stream = new Impl::IdentifyStream();
    stream->engineImpl = d.get();
    stream->targetSinkId = sinkNodeId;

    pw_thread_loop_lock(d->loop);

    const QByteArray nameUtf8 = QByteArrayLiteral("bluecue.identify");
    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        // "Music" e non "Notification": quest'ultimo fa scattare le
        // politiche di auto-routing/ducking di WirePlumber (i suoni di
        // notifica vengono instradati per default all'output attivo del
        // sistema, non a quello scelto esplicitamente) — lo stesso ruolo
        // usato con successo da keepalive/file streams, che infatti si
        // collegano correttamente col linkNodes() manuale sotto.
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, nameUtf8.constData(),
        PW_KEY_NODE_DESCRIPTION, "BT Multizone identify",
        nullptr);

    stream->stream = pw_stream_new(d->core, nameUtf8.constData(), props);
    if (!stream->stream) {
        pw_thread_loop_unlock(d->loop);
        delete stream;
        emit engineError(QStringLiteral("Impossibile creare lo stream di identificazione"));
        return;
    }

    pw_stream_add_listener(stream->stream, &stream->listener, &Impl::identifyStreamEvents, stream);

    uint8_t podBuffer[1024];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    spa_audio_info_raw audioInfo{};
    audioInfo.format = SPA_AUDIO_FORMAT_F32;
    audioInfo.channels = static_cast<uint32_t>(stream->channels);
    audioInfo.rate = stream->rate;
    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&podBuilder, SPA_PARAM_EnumFormat, &audioInfo);

    const int res = pw_stream_connect(
        stream->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), params, 1);

    pw_thread_loop_unlock(d->loop);

    if (res < 0) {
        pw_thread_loop_lock(d->loop);
        pw_stream_destroy(stream->stream);
        pw_thread_loop_unlock(d->loop);
        delete stream;
        emit engineError(QStringLiteral("Impossibile avviare lo stream di identificazione"));
        return;
    }

    d->identifyStreams.append(stream);
}

void PipeWireEngine::setSinkMuted(uint32_t sinkNodeId, bool muted)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return;
    }

    pw_thread_loop_lock(d->loop);

    const auto it = d->sinkWatches.find(sinkNodeId);
    if (it == d->sinkWatches.end() || !it.value()->proxy) {
        pw_thread_loop_unlock(d->loop);
        return; // nodo non (più) tracciato: no-op silenzioso
    }
    Impl::SinkWatch *watch = it.value();

    uint8_t podBuffer[128];
    spa_pod_builder podBuilder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));
    const auto *param = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(
        &podBuilder,
        SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
        SPA_PROP_mute, SPA_POD_Bool(muted)));

    const int result = pw_node_set_param(reinterpret_cast<pw_node *>(watch->proxy),
                                          SPA_PARAM_Props, 0, param);

    pw_thread_loop_unlock(d->loop);

    if (result < 0) {
        QMetaObject::invokeMethod(this, [this, result]() {
            emit engineError(QStringLiteral("Impostazione muto fallita (errore %1)").arg(result));
        }, Qt::QueuedConnection);
    }
}

void PipeWireEngine::setKeepAlivePingFrequency(double hz)
{
    d->keepAlivePingFrequencyHz.store(std::clamp(hz, 1.0, 24000.0), std::memory_order_relaxed);
}

void PipeWireEngine::setKeepAlivePingAmplitude(float amplitude)
{
    d->keepAlivePingAmplitude.store(std::clamp(amplitude, 0.0f, 1.0f), std::memory_order_relaxed);
}

void PipeWireEngine::setKeepAlivePingDuration(double seconds)
{
    d->keepAlivePingDurationSeconds.store(std::max(0.0, seconds), std::memory_order_relaxed);
}

void PipeWireEngine::setKeepAlivePingPeriod(double seconds)
{
    d->keepAlivePingPeriodSeconds.store(std::max(0.5, seconds), std::memory_order_relaxed);
}
