#include "PipeWireEngine.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl.h>
#include <pipewire/filter.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/hook.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/dsp-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <sndfile.h>

#include <QDebug>
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
    if (std::strcmp(mediaClass, "Stream/Output/Audio") == 0)
        return AudioNode::Kind::AppStream;
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

    // TUTTI i link nel grafo, non solo quelli creati da noi (a differenza
    // di activeLinks) — popolato dal discovery del registry (onGlobal,
    // type Link, vedi sotto). Serve a setStreamTarget/forceRelinkNode per
    // "rubare" uno stream già collegato altrove (es. al sink di sistema di
    // default) quando lo si vuole reindirizzare verso un sink nostro: la
    // sola metadata "target.object" non è sufficiente in ogni condizione
    // (vedi il commento su PipeWireEngine::setStreamTarget per il perché),
    // quindi prendiamo il controllo diretto del link esattamente come
    // facciamo per tutto il resto del routing di quest'app.
    struct ForeignLink
    {
        uint32_t linkId = 0;
        uint32_t outputPortId = 0;
        uint32_t inputPortId = 0;
    };
    QVector<ForeignLink> allLinks;

    QVector<uint32_t> ownedVirtualSinkIds;

    // Sink virtuali creati da noi (createVirtualSink), tracciati per NOME
    // (l'unico identificatore noto subito, al momento della creazione — il
    // nodeId arriva solo dopo, in modo asincrono, tramite il normale
    // discovery del registry). Protetto da nodesMutex: onGlobal() lo legge/
    // aggiorna dal thread PipeWire per correlare il nodo appena scoperto,
    // create/removeVirtualSink lo scrivono dal thread Qt.
    struct VirtualSinkInfo
    {
        pw_impl_module *module = nullptr;
        uint32_t nodeId = 0; // 0 finché onGlobal() non lo correla per nome
    };
    QMap<QString, VirtualSinkInfo> virtualSinks;

    // Metadata "default" di PipeWire (quella con cui pavucontrol/wpctl
    // spostano lo stream di un'app verso un altro sink) — bind pigro al
    // primo avvistamento nel registry (onGlobal, type Metadata). Usata da
    // setStreamTarget/clearStreamTarget per "spostare" lo stream di
    // un'applicazione (Kind::AppStream) verso un sink virtuale nostro
    // invece che lasciarlo sull'uscita di sistema di default — vedi
    // PatchManager::addAppStreamCue. Solo thread PipeWire tra bind e
    // distruzione (stop()); le chiamate da thread Qt in
    // setStreamTarget/clearStreamTarget avvengono dentro
    // pw_thread_loop_lock, stesso principio del resto della classe.
    pw_proxy *defaultMetadataProxy = nullptr;
    pw_metadata *defaultMetadata = nullptr;
    spa_hook defaultMetadataListener{};

    // Nome stabile del sink audio corrente di default (chiave
    // "default.audio.sink" sulla metadata "default", con subject 0 —
    // impostata/aggiornata dal sistema, es. quando l'utente cambia
    // dispositivo di default dalle impostazioni), tenuto sincronizzato da
    // onDefaultMetadataProperty. Usato da clearStreamTarget per "restituire"
    // uno stream applicativo, che stavamo catturando, al sink giusto invece
    // di limitarsi a scollegarlo e sperare che il session manager lo
    // ricolleghi da solo — verificato empiricamente NON essere affidabile
    // per uno stream che aveva un target esplicito appena rimosso (a
    // differenza di un semplice scollegamento "grezzo" via pw-link -d, che
    // invece viene ripreso in carico dal session manager). Protetto da
    // nodesMutex: scritto dal thread PipeWire (callback metadata), letto
    // dal thread Qt in clearStreamTarget.
    QString defaultAudioSinkName;

    // Estrae il valore della chiave "name" da un JSON minimale del tipo
    // {"name":"alsa_output.pci-..."} — il formato con cui PipeWire scrive
    // sempre default.audio.sink/source, non serve un parser JSON completo.
    static QString extractJsonNameField(const char *jsonValue)
    {
        if (!jsonValue)
            return {};
        const QString v = QString::fromUtf8(jsonValue);
        const int keyIdx = v.indexOf(QStringLiteral("\"name\""));
        if (keyIdx < 0)
            return {};
        const int colonIdx = v.indexOf(QLatin1Char(':'), keyIdx);
        if (colonIdx < 0)
            return {};
        const int firstQuote = v.indexOf(QLatin1Char('"'), colonIdx + 1);
        if (firstQuote < 0)
            return {};
        const int secondQuote = v.indexOf(QLatin1Char('"'), firstQuote + 1);
        if (secondQuote < 0)
            return {};
        return v.mid(firstQuote + 1, secondQuote - firstQuote - 1);
    }

    static int onDefaultMetadataProperty(void *data, uint32_t subject, const char *key,
                                          const char * /*type*/, const char *value)
    {
        auto *impl = static_cast<Impl *>(data);
        if (subject != 0 || !key || std::strcmp(key, "default.audio.sink") != 0)
            return 0;

        QMutexLocker locker(&impl->nodesMutex);
        impl->defaultAudioSinkName = extractJsonNameField(value);
        return 0;
    }

    static constexpr pw_metadata_events defaultMetadataEvents = {
        .version = PW_VERSION_METADATA_EVENTS,
        .property = onDefaultMetadataProperty,
    };

    // Cerca un nodo già scoperto per nome stabile (AudioNode::name) — usato
    // da clearStreamTarget per risolvere defaultAudioSinkName nel nodeId
    // live corrispondente. 0 se non (ancora) scoperto.
    uint32_t resolveNodeIdByName(const QString &name) const
    {
        if (name.isEmpty())
            return 0;
        QMutexLocker locker(&nodesMutex);
        for (const AudioNode &n : nodes) {
            if (n.name == name)
                return n.id;
        }
        return 0;
    }

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

    // --- Calibrazione automatica del ritardo di output (richiesta
    // esplicitamente dall'utente: non riusciva a trovare a orecchio il
    // valore giusto dalla UI manuale) ---
    //
    // Nessuna proprietà PipeWire espone in modo affidabile la latenza
    // reale di un sink Bluetooth (dipende da codec/dispositivo/condizioni
    // radio, non standardizzata da A2DP) — l'unico modo per avere un
    // numero vero è misurarlo acusticamente: un breve click riprodotto su
    // ciascun sink IN SEQUENZA (non simultaneamente — evita ogni bisogno
    // di sincronizzare due stream diversi, molto più semplice e robusto:
    // il microfono registra in continuo, la differenza si ricava dagli
    // istanti di arrivo misurati nella stessa registrazione, non da un
    // trigger condiviso), catturato da un microfono e localizzato per
    // energia (un impulso di rumore bianco ha un fronte molto più netto
    // di un tono puro, quindi più facile da individuare con una semplice
    // soglia sull'inviluppo RMS).
    struct CalibrationSession
    {
        Impl *engineImpl = nullptr;

        // Stream generatore del click, riusato in sequenza per il sink A
        // poi per il sink B — un solo stream, ricollegato con
        // unlinkNodes/createPortLink diretto (MAI il linkNodes pubblico:
        // andrebbe a reindirizzare attraverso un eventuale filtro di
        // ritardo già presente sul sink, contaminando la misura con un
        // ritardo già applicato in precedenza).
        pw_stream *clickStream = nullptr;
        spa_hook clickStreamListener{};
        uint32_t clickStreamNodeId = 0; // protetto da nodesMutex, 0 finché non noto
        std::atomic<int> pendingClicks{0};  // richieste in coda, thread Qt -> thread audio
        bool emittingClick = false;         // SOLO thread audio
        uint32_t clickSamplesRemaining = 0; // SOLO thread audio
        uint32_t clickRngState = 0x1234abcdu; // SOLO thread audio (xorshift32)

        // Stream di cattura dal microfono, mono, accumula in
        // capturedSamples per tutta la durata della sessione.
        pw_stream *captureStream = nullptr;
        spa_hook captureStreamListener{};
        uint32_t captureStreamNodeId = 0; // protetto da nodesMutex

        // Unico punto in cui questa sessione prende un lock nel percorso
        // realtime (onCalibrationCaptureProcess) — accettabile: sessione
        // rara (un'azione esplicita dell'utente), breve (~2.3s), non
        // critica (un'increspatura nella registrazione non cambia
        // l'esito, serve solo a individuare due transienti netti).
        QVector<float> capturedSamples; // protetto da nodesMutex

        uint32_t sinkA = 0, sinkB = 0, micNodeId = 0;
        uint32_t rate = 48000;

        // Posizione (in campioni dentro capturedSamples) nel momento in
        // cui è stato richiesto ciascun click — non un timestamp di orologio:
        // essendo nello stesso array continuo del microfono, il confronto
        // resta valido indipendentemente da eventuali jitter del thread Qt.
        qint64 fireOffsetA = -1;
        qint64 fireOffsetB = -1;
    };
    std::unique_ptr<CalibrationSession> calibration;

    static void onCalibrationClickProcess(void *data)
    {
        auto *session = static_cast<CalibrationSession *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(session->clickStream);
        if (!b)
            return;
        spa_buffer *buf = b->buffer;
        auto *dst = static_cast<float *>(buf->datas[0].data);
        if (!dst) {
            pw_stream_queue_buffer(session->clickStream, b);
            return;
        }

        constexpr uint32_t kChannels = 2;
        const uint32_t stride = static_cast<uint32_t>(sizeof(float)) * kChannels;
        uint32_t maxFrames = buf->datas[0].maxsize / stride;
        if (b->requested && b->requested < maxFrames)
            maxFrames = static_cast<uint32_t>(b->requested);

        constexpr double kClickSeconds = 0.015;   // 15ms: transiente netto, facile da localizzare
        constexpr double kFadeInSeconds = 0.002;  // solo in apertura, evita un fronte perfettamente istantaneo
        constexpr float kAmplitude = 0.6f;         // volutamente ben udibile: test esplicito e breve, non un ping in sottofondo
        const uint32_t totalClickSamples = static_cast<uint32_t>(kClickSeconds * session->rate);
        const uint32_t fadeSamples = static_cast<uint32_t>(kFadeInSeconds * session->rate);

        for (uint32_t i = 0; i < maxFrames; ++i) {
            if (!session->emittingClick && session->pendingClicks.load(std::memory_order_relaxed) > 0) {
                session->pendingClicks.fetch_sub(1, std::memory_order_relaxed);
                session->emittingClick = true;
                session->clickSamplesRemaining = totalClickSamples;
            }

            float sample = 0.0f;
            if (session->emittingClick) {
                // xorshift32: rumore bianco, nessuna dipendenza da
                // rand()/<random> nel percorso realtime.
                uint32_t x = session->clickRngState;
                x ^= x << 13; x ^= x >> 17; x ^= x << 5;
                session->clickRngState = x;
                const float noise = (static_cast<float>(x) / 4294967295.0f) * 2.0f - 1.0f;

                const uint32_t elapsed = totalClickSamples - session->clickSamplesRemaining;
                float envelope = 1.0f;
                if (elapsed < fadeSamples)
                    envelope = static_cast<float>(elapsed) / static_cast<float>(fadeSamples);

                sample = kAmplitude * envelope * noise;

                if (--session->clickSamplesRemaining == 0)
                    session->emittingClick = false;
            }

            for (uint32_t c = 0; c < kChannels; ++c)
                dst[i * kChannels + c] = sample;
        }

        buf->datas[0].chunk->offset = 0;
        buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
        buf->datas[0].chunk->size = maxFrames * stride;

        pw_stream_queue_buffer(session->clickStream, b);
    }

    static void onCalibrationClickStateChanged(void *data, pw_stream_state /*old*/,
                                                pw_stream_state state, const char * /*error*/)
    {
        auto *session = static_cast<CalibrationSession *>(data);
        if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
            return;
        QMutexLocker locker(&session->engineImpl->nodesMutex);
        if (session->clickStreamNodeId == 0) {
            const uint32_t nodeId = pw_stream_get_node_id(session->clickStream);
            if (nodeId != SPA_ID_INVALID)
                session->clickStreamNodeId = nodeId;
        }
    }

    static void onCalibrationCaptureProcess(void *data)
    {
        auto *session = static_cast<CalibrationSession *>(data);
        pw_buffer *b = pw_stream_dequeue_buffer(session->captureStream);
        if (!b)
            return;
        spa_buffer *buf = b->buffer;
        auto *src = static_cast<float *>(buf->datas[0].data);
        if (src && buf->datas[0].chunk && buf->datas[0].chunk->stride > 0) {
            const uint32_t frames = buf->datas[0].chunk->size / static_cast<uint32_t>(buf->datas[0].chunk->stride);
            QMutexLocker locker(&session->engineImpl->nodesMutex);
            session->capturedSamples.reserve(session->capturedSamples.size() + static_cast<int>(frames));
            for (uint32_t i = 0; i < frames; ++i)
                session->capturedSamples.append(src[i]); // mono: un campione per frame
        }
        pw_stream_queue_buffer(session->captureStream, b);
    }

    static void onCalibrationCaptureStateChanged(void *data, pw_stream_state /*old*/,
                                                  pw_stream_state state, const char * /*error*/)
    {
        auto *session = static_cast<CalibrationSession *>(data);
        if (state != PW_STREAM_STATE_PAUSED && state != PW_STREAM_STATE_STREAMING)
            return;
        Impl *impl = session->engineImpl;
        bool justDiscovered = false;
        {
            QMutexLocker locker(&impl->nodesMutex);
            if (session->captureStreamNodeId == 0) {
                const uint32_t nodeId = pw_stream_get_node_id(session->captureStream);
                if (nodeId != SPA_ID_INVALID) {
                    session->captureStreamNodeId = nodeId;
                    justDiscovered = true;
                }
            }
        }
        if (justDiscovered) {
            const uint32_t micNodeId = session->micNodeId;
            const uint32_t captureNodeId = session->captureStreamNodeId;
            QMetaObject::invokeMethod(
                impl->q, [impl, micNodeId, captureNodeId]() { impl->createPortLink(micNodeId, captureNodeId); },
                Qt::QueuedConnection);
        }
    }

    static constexpr pw_stream_events calibrationClickStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onCalibrationClickStateChanged,
        .process = onCalibrationClickProcess,
    };
    static constexpr pw_stream_events calibrationCaptureStreamEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = onCalibrationCaptureStateChanged,
        .process = onCalibrationCaptureProcess,
    };

    // Cerca, a partire da startIndex, la prima finestra di ~5ms la cui RMS
    // superi una soglia (multiplo del rumore di fondo misurato prima del
    // primo click) — un click di rumore bianco ha un fronte netto,
    // sufficiente per questa semplice rilevazione a soglia senza bisogno
    // di una vera cross-correlazione.
    struct OnsetResult
    {
        bool found = false;
        qint64 sampleIndex = -1;
    };
    static OnsetResult findOnsetAfter(const QVector<float> &samples, qint64 startIndex,
                                      qint64 searchWindowSamples, double noiseFloorRms)
    {
        OnsetResult result;
        if (startIndex < 0 || startIndex >= samples.size())
            return result;
        constexpr qint64 kWindowSamples = 240; // ~5ms a 48kHz
        const qint64 endIndex = std::min<qint64>(samples.size(), startIndex + searchWindowSamples);
        const double threshold = std::max(0.01, noiseFloorRms * 6.0);
        for (qint64 i = startIndex; i + kWindowSamples <= endIndex; i += kWindowSamples / 2) {
            double sumSq = 0.0;
            for (qint64 j = 0; j < kWindowSamples; ++j)
                sumSq += static_cast<double>(samples[i + j]) * samples[i + j];
            const double rms = std::sqrt(sumSq / kWindowSamples);
            if (rms >= threshold) {
                result.found = true;
                result.sampleIndex = i;
                return result;
            }
        }
        return result;
    }

    static double computeNoiseFloorRms(const QVector<float> &samples, qint64 upTo)
    {
        upTo = std::min<qint64>(upTo, samples.size());
        if (upTo <= 0)
            return 0.0;
        double sumSq = 0.0;
        for (qint64 i = 0; i < upTo; ++i)
            sumSq += static_cast<double>(samples[i]) * samples[i];
        return std::sqrt(sumSq / upTo);
    }

    // Distrugge gli stream della sessione e analizza l'audio catturato:
    // individua i due istanti di arrivo (uno per sink) e ne ricava la
    // differenza di latenza. Il chiamante resta proprietario di
    // `calibration` (lo azzera dopo questa chiamata).
    void finishCalibration(CalibrationSession *session)
    {
        pw_thread_loop_lock(loop);
        pw_stream_destroy(session->clickStream);
        pw_stream_destroy(session->captureStream);
        pw_thread_loop_unlock(loop);

        QVector<float> samples;
        qint64 fireA = -1, fireB = -1;
        const uint32_t rate = session->rate;
        {
            QMutexLocker locker(&nodesMutex);
            samples = session->capturedSamples;
            fireA = session->fireOffsetA;
            fireB = session->fireOffsetB;
        }

        const uint32_t sinkA = session->sinkA;
        const uint32_t sinkB = session->sinkB;

        if (fireA < 0 || fireB < 0 || samples.isEmpty()) {
            emit q->calibrationFinished(sinkA, sinkB, 0, false,
                QStringLiteral("Calibrazione interrotta prima di completarsi"));
            return;
        }

        const double noiseFloor = computeNoiseFloorRms(samples, fireA);
        const qint64 searchWindowSamples = static_cast<qint64>(0.9 * rate); // ~900ms di margine dopo ogni click
        const OnsetResult onsetA = findOnsetAfter(samples, fireA, searchWindowSamples, noiseFloor);
        const OnsetResult onsetB = findOnsetAfter(samples, fireB, searchWindowSamples, noiseFloor);

        // Diagnostica lasciata volutamente (non solo temporanea): l'unico
        // modo per capire un risultato sbagliato/sospetto senza dover
        // rifare il test è avere questi numeri nel log — vedi
        // journalctl --user.
        qDebug() << "PipeWireEngine: calibrazione -- fireA" << fireA
                 << "onsetA" << (onsetA.found ? onsetA.sampleIndex : -1)
                 << "fireB" << fireB
                 << "onsetB" << (onsetB.found ? onsetB.sampleIndex : -1)
                 << "rumoreDiFondo" << noiseFloor << "campioniTotali" << samples.size();

        if (!onsetA.found || !onsetB.found) {
            emit q->calibrationFinished(sinkA, sinkB, 0, false,
                QStringLiteral("Click non rilevato chiaramente (ambiente troppo rumoroso o microfono troppo lontano) — riprova"));
            return;
        }

        const qint64 latencyASamples = onsetA.sampleIndex - fireA;
        const qint64 latencyBSamples = onsetB.sampleIndex - fireB;
        const qint64 deltaSamples = latencyBSamples - latencyASamples;
        const int deltaMs = static_cast<int>(std::llround(static_cast<double>(deltaSamples) * 1000.0 / rate));

        qDebug() << "PipeWireEngine: calibrazione -- latenzaA(campioni)" << latencyASamples
                 << "latenzaB(campioni)" << latencyBSamples << "deltaMs" << deltaMs;

        emit q->calibrationFinished(sinkA, sinkB, deltaMs, true, QString());
    }

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

        // Bind pigro alla metadata "default", quella con cui
        // setStreamTarget/clearStreamTarget "spostano" lo stream di
        // un'applicazione verso un sink virtuale nostro (stessa metadata
        // che usano pavucontrol/wpctl). Ne esiste una sola per sessione
        // PipeWire: il primo avvistamento è quello buono, ignora eventuali
        // successivi.
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const char *metadataName = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (metadataName && std::strcmp(metadataName, "default") == 0 && !impl->defaultMetadata) {
                impl->defaultMetadataProxy = static_cast<pw_proxy *>(pw_registry_bind(
                    impl->registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
                if (impl->defaultMetadataProxy) {
                    impl->defaultMetadata = reinterpret_cast<pw_metadata *>(impl->defaultMetadataProxy);
                    pw_metadata_add_listener(impl->defaultMetadata, &impl->defaultMetadataListener,
                                              &Impl::defaultMetadataEvents, impl);
                }
            }
            return;
        }

        // Traccia OGNI link del grafo (non solo quelli creati da noi, a
        // differenza di activeLinks) — usato da setStreamTarget per capire
        // se uno stream applicativo è già collegato al sink giusto o va
        // "rubato" da dove si trova ora. PW_KEY_LINK_OUTPUT_PORT/
        // PW_KEY_LINK_INPUT_PORT sono stringhe numeriche nelle props.
        if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
            const char *outPortStr = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_PORT);
            const char *inPortStr = spa_dict_lookup(props, PW_KEY_LINK_INPUT_PORT);
            if (outPortStr && inPortStr) {
                QMutexLocker locker(&impl->nodesMutex);
                impl->allLinks.append(Impl::ForeignLink{
                    id,
                    static_cast<uint32_t>(std::strtoul(outPortStr, nullptr, 10)),
                    static_cast<uint32_t>(std::strtoul(inPortStr, nullptr, 10)) });
            }
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

        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *description = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        node.name = name ? QString::fromUtf8(name) : QString();
        node.description = description ? QString::fromUtf8(description) : node.name;

        if (kind == AudioNode::Kind::AppStream) {
            const char *pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
            if (pidStr)
                node.appProcessId = static_cast<uint32_t>(std::strtoul(pidStr, nullptr, 10));
        }

        // Correla per NOME (l'unico identificatore noto al momento della
        // createVirtualSink, il nodeId arriva solo ora) un sink appena
        // scoperto con uno creato da noi: se corrisponde, memorizza il
        // nodeId ora noto (usato da removeVirtualSink per distruggere il
        // modulo giusto) e lo marca Kind::VirtualSink, cosi' non viene
        // scambiato per un dispositivo fisico.
        if (kind == AudioNode::Kind::PhysicalSink && !node.name.isEmpty()) {
            QMutexLocker locker(&impl->nodesMutex);
            const auto it = impl->virtualSinks.find(node.name);
            if (it != impl->virtualSinks.end()) {
                it.value().nodeId = id;
                node.kind = AudioNode::Kind::VirtualSink;
                if (!impl->ownedVirtualSinkIds.contains(id))
                    impl->ownedVirtualSinkIds.append(id);
            }
        }

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

            impl->allLinks.erase(
                std::remove_if(impl->allLinks.begin(), impl->allLinks.end(),
                                [&](const Impl::ForeignLink &l) { return l.linkId == id; }),
                impl->allLinks.end());

            impl->ownedVirtualSinkIds.removeAll(id);
            // Il modulo potrebbe essere già stato distrutto da noi
            // (removeVirtualSink, che rimuove la voce PRIMA di arrivare
            // qui) o essere scomparso per altre vie: in entrambi i casi qui
            // ci limitiamo a smettere di tracciarlo, mai a distruggere un
            // modulo potenzialmente già invalido.
            for (auto it = impl->virtualSinks.begin(); it != impl->virtualSinks.end(); ++it) {
                if (it.value().nodeId == id) {
                    impl->virtualSinks.erase(it);
                    break;
                }
            }
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

    // true se ESISTE GIÀ almeno un link (nostro o di chiunque altro, vedi
    // allLinks) da una porta di uscita di sourceNodeId a una porta di
    // ingresso di targetNodeId — usato da setStreamTarget per capire se lo
    // "spostamento" è già avvenuto (nel qual caso non c'è nulla da fare) o
    // se lo stream va ancora preso in carico manualmente.
    bool isNodeLinkedToNode(uint32_t sourceNodeId, uint32_t targetNodeId) const
    {
        QMutexLocker locker(&nodesMutex);
        QSet<uint32_t> sourceOutPorts;
        QSet<uint32_t> targetInPorts;
        for (const TrackedPort &p : ports) {
            if (p.nodeId == sourceNodeId && p.isOutput)
                sourceOutPorts.insert(p.id);
            else if (p.nodeId == targetNodeId && !p.isOutput)
                targetInPorts.insert(p.id);
        }
        for (const ForeignLink &l : allLinks) {
            if (sourceOutPorts.contains(l.outputPortId) && targetInPorts.contains(l.inputPortId))
                return true;
        }
        return false;
    }

    // Distrugge QUALUNQUE link esistente le cui porte di uscita
    // appartengono a sourceNodeId, indipendentemente da chi lo abbia creato
    // o da dove punti (activeLinks traccia solo i nostri) — usa
    // pw_registry_destroy (non bind+pw_proxy_destroy: quest'ultimo
    // rilascerebbe solo il NOSTRO riferimento locale senza chiedere al
    // server di distruggere davvero l'oggetto altrui), la stessa primitiva
    // usata da "pw-link -d". Necessario per "rubare" uno stream applicativo
    // già collegato al sink di sistema di default quando lo si vuole
    // reindirizzare verso un sink nostro — vedi il commento su
    // setStreamTarget per il perché la sola metadata non basta sempre.
    void destroyForeignLinksFromNode(uint32_t sourceNodeId)
    {
        QVector<uint32_t> linkIdsToDestroy;
        {
            QMutexLocker locker(&nodesMutex);
            QSet<uint32_t> sourceOutPorts;
            for (const TrackedPort &p : ports) {
                if (p.nodeId == sourceNodeId && p.isOutput)
                    sourceOutPorts.insert(p.id);
            }
            for (const ForeignLink &l : allLinks) {
                if (sourceOutPorts.contains(l.outputPortId))
                    linkIdsToDestroy.append(l.linkId);
            }
        }
        if (linkIdsToDestroy.isEmpty() || !registry)
            return;

        pw_thread_loop_lock(loop);
        for (uint32_t linkId : linkIdsToDestroy)
            pw_registry_destroy(registry, linkId);
        pw_thread_loop_unlock(loop);
    }

    // Corpo effettivo di linkNodes(): abbina le porte dei due nodi indicati
    // per indice di canale e crea un link PipeWire reale per coppia.
    // Estratto in un metodo Impl separato (invece di restare dentro
    // PipeWireEngine::linkNodes) perché il ritardo di output (vedi
    // DelayFilter sotto) deve poter creare IL PROPRIO link permanente
    // filtro->sink SENZA passare dal reindirizzamento automatico che
    // PipeWireEngine::linkNodes applica quando il nodo di destinazione ha
    // già un filtro — altrimenti quel link si richiuderebbe su se stesso
    // invece di raggiungere il sink vero.
    uint32_t createPortLink(uint32_t outputNodeId, uint32_t inputNodeId)
    {
        if (!loop || !core) {
            emit q->engineError(QStringLiteral("Engine non avviato"));
            return 0;
        }

        // Le porte di un nodo arrivano dal pw_registry come eventi separati
        // e asincroni rispetto al nodo stesso: se questo metodo viene
        // chiamato a ridosso della comparsa di un nodo nuovo, le sue porte
        // potrebbero non essere ancora note. Ritentiamo per una finestra
        // breve, rilasciando il lock tra un tentativo e l'altro (altrimenti
        // il thread PipeWire non può consegnare gli eventi in sospeso),
        // prima di arrenderci.
        constexpr int kMaxPortLookupAttempts = 5;
        constexpr unsigned long kPortLookupRetryDelayMs = 40;

        QVector<TrackedPort> outPorts;
        QVector<TrackedPort> inPorts;
        for (int attempt = 0; attempt < kMaxPortLookupAttempts; ++attempt) {
            pw_thread_loop_lock(loop);
            {
                QMutexLocker locker(&nodesMutex);
                outPorts = portsForNode(outputNodeId, /*wantOutput=*/true);
                inPorts = portsForNode(inputNodeId, /*wantOutput=*/false);
            }
            pw_thread_loop_unlock(loop);

            if (!outPorts.isEmpty() && !inPorts.isEmpty())
                break;
            if (attempt + 1 < kMaxPortLookupAttempts)
                QThread::msleep(kPortLookupRetryDelayMs);
        }

        if (outPorts.isEmpty() || inPorts.isEmpty()) {
            // Include gli id e se il nodo stesso è ancora noto (distingue
            // "nodo sparito nel frattempo" — es. uno stream applicativo
            // che Firefox ha già distrutto/ricreato per un nuovo video —
            // da "nodo presente ma le sue porte non sono ancora arrivate",
            // due cause molto diverse dietro lo stesso sintomo. Aggiunto
            // dopo che il messaggio generico non bastava a diagnosticare
            // un fallimento reale segnalato dall'utente (cattura di uno
            // stream Firefox verso un altoparlante Bluetooth).
            bool outNodeKnown = false;
            bool inNodeKnown = false;
            {
                QMutexLocker locker(&nodesMutex);
                for (const AudioNode &n : nodes) {
                    if (n.id == outputNodeId)
                        outNodeKnown = true;
                    if (n.id == inputNodeId)
                        inNodeKnown = true;
                }
            }
            emit q->engineError(QStringLiteral(
                "Porte non trovate: output=%1 (nodo %2, %3 porte) -> input=%4 (nodo %5, %6 porte)")
                .arg(outputNodeId)
                .arg(outNodeKnown ? QStringLiteral("presente") : QStringLiteral("SPARITO"))
                .arg(outPorts.size())
                .arg(inputNodeId)
                .arg(inNodeKnown ? QStringLiteral("presente") : QStringLiteral("SPARITO"))
                .arg(inPorts.size()));
            return 0;
        }

        pw_thread_loop_lock(loop);

        // Abbiniamo le porte per indice di canale: se un lato ha meno canali
        // dell'altro (es. sorgente mono verso sink stereo), duplichiamo
        // l'unica porta di output su tutte le porte di input rimanenti, cosi'
        // il mono viene inviato a entrambi i canali invece di lasciarne uno
        // muto.
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
                core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
                &linkProps->dict, 0));

            pw_properties_free(linkProps);

            if (!proxy) {
                // Rollback dei link gia' creati in questa chiamata, per non
                // lasciare un routing parziale (es. solo il canale sinistro).
                for (pw_proxy *created : createdProxies)
                    pw_proxy_destroy(created);
                pw_thread_loop_unlock(loop);
                emit q->engineError(QStringLiteral("Creazione del link PipeWire fallita"));
                return 0;
            }

            createdProxies.append(proxy);
        }

        pw_thread_loop_unlock(loop);

        const uint32_t localId = nextLocalLinkId++;
        activeLinks.append(ActiveLink{ localId, createdProxies });

        QMetaObject::invokeMethod(
            q, [this, localId]() { emit q->linkStateChanged(localId, true); }, Qt::QueuedConnection);

        return localId;
    }

    // --- Ritardo di output: un pw_filter interposto tra le sorgenti e un
    // sink reale, per compensare la latenza maggiore di un altro output
    // (tipicamente Bluetooth, che aggiunge trasporto+decodifica A2DP)
    // quando la stessa traccia suona contemporaneamente su entrambi —
    // richiesto esplicitamente dall'utente. Creato pigramente (come il
    // generatore keepalive) alla prima richiesta con delayMs>0 per un dato
    // sink, e mai distrutto per il resto della sessione anche se il ritardo
    // torna a 0 — evita di dover ricollegare la topologia esistente ad ogni
    // cambio, il buffer di ritardo diventa semplicemente un passthrough
    // (delayFrames=0).
    //
    // Due porte per canale: un ingresso (dove si collegano le sorgenti al
    // posto del sink reale, vedi il reindirizzamento in
    // PipeWireEngine::linkNodes) e un'uscita, collegata UNA VOLTA SOLA e
    // permanentemente al sink reale non appena PipeWire assegna il nodeId
    // del filtro (asincrono, stesso pattern di FileStream/keepalive). Un
    // ring buffer per canale (letto/scritto SOLO dal thread audio in
    // onDelayFilterProcess) realizza il ritardo vero e proprio: ogni ciclo
    // scrive i campioni in ingresso in coda al buffer e legge quelli in
    // uscita da "delayFrames" campioni indietro. delayFrames è un
    // std::atomic (scritto dal thread Qt via setOutputDelayMs, letto dal
    // thread audio) per non introdurre un lock nel percorso realtime,
    // stesso principio già usato per FileStream::reverseRequested.
    struct DelayFilter
    {
        Impl *engineImpl = nullptr;
        pw_filter *filter = nullptr;
        spa_hook filterListener{};
        uint32_t sinkNodeId = 0; // sink reale a cui questo filtro è collegato
        uint32_t filterNodeId = 0; // 0 finché non ancora noto (asincrono)
        int channels = 2;
        uint32_t rate = 48000;
        std::atomic<int> delayFrames{0};

        struct ChannelPort
        {
            void *inPort = nullptr;
            void *outPort = nullptr;
            QVector<float> ring; // capacità fissa, letto/scritto SOLO dal thread audio
            uint32_t writePos = 0;
        };
        QVector<ChannelPort> chans;
    };
    QMap<uint32_t, DelayFilter *> delayFilters; // sinkNodeId -> filtro

    // Capacità del ring buffer in secondi: limita anche il ritardo massimo
    // impostabile (vedi PipeWireEngine::setOutputDelayMs, che comunque
    // limita l'input a 2000ms, ben sotto questo margine).
    static constexpr uint32_t kDelayRingSeconds = 3;

    static void onDelayFilterProcess(void *data, struct spa_io_position *position)
    {
        auto *df = static_cast<DelayFilter *>(data);
        if (!position)
            return;
        const uint32_t n = static_cast<uint32_t>(position->clock.duration);
        if (n == 0)
            return;

        const int delayFrames = df->delayFrames.load(std::memory_order_relaxed);

        for (int ch = 0; ch < df->channels; ++ch) {
            DelayFilter::ChannelPort &cp = df->chans[ch];
            const uint32_t cap = static_cast<uint32_t>(cp.ring.size());
            if (cap == 0)
                continue;
            const uint32_t framesThisCycle = std::min(n, cap);

            auto *in = static_cast<float *>(pw_filter_get_dsp_buffer(cp.inPort, framesThisCycle));
            auto *out = static_cast<float *>(pw_filter_get_dsp_buffer(cp.outPort, framesThisCycle));

            const uint32_t wp = cp.writePos;
            for (uint32_t i = 0; i < framesThisCycle; ++i)
                cp.ring[(wp + i) % cap] = in ? in[i] : 0.0f;

            if (out) {
                const uint32_t clampedDelay =
                    static_cast<uint32_t>(std::clamp(delayFrames, 0, static_cast<int>(cap) - 1));
                const uint32_t rp = (wp + cap - clampedDelay) % cap;
                for (uint32_t i = 0; i < framesThisCycle; ++i)
                    out[i] = cp.ring[(rp + i) % cap];
            }

            cp.writePos = (wp + framesThisCycle) % cap;
        }
    }

    // Non appena il filtro raggiunge PAUSED/STREAMING, il suo nodeId è
    // finalmente noto (pw_filter_get_node_id) — crea allora, UNA SOLA VOLTA,
    // il link permanente uscita-filtro -> ingresso-sink reale, usando
    // createPortLink() direttamente (NON PipeWireEngine::linkNodes, che
    // reindirizzerebbe di nuovo su questo stesso filtro, creando un
    // autoanello invece di raggiungere il sink). Marshalling sul thread Qt
    // come già fatto per onKeepAliveStateChanged: createPortLink() blocca
    // pw_thread_loop internamente, e questa callback arriva già sul thread
    // PipeWire (non RT anche con RT_PROCESS, che riguarda solo "process").
    static void onDelayFilterStateChanged(void *data, pw_filter_state /*old*/,
                                           pw_filter_state state, const char * /*error*/)
    {
        auto *df = static_cast<DelayFilter *>(data);
        if (state != PW_FILTER_STATE_PAUSED && state != PW_FILTER_STATE_STREAMING)
            return;
        if (df->filterNodeId != 0)
            return;

        const uint32_t nodeId = pw_filter_get_node_id(df->filter);
        if (nodeId == SPA_ID_INVALID)
            return;
        df->filterNodeId = nodeId;

        Impl *impl = df->engineImpl;
        const uint32_t sinkNodeId = df->sinkNodeId;
        QMetaObject::invokeMethod(
            impl->q, [impl, nodeId, sinkNodeId]() { impl->createPortLink(nodeId, sinkNodeId); },
            Qt::QueuedConnection);
    }

    static constexpr pw_filter_events delayFilterEvents = {
        .version = PW_VERSION_FILTER_EVENTS,
        .state_changed = onDelayFilterStateChanged,
        .process = onDelayFilterProcess,
    };

    // Crea (se non esiste già) il filtro di ritardo per sinkNodeId. Ritorna
    // nullptr solo per un errore PipeWire immediato (engine non avviato,
    // pw_filter_new/connect falliti) — segnalato via engineError.
    DelayFilter *ensureDelayFilter(uint32_t sinkNodeId)
    {
        const auto existing = delayFilters.find(sinkNodeId);
        if (existing != delayFilters.end())
            return existing.value();

        if (!loop || !core) {
            emit q->engineError(QStringLiteral("Engine non avviato"));
            return nullptr;
        }

        auto *df = new DelayFilter();
        df->engineImpl = this;
        df->sinkNodeId = sinkNodeId;
        df->channels = 2;
        df->rate = 48000;

        const uint32_t ringCapacity = df->rate * kDelayRingSeconds;
        df->chans.resize(df->channels);
        for (auto &cp : df->chans)
            cp.ring.fill(0.0f, static_cast<int>(ringCapacity));

        pw_thread_loop_lock(loop);

        const QByteArray nameUtf8 =
            QByteArrayLiteral("bluecue.delay.") + QByteArray::number(sinkNodeId);
        pw_properties *filterProps = pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Filter",
            PW_KEY_NODE_NAME, nameUtf8.constData(),
            PW_KEY_NODE_DESCRIPTION, "BT Multizone output delay",
            nullptr);

        df->filter = pw_filter_new(core, nameUtf8.constData(), filterProps);
        if (!df->filter) {
            pw_thread_loop_unlock(loop);
            delete df;
            emit q->engineError(QStringLiteral("Impossibile creare il filtro di ritardo"));
            return nullptr;
        }

        pw_filter_add_listener(df->filter, &df->filterListener, &delayFilterEvents, df);

        spa_audio_info_dsp dspInfo{};
        dspInfo.format = SPA_AUDIO_FORMAT_DSP_F32;

        for (int ch = 0; ch < df->channels; ++ch) {
            uint8_t inPodBuffer[512];
            spa_pod_builder inBuilder = SPA_POD_BUILDER_INIT(inPodBuffer, sizeof(inPodBuffer));
            const spa_pod *inParams[1];
            inParams[0] = spa_format_audio_dsp_build(&inBuilder, SPA_PARAM_EnumFormat, &dspInfo);

            const QByteArray inPortName = QByteArrayLiteral("in_") + QByteArray::number(ch);
            pw_properties *inPortProps = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, inPortName.constData(),
                nullptr);
            df->chans[ch].inPort = pw_filter_add_port(
                df->filter, PW_DIRECTION_INPUT, PW_FILTER_PORT_FLAG_NONE,
                0, inPortProps, inParams, 1);

            uint8_t outPodBuffer[512];
            spa_pod_builder outBuilder = SPA_POD_BUILDER_INIT(outPodBuffer, sizeof(outPodBuffer));
            const spa_pod *outParams[1];
            outParams[0] = spa_format_audio_dsp_build(&outBuilder, SPA_PARAM_EnumFormat, &dspInfo);

            const QByteArray outPortName = QByteArrayLiteral("out_") + QByteArray::number(ch);
            pw_properties *outPortProps = pw_properties_new(
                PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                PW_KEY_PORT_NAME, outPortName.constData(),
                nullptr);
            df->chans[ch].outPort = pw_filter_add_port(
                df->filter, PW_DIRECTION_OUTPUT, PW_FILTER_PORT_FLAG_NONE,
                0, outPortProps, outParams, 1);
        }

        const int res = pw_filter_connect(df->filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);

        pw_thread_loop_unlock(loop);

        if (res < 0) {
            pw_thread_loop_lock(loop);
            pw_filter_destroy(df->filter);
            pw_thread_loop_unlock(loop);
            delete df;
            emit q->engineError(QStringLiteral("Impossibile avviare il filtro di ritardo"));
            return nullptr;
        }

        delayFilters.insert(sinkNodeId, df);
        return df;
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

        for (Impl::DelayFilter *df : std::as_const(d->delayFilters))
            pw_filter_destroy(df->filter);

        for (const Impl::VirtualSinkInfo &info : std::as_const(d->virtualSinks))
            pw_impl_module_destroy(info.module);

        if (d->defaultMetadataProxy)
            pw_proxy_destroy(d->defaultMetadataProxy);

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

    qDeleteAll(d->delayFilters);
    d->delayFilters.clear();

    d->virtualSinks.clear();
    d->ownedVirtualSinkIds.clear();
    d->defaultMetadataProxy = nullptr;
    d->defaultMetadata = nullptr;

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

    // node.autoconnect=false sul lato "playback" (il lato interno del
    // modulo che normalmente rimanderebbe l'audio catturato sull'uscita di
    // sistema di default): SENZA questo, l'audio arrivato al sink virtuale
    // verrebbe anche automaticamente rimandato là, DUPLICATO — verificato
    // empiricamente (pw-dump) durante lo sviluppo di questa funzione: senza
    // il flag, il nodo "<name>-in" risultava sempre collegato al sink di
    // sistema. Con il flag resta scollegato, e l'audio catturato è
    // disponibile SOLO sulle porte monitor_* del sink virtuale — quelle che
    // PatchManager collega esplicitamente con linkNodes verso gli output
    // scelti dall'utente (esattamente come richiesto: "spostare" l'audio di
    // un'app nella patch bay, non duplicarlo).
    // "node.name" a livello TOP (non solo dentro capture.props/
    // playback.props) è indispensabile: senza di esso il modulo non
    // assegna un node.group/node.link-group coerente alle due metà —
    // verificato empiricamente confrontando questo sink con uno creato
    // dalla CLI pw-loopback (che imposta sempre un nome a questo livello
    // con "-n"): SENZA questa proprietà, il reindirizzamento di un altro
    // stream qui tramite setStreamTarget veniva accettato dalla metadata
    // (nessun errore, valore visibile con pw-metadata) ma WirePlumber non
    // spostava mai davvero il link — CON questa proprietà, identico in
    // tutto il resto, funziona. Nome ripetuto qui invece che in
    // capture.props così l'intero modulo (entrambe le metà) condivide la
    // stessa identità di gruppo, non solo il lato sink.
    // audio.channels/audio.position espliciti: senza, i test empirici
    // mostravano un sink che accettava collegamenti in ingresso ma NON
    // veniva mai accettato come target.object valido da WirePlumber per
    // uno stream esterno reindirizzato con setStreamTarget (la metadata
    // veniva impostata senza errori, ma il link non si spostava mai —
    // confrontato con un sink creato dalla CLI pw-loopback, che imposta
    // sempre questi due valori di default e per cui il reindirizzamento
    // funzionava). Stereo hardcoded, coerente con il resto del codebase
    // (keepalive/delay filter assumono lo stesso).
    const QByteArray args = QByteArray(
        "{ node.name = \"") + nameUtf8 + QByteArray("\" "
        "node.description = \"") + descUtf8 + QByteArray("\" "
        "capture.props = { node.name = \"") + nameUtf8 + QByteArray("\" "
        "media.class = Audio/Sink "
        "audio.channels = 2 "
        "audio.position = [ FL, FR ] } "
        "playback.props = { node.name = \"") + nameUtf8 + QByteArray("-in\" "
        "node.autoconnect = false } }");

    pw_impl_module *module = pw_context_load_module(
        d->context, "libpipewire-module-loopback", args.constData(), nullptr);

    if (module) {
        QMutexLocker locker(&d->nodesMutex);
        d->virtualSinks.insert(name, Impl::VirtualSinkInfo{ module, 0 });
    }

    pw_thread_loop_unlock(d->loop);

    if (!module) {
        emit engineError(QStringLiteral("Impossibile creare il sink virtuale '%1'").arg(name));
        return;
    }

    // Il nodo risultante arriva tramite la normale callback onGlobal() del
    // registry; il suo id numerico (e la correlazione VirtualSinkInfo::nodeId)
    // si scopre solo lì, correlato per nome (vedi onGlobal).
}

void PipeWireEngine::removeVirtualSink(uint32_t nodeId)
{
    if (!d->loop)
        return;

    pw_impl_module *module = nullptr;
    {
        QMutexLocker locker(&d->nodesMutex);
        for (auto it = d->virtualSinks.begin(); it != d->virtualSinks.end(); ++it) {
            if (it.value().nodeId == nodeId) {
                module = it.value().module;
                d->virtualSinks.erase(it);
                break;
            }
        }
        d->ownedVirtualSinkIds.removeAll(nodeId);
    }

    if (!module)
        return; // non un sink virtuale nostro (o già rimosso): no-op

    pw_thread_loop_lock(d->loop);
    pw_impl_module_destroy(module);
    pw_thread_loop_unlock(d->loop);
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
    // Se il sink di destinazione ha già un filtro di ritardo attivo (vedi
    // setOutputDelayMs/Impl::DelayFilter) collega alla sua porta di ingresso
    // invece che al sink direttamente — il filtro stesso resta collegato al
    // sink reale da un link permanente separato, creato una sola volta non
    // appena il suo nodeId diventa noto. Se il filtro esiste ma il suo
    // nodeId non è ancora noto (finestra breve subito dopo la primissima
    // richiesta di ritardo per questo sink), si collega comunque al sink
    // direttamente per non bloccare la riproduzione: il ritardo si applicherà
    // dalla prossima chiamata.
    uint32_t effectiveInputNodeId = inputNodeId;
    const auto it = d->delayFilters.constFind(inputNodeId);
    if (it != d->delayFilters.constEnd() && it.value()->filterNodeId != 0)
        effectiveInputNodeId = it.value()->filterNodeId;

    return d->createPortLink(outputNodeId, effectiveInputNodeId);
}

void PipeWireEngine::setOutputDelayMs(uint32_t sinkNodeId, int delayMs)
{
    delayMs = std::clamp(delayMs, 0, 2000);
    if (delayMs <= 0 && !d->delayFilters.contains(sinkNodeId))
        return; // nessun filtro mai creato per questo sink: routing diretto invariato

    Impl::DelayFilter *df = d->ensureDelayFilter(sinkNodeId);
    if (!df)
        return;

    const int frames = static_cast<int>((static_cast<int64_t>(delayMs) * df->rate) / 1000);
    df->delayFrames.store(frames, std::memory_order_relaxed);
}

void PipeWireEngine::setStreamTarget(uint32_t streamNodeId, uint32_t targetSinkNodeId, const QString &targetSinkName)
{
    if (!d->loop || !d->core)
        return;

    // Metadata "target.object": un suggerimento per WirePlumber, non un
    // ordine garantito. Il TIPO conta — un valore stringa JSON-quotato
    // ("\"nome\"") o senza tipo esplicito viene accettato dalla metadata
    // ma IGNORATO dallo script di routing di WirePlumber (verificato
    // empiricamente: nessun errore, la metadata risultava impostata
    // correttamente interrogandola, ma lo spostamento non avveniva). Con
    // "Spa:String" e il nome NON quotato invece funziona, quando funziona.
    if (d->defaultMetadata) {
        pw_thread_loop_lock(d->loop);
        const QByteArray nameUtf8 = targetSinkName.toUtf8();
        pw_metadata_set_property(d->defaultMetadata, streamNodeId, "target.object", "Spa:String", nameUtf8.constData());
        pw_thread_loop_unlock(d->loop);
    }

    // La sola metadata NON basta sempre. Verificato empiricamente durante
    // lo sviluppo: con un DelayFilter presente ovunque nel grafo (vedi
    // sopra) — anche per un sink completamente estraneo a questo
    // spostamento — WirePlumber smette di onorare target.object per
    // QUALUNQUE stream, silenziosamente (la metadata risulta impostata
    // correttamente se interrogata con pw-metadata, ma il link non si
    // sposta mai). Non è stato possibile determinare la causa esatta
    // all'interno di WirePlumber in tempi ragionevoli; la soluzione
    // robusta è prendere il controllo diretto del link, esattamente come
    // facciamo già per tutto il resto del routing di quest'app, invece di
    // dipendere dalla cooperazione (talvolta assente) del session manager:
    // se lo stream non risulta già collegato al sink di destinazione,
    // distruggiamo qualunque suo link esistente (verso qualunque
    // destinazione fosse, tipicamente il sink di sistema di default) e ne
    // creiamo uno nuovo noi stessi. Chiamato anche periodicamente
    // (PatchManager::m_appStreamReassertTimer): idempotente, non fa nulla
    // se il link è già quello giusto.
    if (!d->isNodeLinkedToNode(streamNodeId, targetSinkNodeId)) {
        // Verifica preventiva che ENTRAMBI i nodi abbiano già le porte
        // giuste PRIMA di staccare lo stream da dove si trova ora: senza
        // questo controllo, un sink target le cui porte non sono ancora
        // pronte (discovery asincrono, stessa razza documentata altrove in
        // questo file) causava comunque la distruzione del link
        // esistente — silenziando lo stream — seguita da un
        // createPortLink che falliva comunque ("Porte non trovate"),
        // lasciandolo scollegato finché il prossimo giro del timer di
        // auto-recovery non ripeteva lo stesso ciclo dannoso. Meglio
        // aspettare il prossimo giro senza toccare nulla.
        bool streamHasOutPorts = false;
        bool sinkHasInPorts = false;
        {
            QMutexLocker locker(&d->nodesMutex);
            streamHasOutPorts = !d->portsForNode(streamNodeId, /*wantOutput=*/true).isEmpty();
            sinkHasInPorts = !d->portsForNode(targetSinkNodeId, /*wantOutput=*/false).isEmpty();
        }
        if (streamHasOutPorts && sinkHasInPorts) {
            d->destroyForeignLinksFromNode(streamNodeId);
            d->createPortLink(streamNodeId, targetSinkNodeId);
        } else {
            qDebug() << "PipeWireEngine::setStreamTarget: rimando, porte non ancora pronte -- stream" << streamNodeId
                     << "ha porte out:" << streamHasOutPorts << "| sink" << targetSinkNodeId
                     << "ha porte in:" << sinkHasInPorts;
        }
    }
}

void PipeWireEngine::clearStreamTarget(uint32_t streamNodeId)
{
    if (!d->loop || !d->core)
        return;

    if (d->defaultMetadata) {
        pw_thread_loop_lock(d->loop);
        pw_metadata_set_property(d->defaultMetadata, streamNodeId, "target.object", nullptr, nullptr);
        pw_thread_loop_unlock(d->loop);
    }

    // Distrugge il link manuale creato da setStreamTarget: senza questo,
    // lo stream resterebbe collegato al NOSTRO sink di cattura (che stiamo
    // per rimuovere).
    d->destroyForeignLinksFromNode(streamNodeId);

    // Ricollega esplicitamente al sink di sistema di default corrente
    // (risolto da defaultAudioSinkName, tenuto sincronizzato dalla
    // metadata "default.audio.sink") invece di limitarsi a scollegare e
    // sperare che WirePlumber lo ricolleghi da solo. Verificato
    // empiricamente NON essere affidabile: uno stream a cui era stato
    // appena rimosso un target.object esplicito, anche scollegato e con la
    // metadata già cancellata, poteva restare scollegato indefinitamente
    // (osservato per 10+ secondi in un test controllato) — a differenza di
    // un semplice scollegamento "grezzo" mai passato da un target
    // esplicito, che invece il session manager riprende in carico da solo.
    // Stesso principio già applicato in setStreamTarget: prendersi il
    // controllo diretto del link invece di dipendere dalla cooperazione
    // (qui dimostrata inaffidabile) del session manager.
    const uint32_t defaultSinkId = d->resolveNodeIdByName(d->defaultAudioSinkName);
    if (defaultSinkId != 0)
        d->createPortLink(streamNodeId, defaultSinkId);
}

void PipeWireEngine::calibrateOutputDelay(uint32_t sinkNodeIdA, uint32_t sinkNodeIdB, uint32_t micNodeId)
{
    if (!d->loop || !d->core) {
        emit engineError(QStringLiteral("Engine non avviato"));
        return;
    }
    if (d->calibration) {
        emit engineError(QStringLiteral("Una calibrazione è già in corso"));
        return;
    }

    auto session = std::make_unique<Impl::CalibrationSession>();
    session->engineImpl = d.get();
    session->sinkA = sinkNodeIdA;
    session->sinkB = sinkNodeIdB;
    session->micNodeId = micNodeId;
    session->rate = 48000; // coerente col resto del codebase (keepalive/delay filter assumono lo stesso)

    pw_thread_loop_lock(d->loop);

    // --- Stream generatore del click (uscita, stereo, riusato per A e B) ---
    const QByteArray clickName = QByteArrayLiteral("bluecue.calibration.click");
    pw_properties *clickProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, clickName.constData(),
        PW_KEY_NODE_DESCRIPTION, "BT Multizone calibration click",
        nullptr);
    session->clickStream = pw_stream_new(d->core, clickName.constData(), clickProps);
    if (!session->clickStream) {
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile creare lo stream di calibrazione"));
        return;
    }
    pw_stream_add_listener(session->clickStream, &session->clickStreamListener,
                            &Impl::calibrationClickStreamEvents, session.get());

    uint8_t clickPodBuffer[1024];
    spa_pod_builder clickPodBuilder = SPA_POD_BUILDER_INIT(clickPodBuffer, sizeof(clickPodBuffer));
    spa_audio_info_raw clickAudioInfo{};
    clickAudioInfo.format = SPA_AUDIO_FORMAT_F32;
    clickAudioInfo.channels = 2;
    clickAudioInfo.rate = session->rate;
    const spa_pod *clickParams[1];
    clickParams[0] = spa_format_audio_raw_build(&clickPodBuilder, SPA_PARAM_EnumFormat, &clickAudioInfo);

    int res = pw_stream_connect(session->clickStream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                 static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), clickParams, 1);
    if (res < 0) {
        pw_stream_destroy(session->clickStream);
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile avviare lo stream di calibrazione"));
        return;
    }

    // --- Stream di cattura dal microfono (ingresso, mono) ---
    const QByteArray captureName = QByteArrayLiteral("bluecue.calibration.capture");
    pw_properties *captureProps = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Production",
        PW_KEY_NODE_NAME, captureName.constData(),
        PW_KEY_NODE_DESCRIPTION, "BT Multizone calibration capture",
        nullptr);
    session->captureStream = pw_stream_new(d->core, captureName.constData(), captureProps);
    if (!session->captureStream) {
        pw_stream_destroy(session->clickStream);
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile creare lo stream di registrazione per la calibrazione"));
        return;
    }
    pw_stream_add_listener(session->captureStream, &session->captureStreamListener,
                            &Impl::calibrationCaptureStreamEvents, session.get());

    uint8_t capturePodBuffer[1024];
    spa_pod_builder capturePodBuilder = SPA_POD_BUILDER_INIT(capturePodBuffer, sizeof(capturePodBuffer));
    spa_audio_info_raw captureAudioInfo{};
    captureAudioInfo.format = SPA_AUDIO_FORMAT_F32;
    captureAudioInfo.channels = 1;
    captureAudioInfo.rate = session->rate;
    const spa_pod *captureParams[1];
    captureParams[0] = spa_format_audio_raw_build(&capturePodBuilder, SPA_PARAM_EnumFormat, &captureAudioInfo);

    res = pw_stream_connect(session->captureStream, PW_DIRECTION_INPUT, PW_ID_ANY,
                             static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS), captureParams, 1);
    if (res < 0) {
        pw_stream_destroy(session->clickStream);
        pw_stream_destroy(session->captureStream);
        pw_thread_loop_unlock(d->loop);
        emit engineError(QStringLiteral("Impossibile avviare lo stream di registrazione per la calibrazione"));
        return;
    }

    pw_thread_loop_unlock(d->loop);

    Impl::CalibrationSession *rawSession = session.get();
    d->calibration = std::move(session);

    // La sequenza vera e propria (link -> click -> attesa -> link ->
    // click -> attesa -> analisi) parte solo quando ENTRAMBI gli stream
    // hanno un nodeId noto (asincrono, arriva via state_changed) — poll
    // semplice ogni 50ms invece di un meccanismo dedicato: la finestra è
    // tipicamente sub-secondo e questo non è un percorso realtime.
    // Timeout di sicurezza a 5s nel caso (raro) in cui uno dei due stream
    // non venga mai schedulato.
    // Tutta la sequenza (fase 1: link+click sul sink A, attesa 1s, fase 2:
    // link+click sul sink B, attesa 1s, analisi) è scritta come una serie
    // di lambda annidate invece di metodi separati: ognuna gira solo una
    // volta, in ordine, e restare dentro questa funzione evita di dover
    // esporre Impl::CalibrationSession nell'header (Impl è solo
    // dichiarato in avanti lì) per un dettaglio puramente interno.
    auto *waitTimer = new QTimer(this);
    auto *pollCount = new int(0);
    connect(waitTimer, &QTimer::timeout, this, [this, rawSession, waitTimer, pollCount]() {
        ++(*pollCount);
        uint32_t clickId = 0, captureId = 0;
        {
            QMutexLocker locker(&d->nodesMutex);
            clickId = rawSession->clickStreamNodeId;
            captureId = rawSession->captureStreamNodeId;
        }
        if (clickId != 0 && captureId != 0) {
            waitTimer->stop();
            waitTimer->deleteLater();
            delete pollCount;

            // --- Fase 1: sink A ---
            const uint32_t linkIdA = d->createPortLink(rawSession->clickStreamNodeId, rawSession->sinkA);
            if (linkIdA == 0) {
                const uint32_t sinkA = rawSession->sinkA, sinkB = rawSession->sinkB;
                pw_thread_loop_lock(d->loop);
                pw_stream_destroy(rawSession->clickStream);
                pw_stream_destroy(rawSession->captureStream);
                pw_thread_loop_unlock(d->loop);
                d->calibration.reset();
                emit calibrationFinished(sinkA, sinkB, 0, false,
                    QStringLiteral("Impossibile collegare il click al primo output"));
                return;
            }

            // Attesa di "riscaldamento" PRIMA di sparare il click, non
            // subito dopo aver collegato: un link appena creato verso un
            // sink Bluetooth può impiegare qualche centinaio di ms prima
            // che il trasporto A2DP inizi davvero a trasmettere (lo stesso
            // motivo per cui esiste il keepalive) — un click isolato
            // inviato nell'istante stesso del collegamento rischiava di
            // essere silenziosamente perso durante quella finestra,
            // producendo un "secondo click" mai davvero suonato e quindi
            // un delta falsato (osservato in pratica: 0ms misurato tra
            // audio interno e una cassa Bluetooth, chiaramente sbagliato).
            // Applicata identica su entrambe le fasi: essendo una costante
            // aggiunta a entrambe le misure, si elide nel delta finale e
            // non ne altera il valore, dà solo tempo al trasporto di
            // stabilizzarsi prima del click vero.
            QTimer::singleShot(500, this, [this, rawSession, linkIdA]() {
                {
                    QMutexLocker locker(&d->nodesMutex);
                    rawSession->fireOffsetA = rawSession->capturedSamples.size();
                }
                rawSession->pendingClicks.fetch_add(1, std::memory_order_relaxed);

                QTimer::singleShot(1000, this, [this, rawSession, linkIdA]() {
                    unlinkNodes(linkIdA);

                    // --- Fase 2: sink B ---
                    const uint32_t linkIdB = d->createPortLink(rawSession->clickStreamNodeId, rawSession->sinkB);
                    if (linkIdB == 0) {
                        const uint32_t sinkA = rawSession->sinkA, sinkB = rawSession->sinkB;
                        pw_thread_loop_lock(d->loop);
                        pw_stream_destroy(rawSession->clickStream);
                        pw_stream_destroy(rawSession->captureStream);
                        pw_thread_loop_unlock(d->loop);
                        d->calibration.reset();
                        emit calibrationFinished(sinkA, sinkB, 0, false,
                            QStringLiteral("Impossibile collegare il click al secondo output"));
                        return;
                    }

                    QTimer::singleShot(500, this, [this, rawSession, linkIdB]() {
                        {
                            QMutexLocker locker(&d->nodesMutex);
                            rawSession->fireOffsetB = rawSession->capturedSamples.size();
                        }
                        rawSession->pendingClicks.fetch_add(1, std::memory_order_relaxed);

                        QTimer::singleShot(1000, this, [this, rawSession, linkIdB]() {
                            unlinkNodes(linkIdB);
                            d->finishCalibration(rawSession);
                            d->calibration.reset();
                        });
                    });
                });
            });
        } else if (*pollCount > 100) { // 5s
            waitTimer->stop();
            waitTimer->deleteLater();
            delete pollCount;
            const uint32_t sinkA = rawSession->sinkA;
            const uint32_t sinkB = rawSession->sinkB;
            pw_thread_loop_lock(d->loop);
            pw_stream_destroy(rawSession->clickStream);
            pw_stream_destroy(rawSession->captureStream);
            pw_thread_loop_unlock(d->loop);
            d->calibration.reset();
            emit calibrationFinished(sinkA, sinkB, 0, false,
                QStringLiteral("Impossibile avviare gli stream di calibrazione (timeout)"));
        }
    });
    waitTimer->start(50);
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
