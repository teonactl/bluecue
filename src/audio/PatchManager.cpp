#include "PatchManager.h"
#include "AudioEngine.h"
#include "bluetooth/BluetoothManager.h"
#include "models/PortModel.h"

#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTimer>

namespace {
constexpr int kMaxRecentProjects = 8;
const QString kRecentProjectsSettingsKey = QStringLiteral("recentProjects");
const QString kKeepAliveSettingsKey = QStringLiteral("keepAliveEnabled");
const QString kOutputNicknamesSettingsKey = QStringLiteral("outputNicknames");
const QString kOutputDelaysSettingsKey = QStringLiteral("outputDelaysMs");
const QString kKeepAlivePingFrequencyKey = QStringLiteral("keepAlivePingFrequencyHz");
const QString kKeepAlivePingAmplitudeKey = QStringLiteral("keepAlivePingAmplitudeUnits");
const QString kKeepAlivePingDurationKey = QStringLiteral("keepAlivePingDurationMs");
const QString kKeepAlivePingPeriodKey = QStringLiteral("keepAlivePingPeriodSeconds");
// Prefisso dei sink virtuali di cattura creati da addAppStreamCue — usato
// per riconoscere ed escludere sia loro sia il flusso di riproduzione
// interno del modulo libpipewire-module-loopback ("<nome>-in", che appare
// nel discovery come un ulteriore Kind::AppStream) dall'elenco degli stream
// applicativi selezionabili.
const QString kAppCaptureSinkPrefix = QStringLiteral("bluecue.appcapture.");
}

PatchManager::PatchManager(AudioEngine *engine, BluetoothManager *blueZ, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
    , m_blueZ(blueZ)
    , m_inputs(new PortModel(PortModel::Direction::Input, this))
    , m_outputs(new PortModel(PortModel::Direction::Output, this))
    , m_appStreamReassertTimer(new QTimer(this))
{
    m_recentProjects = QSettings().value(kRecentProjectsSettingsKey).toStringList();
    m_keepAliveSettingEnabled = QSettings().value(kKeepAliveSettingsKey, true).toBool();
    m_keepAlivePingFrequencyHz = QSettings().value(kKeepAlivePingFrequencyKey, m_keepAlivePingFrequencyHz).toInt();
    m_keepAlivePingAmplitudeUnits = QSettings().value(kKeepAlivePingAmplitudeKey, m_keepAlivePingAmplitudeUnits).toInt();
    m_keepAlivePingDurationMs = QSettings().value(kKeepAlivePingDurationKey, m_keepAlivePingDurationMs).toInt();
    m_keepAlivePingPeriodSeconds = QSettings().value(kKeepAlivePingPeriodKey, m_keepAlivePingPeriodSeconds).toInt();
    // Applicati subito al motore: senza questo, i valori salvati resterebbero
    // solo nella UI (Q_PROPERTY) finché l'utente non tocca di nuovo ogni
    // singolo campo — il generatore keepalive partirebbe invece con i
    // default hardcoded di PipeWireEngine.
    m_engine->setKeepAlivePingFrequency(m_keepAlivePingFrequencyHz);
    m_engine->setKeepAlivePingAmplitude(m_keepAlivePingAmplitudeUnits / 100000.0f);
    m_engine->setKeepAlivePingDuration(m_keepAlivePingDurationMs / 1000.0);
    m_engine->setKeepAlivePingPeriod(m_keepAlivePingPeriodSeconds);
    {
        const QVariantMap stored = QSettings().value(kOutputNicknamesSettingsKey).toMap();
        for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
            m_outputNicknames.insert(it.key(), it.value().toString());
    }
    {
        const QVariantMap stored = QSettings().value(kOutputDelaysSettingsKey).toMap();
        for (auto it = stored.constBegin(); it != stored.constEnd(); ++it)
            m_outputDelaysMs.insert(it.key(), it.value().toInt());
    }

    // BlueZManager::devicesChanged scatta dopo ogni refreshDevices() (avvio,
    // apertura del dialog "Dispositivi Bluetooth", o il pulsante "↻" in
    // colonna Output) e dopo un connect/disconnect riuscito — è il punto in
    // cui una percentuale batteria aggiornata può essere applicata ai sink
    // Output già scoperti.
    connect(m_blueZ, &BluetoothManager::devicesChanged, this, &PatchManager::refreshBatteryLevels);

    // Auto-recovery per le cue "sorgente app" (vedi addAppStreamCue): il
    // target impostato sulla metadata "default" di PipeWire è un
    // suggerimento applicato dal session manager alla prossima
    // rivalutazione del routing, non una garanzia permanente — riproporlo
    // periodicamente costa pochissimo (una sola chiamata idempotente per
    // cue attiva) ed evita di doversi fidare ciecamente di un singolo
    // tentativo iniziale.
    m_appStreamReassertTimer->setInterval(5000);
    connect(m_appStreamReassertTimer, &QTimer::timeout, this, [this]() {
        for (const Cue &cue : std::as_const(m_cues)) {
            if (cue.isAppStream && cue.appStreamNodeId != 0 && cue.captureSinkNodeId != 0
                && cue.nodeId != 0 && !cue.paused && !cue.ended) {
                m_engine->setStreamTarget(cue.appStreamNodeId, cue.captureSinkNodeId, cue.captureSinkName);
            }
        }
    });
    m_appStreamReassertTimer->start();

    // Ogni nodo scoperto da PipeWire viene smistato in base al suo Kind. I
    // sink (fisici o virtuali) vanno sempre in colonna Output — questo copre
    // sia i jack hardware sia i sink Bluetooth già connessi al momento
    // dell'avvio. I nodi Source invece NON popolano più automaticamente
    // m_inputs/la colonna Input: quella colonna ora mostra solo la playlist
    // (vedi cueModel), non ogni sorgente audio grezza scoperta da PipeWire
    // (es. il microfono integrato). L'unico uso che facciamo di un nodo
    // Source qui è riconoscere quando PipeWire assegna il nodeId a una
    // traccia appena avviata — correlato per NOME dello stream
    // (Cue::pendingStreamName), non più per un singolo indice "in attesa":
    // con più cue avviabili in contemporanea (vedi playCueAt), più tracce
    // possono avere un nodeId in sospeso nello stesso momento.
    connect(m_engine, &AudioEngine::nodeAdded, this, [this](const AudioNode &node) {
        if (node.kind == AudioNode::Kind::Source) {
            for (int i = 0; i < m_cues.size(); ++i) {
                if (!m_cues[i].pendingStreamName.isEmpty() && m_cues[i].pendingStreamName == node.name) {
                    m_cues[i].nodeId = node.id;
                    m_cues[i].pendingStreamName.clear();
                    applyDesiredConnections(i);
                    emit cuesChanged();
                    break;
                }
            }
            // Un vero microfono/line-in hardware, non uno dei nostri
            // stream interni (file, calibrazione, ecc. — sempre prefissati
            // "bluecue.") — selezionabile in calibrateOutputDelay.
            if (!node.name.startsWith(QStringLiteral("bluecue."))) {
                auto it = std::find_if(m_availableMicrophones.begin(), m_availableMicrophones.end(),
                                        [&](const AudioNode &n) { return n.id == node.id; });
                if (it != m_availableMicrophones.end())
                    *it = node;
                else
                    m_availableMicrophones.append(node);
                emit microphonesChanged();
            }
        } else if (node.kind == AudioNode::Kind::AppStream) {
            handleAppStreamNode(node);
        } else if (node.kind == AudioNode::Kind::PhysicalSink || node.kind == AudioNode::Kind::VirtualSink) {
            // Se il nome corrisponde al sink di cattura che una cue
            // "sorgente app" sta aspettando (vedi beginAppStreamCapture),
            // completane la correlazione invece di trattarlo come un
            // output normale della colonna Output — è un dettaglio interno,
            // non un dispositivo che l'utente ha collegato.
            bool matchedCapture = false;
            if (m_appCaptureSinkNames.contains(node.name)) {
                for (int i = 0; i < m_cues.size(); ++i) {
                    Cue &cue = m_cues[i];
                    if (cue.isAppStream && cue.captureSinkNodeId == 0 && cue.captureSinkName == node.name) {
                        cue.nodeId = node.id;
                        cue.captureSinkNodeId = node.id;
                        qDebug() << "PatchManager: sink di cattura correlato" << node.name
                                 << "nodeId" << node.id << "-- setStreamTarget verso appStreamNodeId"
                                 << cue.appStreamNodeId;
                        m_engine->setStreamTarget(cue.appStreamNodeId, node.id, node.name);
                        applyDesiredConnections(i);
                        emit cuesChanged();
                        matchedCapture = true;
                        break;
                    }
                }
            }
            if (!matchedCapture)
                handleSinkNode(node);
        }
    });
    connect(m_engine, &AudioEngine::nodeUpdated, this, [this](const AudioNode &node) {
        // Un sink Bluetooth arriva quasi sempre SENZA device.api ancora
        // valorizzato nell'annuncio iniziale (nodeAdded): PipeWire lo
        // aggiunge un istante dopo tramite un aggiornamento delle info del
        // nodo, che arriva qui come nodeUpdated — è per questo che
        // isBluetooth va ricontrollato anche in questo handler, non solo in
        // nodeAdded (vedi PipeWireEngine::Impl::onSinkNodeInfo).
        if (node.kind == AudioNode::Kind::Source || node.kind == AudioNode::Kind::AppStream)
            return;
        if (m_appCaptureSinkNames.contains(node.name))
            return; // uno dei nostri sink di cattura: mai in colonna Output
        handleSinkNode(node);
    });
    connect(m_engine, &AudioEngine::nodeRemoved, this, [this](uint32_t nodeId) {
        m_outputs->removeNode(nodeId);
        m_bluetoothOutputNodeIds.remove(nodeId);
        m_outputNodeNames.remove(nodeId);
        m_outputNodeDescriptions.remove(nodeId);
        m_outputNodeMacs.remove(nodeId);
        m_engine->setKeepAliveEnabled(nodeId, false); // no-op se non era un sink BT tenuto sveglio

        // Uno stream applicativo selezionabile è sparito (app chiusa, o
        // smesso di produrre audio): non più scelto nel picker.
        const auto beforeCount = m_availableAppStreams.size();
        m_availableAppStreams.erase(
            std::remove_if(m_availableAppStreams.begin(), m_availableAppStreams.end(),
                            [nodeId](const AudioNode &n) { return n.id == nodeId; }),
            m_availableAppStreams.end());
        if (m_availableAppStreams.size() != beforeCount)
            emit appStreamsChanged();

        const auto micBeforeCount = m_availableMicrophones.size();
        m_availableMicrophones.erase(
            std::remove_if(m_availableMicrophones.begin(), m_availableMicrophones.end(),
                            [nodeId](const AudioNode &n) { return n.id == nodeId; }),
            m_availableMicrophones.end());
        if (m_availableMicrophones.size() != micBeforeCount)
            emit microphonesChanged();

        // Se quello stream stava alimentando una cattura attiva, prova
        // PRIMA a "seguirlo" verso un nuovo stream dello stesso processo
        // (comune con client pipewire-pulse come Firefox, che possono
        // ricreare il proprio stream più volte durante una riproduzione
        // continua e ininterrotta dal punto di vista dell'utente — vedi
        // Cue::appProcessId) — solo se non c'è alcun sostituto già noto la
        // cattura si ferma davvero. Se un sostituto arriva più tardi
        // (asincrono, nodeAdded separato), lo raccoglie handleAppStreamNode
        // grazie ad appStreamNodeId lasciato a 0 qui sotto.
        for (int i = 0; i < m_cues.size(); ++i) {
            Cue &cue = m_cues[i];
            if (cue.isAppStream && cue.appStreamNodeId == nodeId && cue.nodeId != 0) {
                const uint32_t replacementId = findReplacementAppStreamNodeId(cue.appProcessId, nodeId);
                if (replacementId != 0) {
                    cue.appStreamNodeId = replacementId;
                    qDebug() << "PatchManager: stream" << nodeId << "sparito, agganciato subito il sostituto"
                             << replacementId << "(stesso processo" << cue.appProcessId << ")";
                    m_engine->setStreamTarget(replacementId, cue.captureSinkNodeId, cue.captureSinkName);
                } else if (cue.appProcessId != 0) {
                    cue.appStreamNodeId = 0; // in attesa: vedi handleAppStreamNode
                    qDebug() << "PatchManager: stream" << nodeId << "sparito, nessun sostituto ancora noto"
                             << "(processo" << cue.appProcessId << "), in attesa";
                } else {
                    stopCueAt(i); // nessun PID noto: impossibile seguirlo, ferma la cattura
                }
                break;
            }
        }
    });

    // Una traccia con loopCount finito si ferma da sola (silenzio) quando
    // esaurisce le ripetizioni: gestita come una fine "naturale" (rispetta
    // l'eventuale postWaitSeconds), esattamente come il timer di
    // durationSeconds in handleCueNaturalEnd. Cercata per nodeId dato che
    // più cue possono essere in riproduzione insieme.
    connect(m_engine, &AudioEngine::fileStreamFinished, this, [this](uint32_t nodeId) {
        for (int i = 0; i < m_cues.size(); ++i) {
            if (m_cues[i].nodeId == nodeId) {
                handleCueNaturalEnd(i);
                break;
            }
        }
    });

    // Esito di calibrateOutputDelay: deltaMsAtoB = latenza(B) - latenza(A).
    // Positivo => A è il più veloce e va ritardato di deltaMsAtoB (B
    // riportato a 0, ora è lui il riferimento); negativo => il contrario.
    // Un valore di ~0 (sotto la risoluzione della rilevazione, ~5ms) non
    // ha senso ritardare nulla: entrambi a 0.
    connect(m_engine, &AudioEngine::calibrationFinished, this,
            [this](uint32_t sinkA, uint32_t sinkB, int deltaMsAtoB, bool success, const QString &message) {
        m_calibrationInProgress = false;
        emit calibrationStateChanged();
        if (!success) {
            emit calibrationResult(false, message, sinkA, 0, sinkB, 0);
            return;
        }
        int delayA = 0;
        int delayB = 0;
        if (deltaMsAtoB > 0) {
            delayA = deltaMsAtoB;
        } else if (deltaMsAtoB < 0) {
            delayB = -deltaMsAtoB;
        }
        setOutputDelayMs(sinkA, delayA);
        setOutputDelayMs(sinkB, delayB);
        emit calibrationResult(true,
                                tr("Ritardo misurato: %1ms — applicato e salvato automaticamente").arg(qAbs(deltaMsAtoB)),
                                sinkA, delayA, sinkB, delayB);
    });

    // Nota: PipeWireEngine::fileStreamLooped (un giro completo del file) non
    // è più collegato alla rotazione output — richiesto esplicitamente
    // dall'utente: la rotazione ora avanza SOLO a comando (barra
    // spaziatrice, vedi advanceCue), non automaticamente ad ogni loop.
}

void PatchManager::handleSinkNode(const AudioNode &node)
{
    // node.batteryPercentage arriva sempre a -1 da PipeWireEngine (che non
    // sa nulla di batterie): se questo sink Bluetooth ha già una
    // percentuale nota da BlueZManager (vedi refreshBatteryLevels), va
    // applicata PRIMA di upsertNode così la riga in colonna Output la
    // mostra subito, anche alla primissima scoperta del nodo.
    AudioNode effectiveNode = node;
    if (!node.bluetoothMac.isEmpty()) {
        m_outputNodeMacs.insert(node.id, node.bluetoothMac);
        effectiveNode.batteryPercentage = m_batteryByMac.value(node.bluetoothMac, -1);
    }

    m_outputs->upsertNode(effectiveNode);
    m_outputNodeNames.insert(node.id, node.name);
    m_outputNodeDescriptions.insert(node.id, node.description);
    if (!node.description.isEmpty())
        m_lastKnownOutputDescriptionByName.insert(node.name, node.description);
    // Se questo sink ha già un nickname salvato da una sessione precedente
    // (per nome stabile, quindi sopravvive a riavvii/riconnessioni), va
    // applicato subito alla riga in colonna Output — altrimenti resterebbe
    // visibile solo dopo il prossimo giro di setOutputNickname.
    const QString nickname = m_outputNicknames.value(node.name);
    if (!nickname.isEmpty())
        m_outputs->updateDescription(node.id, nickname);
    // Stesso principio del nickname: se questo sink ha già un ritardo
    // salvato da una sessione precedente (per nome stabile), va applicato
    // subito sia al motore (altrimenti il routing partirebbe senza
    // ritardo finché l'utente non tocca di nuovo il controllo) sia alla
    // riga in colonna Output.
    const int savedDelayMs = m_outputDelaysMs.value(node.name, 0);
    if (savedDelayMs > 0) {
        m_engine->setOutputDelayMs(node.id, savedDelayMs);
        m_outputs->updateDelayMs(node.id, savedDelayMs);
    }
    // Le casse Bluetooth in colonna Output ricevono un ping periodico per
    // non andare in stand-by per inattività (vedi
    // PipeWireEngine::setKeepAliveEnabled), a meno che l'utente non l'abbia
    // disattivato dal menu Impostazioni (keepAliveEnabled). isBluetooth può
    // diventare true solo più tardi (via nodeUpdated) rispetto al primo
    // nodeAdded, quindi questo va chiamato da entrambi gli handler — vedi
    // il commento sul segnale nodeUpdated nel costruttore.
    if (node.isBluetooth && !m_bluetoothOutputNodeIds.contains(node.id)) {
        m_bluetoothOutputNodeIds.insert(node.id);
        if (m_keepAliveSettingEnabled)
            m_engine->setKeepAliveEnabled(node.id, true);
    }
}

void PatchManager::handleAppStreamNode(const AudioNode &node)
{
    if (node.name.startsWith(kAppCaptureSinkPrefix))
        return; // il nostro stesso "<nome>-in" (vedi PipeWireEngine::createVirtualSink): non è una sorgente da poter scegliere

    // Se una cue "sorgente app" attiva stava aspettando un sostituto dallo
    // stesso processo (il suo stream precedente è sparito senza che
    // l'utente fermasse nulla — vedi il commento sul nodeRemoved qui
    // sotto, comune con client pipewire-pulse come Firefox), riagganciala
    // subito a questo nuovo stream invece di lasciarla silenziosa.
    // appStreamNodeId == 0 distingue "in attesa di un sostituto" da una cue
    // già agganciata a un nodeId vivo (che non va toccata qui).
    if (node.appProcessId != 0) {
        for (int i = 0; i < m_cues.size(); ++i) {
            Cue &cue = m_cues[i];
            if (cue.isAppStream && cue.nodeId != 0 && cue.appStreamNodeId == 0
                && cue.appProcessId == node.appProcessId) {
                cue.appStreamNodeId = node.id;
                qDebug() << "PatchManager: cue" << i << "riagganciata al nuovo stream" << node.name
                         << "nodeId" << node.id << "dello stesso processo" << node.appProcessId;
                m_engine->setStreamTarget(node.id, cue.captureSinkNodeId, cue.captureSinkName);
                emit cuesChanged();
            }
        }
    }

    auto it = std::find_if(m_availableAppStreams.begin(), m_availableAppStreams.end(),
                            [&](const AudioNode &n) { return n.id == node.id; });
    if (it != m_availableAppStreams.end())
        *it = node;
    else
        m_availableAppStreams.append(node);
    emit appStreamsChanged();
}

uint32_t PatchManager::findReplacementAppStreamNodeId(uint32_t appProcessId, uint32_t excludeNodeId) const
{
    if (appProcessId == 0)
        return 0;
    for (const AudioNode &n : m_availableAppStreams) {
        if (n.appProcessId == appProcessId && n.id != excludeNodeId)
            return n.id;
    }
    return 0;
}

QVariantList PatchManager::appStreamsModel() const
{
    QVariantList result;
    result.reserve(m_availableAppStreams.size());
    for (const AudioNode &n : m_availableAppStreams) {
        QVariantMap entry;
        entry[QStringLiteral("nodeId")] = n.id;
        entry[QStringLiteral("description")] = n.description.isEmpty() ? n.name : n.description;
        result.append(entry);
    }
    return result;
}

QVariantList PatchManager::microphonesModel() const
{
    QVariantList result;
    result.reserve(m_availableMicrophones.size());
    for (const AudioNode &n : m_availableMicrophones) {
        QVariantMap entry;
        entry[QStringLiteral("nodeId")] = n.id;
        entry[QStringLiteral("description")] = n.description.isEmpty() ? n.name : n.description;
        result.append(entry);
    }
    return result;
}

void PatchManager::calibrateOutputDelay(uint32_t nodeIdA, uint32_t nodeIdB, uint32_t micNodeId)
{
    if (m_calibrationInProgress) {
        emit patchError(tr("Una calibrazione è già in corso"));
        return;
    }
    m_calibrationInProgress = true;
    emit calibrationStateChanged();
    m_engine->calibrateOutputDelay(nodeIdA, nodeIdB, micNodeId);
}

void PatchManager::refreshBatteryLevels()
{
    m_batteryByMac.clear();
    for (const BluetoothDevice &dev : m_blueZ->devices()) {
        if (dev.batteryPercentage >= 0)
            m_batteryByMac.insert(dev.address, dev.batteryPercentage);
    }

    for (auto it = m_outputNodeMacs.constBegin(); it != m_outputNodeMacs.constEnd(); ++it)
        m_outputs->updateBatteryPercentage(it.key(), m_batteryByMac.value(it.value(), -1));
}

void PatchManager::addCueFile(const QUrl &fileUrl)
{
    // FileDialog.selectedFile arriva come QUrl (es. "file:///home/..."); va
    // convertito in path locale prima di usarlo con QFileInfo, altrimenti il
    // controllo di esistenza fallisce sempre confrontando lo schema "file://"
    // come se fosse parte del path.
    const QString filePath = fileUrl.toLocalFile();
    const QFileInfo info(filePath);
    if (!info.exists()) {
        emit patchError(tr("File non trovato: %1").arg(filePath));
        return;
    }

    pushUndoSnapshot();

    Cue cue;
    cue.id = m_nextCueId++;
    cue.filePath = filePath;
    cue.displayName = info.fileName();
    m_cues.append(cue);

    if (m_armedIndex < 0)
        m_armedIndex = m_cues.size() - 1; // coda vuota: arma subito questa traccia

    emit cuesChanged();
}

void PatchManager::addAppStreamCue(uint32_t appStreamNodeId)
{
    const auto it = std::find_if(m_availableAppStreams.cbegin(), m_availableAppStreams.cend(),
                                  [&](const AudioNode &n) { return n.id == appStreamNodeId; });
    if (it == m_availableAppStreams.cend()) {
        emit patchError(tr("Sorgente app non più disponibile"));
        return;
    }

    pushUndoSnapshot();

    Cue cue;
    cue.id = m_nextCueId++;
    cue.isAppStream = true;
    cue.appStreamNodeId = appStreamNodeId;
    cue.appProcessId = it->appProcessId;
    cue.appStreamMatchName = it->name;
    cue.displayName = it->description.isEmpty() ? it->name : it->description;
    m_cues.append(cue);

    if (m_armedIndex < 0)
        m_armedIndex = m_cues.size() - 1;

    emit cuesChanged();
}

void PatchManager::removeCue(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;

    pushUndoSnapshot();

    stopCueAt(index); // no-op se non era in riproduzione né in attesa di partire

    m_cues.remove(index);

    if (m_armedIndex == index) {
        if (m_armedIndex >= m_cues.size())
            m_armedIndex = m_cues.isEmpty() ? -1 : m_cues.size() - 1;
    } else if (m_armedIndex > index) {
        --m_armedIndex;
    }

    emit cuesChanged();
}

void PatchManager::moveCue(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_cues.size() || toIndex < 0 || toIndex >= m_cues.size()
        || fromIndex == toIndex)
        return;

    pushUndoSnapshot();

    const quint64 armedId = (m_armedIndex >= 0 && m_armedIndex < m_cues.size())
        ? m_cues[m_armedIndex].id : 0;

    m_cues.move(fromIndex, toIndex);

    // m_armedIndex segue la STESSA cue per id, non per posizione — senza
    // questo, spostare una traccia diversa da quella armata cambierebbe
    // quale traccia parte con la barra spaziatrice, sorpresa non voluta da
    // un semplice riordino visivo.
    if (armedId != 0)
        m_armedIndex = findCueIndexById(armedId);

    emit cuesChanged();
}

void PatchManager::setCueDisplayName(int cueIndex, const QString &name)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || m_cues[cueIndex].displayName == trimmed)
        return;

    pushUndoSnapshot();
    m_cues[cueIndex].displayName = trimmed;
    emit cuesChanged();
}

void PatchManager::stopCueAt(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;

    Cue &cue = m_cues[index];
    if (cue.nodeId == 0 && !cue.waitingToStart)
        return; // niente in corso né in attesa di partire

    if (cue.nodeId != 0) {
        for (auto it = m_connections.begin(); it != m_connections.end();) {
            if (it->inputNodeId == cue.nodeId) {
                m_engine->unlinkNodes(it->linkId);
                it = m_connections.erase(it);
            } else {
                ++it;
            }
        }
        if (cue.isAppStream) {
            // appStreamNodeId può essere 0 qui: la cue era "in attesa" di
            // un sostituto dallo stesso processo (vedi il commento su
            // Cue::appProcessId/nodeRemoved) quando l'utente l'ha fermata a
            // mano. Il subject 0 della metadata PipeWire è quello globale
            // ("default.audio.sink" ecc.): NON va mai usato come se fosse
            // "nessuno stream", quindi va saltato esplicitamente qui.
            if (cue.appStreamNodeId != 0)
                m_engine->clearStreamTarget(cue.appStreamNodeId);
            m_engine->removeVirtualSink(cue.captureSinkNodeId);
            m_appCaptureSinkNames.remove(cue.captureSinkName);
        } else {
            m_engine->removeFileStream(cue.nodeId);
        }
    }

    cue.nodeId = 0;
    cue.captureSinkNodeId = 0;
    cue.captureSinkName.clear();
    cue.rotateOutputIndex = 0;
    cue.waitingToStart = false;
    cue.inPostWait = false;
    cue.ended = false;
    cue.paused = false;
    cue.pendingStreamName.clear();
    emit connectionsChanged();
    emit cuesChanged();
}

void PatchManager::setCueLiveActive(Cue &cue, bool active)
{
    if (cue.isAppStream) {
        // Non esiste un vero "pausa" per uno stream che non controlliamo
        // noi: l'equivalente più fedele è smettere di alimentare il sink
        // di cattura (silenzio reale, il monitor non riceve più nulla) o
        // riprendere a farlo, senza toccare il sink/i collegamenti, che
        // restano vivi esattamente come setFileStreamActive fa per una
        // cue file.
        // appStreamNodeId può essere 0 se la cue è "in attesa" di un
        // sostituto dallo stesso processo (vedi Cue::appProcessId) — in
        // quel caso non c'è nessuno stream su cui agire, il subject 0 della
        // metadata PipeWire è quello globale, non va mai usato qui.
        if (cue.appStreamNodeId != 0) {
            if (active)
                m_engine->setStreamTarget(cue.appStreamNodeId, cue.captureSinkNodeId, cue.captureSinkName);
            else
                m_engine->clearStreamTarget(cue.appStreamNodeId);
        }
    } else {
        m_engine->setFileStreamActive(cue.nodeId, active);
    }
}

void PatchManager::advanceCue()
{
    // Se una traccia in riproduzione ha la rotazione output attiva con più
    // di un output collegato, la barra spaziatrice avanza PRIMA la sua
    // rotazione (etichette sotto la riga in CueList.qml) invece di avviare
    // la traccia successiva — la rotazione "occupa" la barra spaziatrice
    // finché c'è una traccia così in riproduzione. Con più tracce del
    // genere in contemporanea si avanza quella trovata per prima (ordine di
    // playlist), caso raro non approfondito oltre.
    for (int i = 0; i < m_cues.size(); ++i) {
        if (m_cues[i].nodeId != 0 && m_cues[i].rotateOutputs && m_cues[i].desiredOutputNames.size() > 1) {
            advanceOutputRotation(i);
            return;
        }
    }

    playArmedCue();
}

void PatchManager::playArmedCue()
{
    if (m_armedIndex < 0 || m_armedIndex >= m_cues.size()) {
        emit cuesChanged();
        return; // playlist esaurita: niente da avviare, ma non tocca le tracce già in corso
    }

    playCueAt(m_armedIndex);
}

void PatchManager::armCue(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;
    m_armedIndex = index;
    emit cuesChanged();
}

int PatchManager::findCueIndexById(quint64 id) const
{
    for (int i = 0; i < m_cues.size(); ++i) {
        if (m_cues[i].id == id)
            return i;
    }
    return -1;
}

QVector<PatchManager::CueSnapshotEntry> PatchManager::snapshotCues() const
{
    QVector<CueSnapshotEntry> result;
    result.reserve(m_cues.size());
    for (const Cue &c : m_cues) {
        CueSnapshotEntry entry;
        entry.id = c.id;
        entry.filePath = c.filePath;
        entry.displayName = c.displayName;
        entry.desiredOutputNames = c.desiredOutputNames;
        entry.loopCount = c.loopCount;
        entry.reverse = c.reverse;
        entry.rotateOutputs = c.rotateOutputs;
        entry.rotationCycleCount = c.rotationCycleCount;
        entry.preWaitSeconds = c.preWaitSeconds;
        entry.durationSeconds = c.durationSeconds;
        entry.postWaitSeconds = c.postWaitSeconds;
        entry.isAppStream = c.isAppStream;
        entry.appStreamNodeId = c.appStreamNodeId;
        entry.appStreamMatchName = c.appStreamMatchName;
        result.append(entry);
    }
    return result;
}

void PatchManager::restoreCueSnapshot(const QVector<CueSnapshotEntry> &snapshot)
{
    // Ferma (senza rimuoverle da m_cues ancora: serve l'indice originale)
    // le cue live che lo snapshot non contiene più — sparite per un
    // "annulla" di una addCueFile precedente, o per un "ripeti" di una
    // removeCue. Nessuno stream PipeWire può essere "resuscitato" al
    // contrario, quindi la riproduzione va fermata comunque.
    for (int i = 0; i < m_cues.size(); ++i) {
        const quint64 id = m_cues[i].id;
        const bool stillExists = std::any_of(snapshot.cbegin(), snapshot.cend(),
            [&](const CueSnapshotEntry &e) { return e.id == id; });
        if (!stillExists)
            stopCueAt(i);
    }

    QVector<Cue> newCues;
    newCues.reserve(snapshot.size());
    for (const CueSnapshotEntry &entry : snapshot) {
        const int existingIdx = findCueIndexById(entry.id);
        // Preserva lo stato di riproduzione live (nodeId, pausa, timer di
        // pre/post wait...) per le cue ancora presenti — solo i campi
        // "di progetto" vengono sovrascritti dallo snapshot. Per una cue
        // ricreata da zero (rimossa dopo lo snapshot, ora "ripetuta" in
        // vita) tutto il resto resta ai valori di default di Cue.
        Cue cue = (existingIdx >= 0) ? m_cues[existingIdx] : Cue{};
        cue.id = entry.id;
        cue.filePath = entry.filePath;
        cue.displayName = entry.displayName;
        cue.desiredOutputNames = entry.desiredOutputNames;
        cue.loopCount = entry.loopCount;
        cue.reverse = entry.reverse;
        cue.rotateOutputs = entry.rotateOutputs;
        cue.rotationCycleCount = entry.rotationCycleCount;
        cue.preWaitSeconds = entry.preWaitSeconds;
        cue.durationSeconds = entry.durationSeconds;
        cue.postWaitSeconds = entry.postWaitSeconds;
        // isAppStream/appStreamNodeId sono già preservati dalla copia
        // iniziale (m_cues[existingIdx]) per una cue ancora presente; per
        // una resuscitata da zero (rimossa dopo lo snapshot, "ripetuta" ora)
        // vanno invece ripristinati dallo snapshot come tutto il resto.
        if (existingIdx < 0) {
            cue.isAppStream = entry.isAppStream;
            cue.appStreamNodeId = entry.appStreamNodeId;
            cue.appStreamMatchName = entry.appStreamMatchName;
        }
        newCues.append(cue);
    }

    const quint64 armedId = (m_armedIndex >= 0 && m_armedIndex < m_cues.size())
        ? m_cues[m_armedIndex].id : 0;

    m_cues = newCues;

    // Riapplica il routing voluto per ogni cue ancora live: lo snapshot
    // può aver cambiato desiredOutputNames.
    for (int i = 0; i < m_cues.size(); ++i) {
        if (m_cues[i].nodeId > 0)
            applyDesiredConnections(i);
    }

    m_armedIndex = findCueIndexById(armedId);
    if (m_armedIndex < 0 && !m_cues.isEmpty())
        m_armedIndex = 0;

    emit connectionsChanged();
    emit cuesChanged();
}

void PatchManager::pushUndoSnapshot()
{
    m_undoStack.append(snapshotCues());
    if (m_undoStack.size() > kMaxUndoDepth)
        m_undoStack.removeFirst();
    m_redoStack.clear();
    emit undoRedoAvailabilityChanged();
}

void PatchManager::undo()
{
    if (m_undoStack.isEmpty())
        return;
    m_redoStack.append(snapshotCues());
    const QVector<CueSnapshotEntry> snapshot = m_undoStack.takeLast();
    restoreCueSnapshot(snapshot);
    emit undoRedoAvailabilityChanged();
}

void PatchManager::redo()
{
    if (m_redoStack.isEmpty())
        return;
    m_undoStack.append(snapshotCues());
    const QVector<CueSnapshotEntry> snapshot = m_redoStack.takeLast();
    restoreCueSnapshot(snapshot);
    emit undoRedoAvailabilityChanged();
}

void PatchManager::playCueAt(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;

    Cue &cue = m_cues[index];
    if (cue.nodeId != 0 || cue.waitingToStart)
        return; // già in riproduzione o già in attesa di partire: niente doppio trigger

    // NON ferma nessun'altra traccia: più cue possono restare in
    // riproduzione insieme, esattamente come chiesto — avviarne una nuova
    // non deve interrompere quelle già partite.
    ++cue.playbackGeneration; // invalida eventuali timer di un avvio precedente della stessa cue
    const quint64 generation = cue.playbackGeneration;
    const quint64 cueId = cue.id;

    m_armedIndex = (index + 1 < m_cues.size()) ? index + 1 : -1;

    if (cue.preWaitSeconds > 0.0) {
        cue.waitingToStart = true;
        emit cuesChanged();
        QTimer::singleShot(static_cast<int>(cue.preWaitSeconds * 1000.0), this, [this, cueId, generation]() {
            const int idx = findCueIndexById(cueId);
            if (idx >= 0 && m_cues[idx].playbackGeneration == generation && m_cues[idx].waitingToStart)
                startCueNow(idx);
        });
        return;
    }

    startCueNow(index);
}

void PatchManager::startCueNow(int index)
{
    if (index < 0 || index >= m_cues.size())
        return; // la cue è stata rimossa (o già fermata) nel frattempo

    Cue &cue = m_cues[index];
    cue.waitingToStart = false;
    cue.inPostWait = false;
    cue.ended = false;
    cue.rotateOutputIndex = 0;
    cue.rotationCyclesCompleted = 0;
    cue.paused = false;
    const quint64 generation = cue.playbackGeneration; // impostato da playCueAt al momento del trigger

    if (cue.isAppStream) {
        // Una cue ricaricata da un file di progetto (o che aveva perso il
        // suo stream senza trovare un sostituto — vedi Cue::appProcessId)
        // arriva qui con appStreamNodeId a 0: ri-risolvilo ORA cercando tra
        // gli stream applicativi attualmente noti quello con lo stesso
        // nome tecnico stabile. Ambiguo se più stream con lo stesso nome
        // sono attivi insieme (es. due finestre Firefox): prende il primo,
        // limite accettato.
        if (cue.appStreamNodeId == 0) {
            uint32_t resolvedNodeId = 0;
            uint32_t resolvedProcessId = 0;
            for (const AudioNode &n : m_availableAppStreams) {
                if (n.name == cue.appStreamMatchName) {
                    resolvedNodeId = n.id;
                    resolvedProcessId = n.appProcessId;
                    break;
                }
            }
            if (resolvedNodeId == 0) {
                emit patchError(tr("Sorgente app '%1' non disponibile al momento").arg(cue.appStreamMatchName));
                emit cuesChanged();
                return;
            }
            cue.appStreamNodeId = resolvedNodeId;
            cue.appProcessId = resolvedProcessId;
        }
        beginAppStreamCapture(index);
    } else {
        const QString streamName = m_engine->createFileStream(cue.filePath, cue.displayName,
                                                                cue.loopCount, cue.reverse);
        if (streamName.isEmpty()) {
            emit patchError(tr("Impossibile riprodurre: %1").arg(cue.displayName));
            emit cuesChanged();
            return;
        }
        cue.pendingStreamName = streamName;
    }

    if (cue.durationSeconds > 0.0) {
        const quint64 cueId = cue.id;
        QTimer::singleShot(static_cast<int>(cue.durationSeconds * 1000.0), this, [this, cueId, generation]() {
            const int idx = findCueIndexById(cueId);
            if (idx >= 0 && m_cues[idx].playbackGeneration == generation)
                handleCueNaturalEnd(idx);
        });
    }

    // Il nodeId della traccia arriva in modo asincrono tramite nodeAdded
    // (vedi costruttore), correlato per nome dello stream
    // (Cue::pendingStreamName): aggiorna m_cues[index].nodeId, applica il
    // routing voluto (applyDesiredConnections) e riemette cuesChanged() da
    // solo.
    emit cuesChanged();
}

void PatchManager::beginAppStreamCapture(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;

    Cue &cue = m_cues[index];
    const QString captureSinkName = kAppCaptureSinkPrefix + QString::number(cue.appStreamNodeId);
    cue.captureSinkName = captureSinkName;
    m_appCaptureSinkNames.insert(captureSinkName);

    // Il nodeId del sink virtuale (e il conseguente setStreamTarget che
    // "sposta" davvero l'audio) arriva in modo asincrono tramite nodeAdded,
    // correlato per nome — vedi il ramo Kind::PhysicalSink/VirtualSink nel
    // costruttore.
    qDebug() << "PatchManager: beginAppStreamCapture cueIndex" << index
             << "appStreamNodeId" << cue.appStreamNodeId << "captureSinkName" << captureSinkName;
    m_engine->createVirtualSink(captureSinkName, tr("Cattura: %1").arg(cue.displayName));
}

void PatchManager::handleCueNaturalEnd(int index)
{
    if (index < 0 || index >= m_cues.size())
        return;

    Cue &cue = m_cues[index];
    if (cue.nodeId == 0 || cue.ended)
        return; // già fermata a mano, o già gestita (durata e loop scaduti insieme)
    cue.ended = true;

    setCueLiveActive(cue, false); // silenzio, nodo/collegamenti vivi

    if (cue.postWaitSeconds > 0.0) {
        cue.inPostWait = true;
        const quint64 cueId = cue.id;
        const quint64 generation = cue.playbackGeneration;
        emit cuesChanged();
        QTimer::singleShot(static_cast<int>(cue.postWaitSeconds * 1000.0), this, [this, cueId, generation]() {
            const int idx = findCueIndexById(cueId);
            if (idx >= 0 && m_cues[idx].playbackGeneration == generation)
                stopCueAt(idx);
        });
        return;
    }

    stopCueAt(index);
}

void PatchManager::play()
{
    bool resumedAny = false;
    for (Cue &cue : m_cues) {
        if (cue.nodeId != 0 && cue.paused && !cue.ended) {
            setCueLiveActive(cue, true);
            cue.paused = false;
            resumedAny = true;
        }
    }
    if (resumedAny) {
        emit cuesChanged();
        return;
    }
    // Nessuna traccia in pausa da riprendere: avvia quella in armo, SENZA
    // fermare le altre già in riproduzione.
    advanceCue();
}

void PatchManager::pause()
{
    bool pausedAny = false;
    for (Cue &cue : m_cues) {
        if (cue.nodeId != 0 && !cue.paused && !cue.ended && !cue.waitingToStart) {
            setCueLiveActive(cue, false);
            cue.paused = true;
            pausedAny = true;
        }
    }
    if (pausedAny)
        emit cuesChanged();
}

void PatchManager::toggleCueOutput(int cueIndex, uint32_t outputNodeId)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    const QString outputName = m_outputNodeNames.value(outputNodeId);
    if (outputName.isEmpty()) {
        emit patchError(tr("Output sconosciuto"));
        return;
    }

    pushUndoSnapshot();
    Cue &cue = m_cues[cueIndex];

    const int existingIndex = cue.desiredOutputNames.indexOf(outputName);
    if (existingIndex >= 0) {
        cue.desiredOutputNames.removeAt(existingIndex);
        if (cue.rotateOutputIndex > existingIndex)
            --cue.rotateOutputIndex;
    } else {
        cue.desiredOutputNames.append(outputName);
    }
    if (cue.desiredOutputNames.isEmpty() || cue.rotateOutputIndex >= cue.desiredOutputNames.size())
        cue.rotateOutputIndex = 0;

    // Se la traccia è già in riproduzione, riflette subito la modifica sul
    // collegamento PipeWire reale. Se non lo è, resta solo "voluta" e verrà
    // creata in automatico non appena la traccia parte (playCueAt ->
    // applyDesiredConnections). In modalità rotazione il routing live segue
    // SOLO l'output attivo corrente, mai l'intero insieme: risincronizza da
    // zero invece di alternare la singola connessione appena toccata,
    // altrimenti un output "extra" potrebbe restare collegato insieme a
    // quello attivo.
    if (cue.nodeId > 0) {
        if (cue.rotateOutputs)
            resyncRotationConnection(cueIndex);
        else
            toggleConnection(cue.nodeId, outputNodeId);
    }

    // cuesChanged() (non solo connectionsChanged(), già emesso sopra se la
    // traccia era live) serve perché il routing voluto è ora parte di
    // cueModel() (desiredOutputNodeIds) ed è quello che la UI disegna come
    // cavo — anche per una traccia NON in riproduzione, che altrimenti non
    // riceverebbe mai un aggiornamento.
    emit cuesChanged();
}

void PatchManager::removeCueDesiredOutputByName(int cueIndex, const QString &outputName)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    const int existingIndex = m_cues[cueIndex].desiredOutputNames.indexOf(outputName);
    if (existingIndex < 0)
        return;

    pushUndoSnapshot();
    Cue &cue = m_cues[cueIndex];

    cue.desiredOutputNames.removeAt(existingIndex);
    if (cue.rotateOutputIndex > existingIndex)
        --cue.rotateOutputIndex;
    if (cue.desiredOutputNames.isEmpty() || cue.rotateOutputIndex >= cue.desiredOutputNames.size())
        cue.rotateOutputIndex = 0;

    // Se l'output rimosso risulta comunque live in questo momento (nome
    // ancora presente in m_outputNodeNames) e la traccia è in riproduzione,
    // scollega anche il link PipeWire reale — stesso ramo di toggleCueOutput.
    const uint32_t liveNodeId = m_outputNodeNames.key(outputName, 0);
    if (cue.nodeId > 0 && liveNodeId != 0) {
        if (cue.rotateOutputs)
            resyncRotationConnection(cueIndex);
        else if (isConnected(cue.nodeId, liveNodeId))
            toggleConnection(cue.nodeId, liveNodeId);
    }

    emit cuesChanged();
}

void PatchManager::setCueLoopCount(int cueIndex, int loopCount)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    Cue &cue = m_cues[cueIndex];
    cue.loopCount = loopCount;
    if (cue.nodeId > 0)
        m_engine->setFileStreamLoopCount(cue.nodeId, loopCount);
    emit cuesChanged();
}

void PatchManager::setCueReverse(int cueIndex, bool reverse)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    Cue &cue = m_cues[cueIndex];
    cue.reverse = reverse;
    if (cue.nodeId > 0)
        m_engine->setFileStreamReverse(cue.nodeId, reverse);
    emit cuesChanged();
}

void PatchManager::setCueRotateOutputs(int cueIndex, bool rotate)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    Cue &cue = m_cues[cueIndex];
    if (cue.rotateOutputs == rotate)
        return;

    cue.rotateOutputs = rotate;
    cue.rotateOutputIndex = 0;
    if (cue.nodeId > 0) {
        if (rotate)
            resyncRotationConnection(cueIndex);
        else
            applyDesiredConnections(cueIndex); // ricollega tutti gli output voluti insieme
    }
    emit cuesChanged();
}

void PatchManager::setCueRotationCycleCount(int cueIndex, int cycleCount)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;
    m_cues[cueIndex].rotationCycleCount = cycleCount;
    emit cuesChanged();
}

void PatchManager::setCuePreWait(int cueIndex, double seconds)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;
    m_cues[cueIndex].preWaitSeconds = std::max(0.0, seconds);
    emit cuesChanged();
}

void PatchManager::setCueDuration(int cueIndex, double seconds)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    Cue &cue = m_cues[cueIndex];
    cue.durationSeconds = std::max(0.0, seconds);

    // Se già in riproduzione, il nuovo limite riparte da ADESSO (non
    // dall'inizio originale della traccia): invalida il vecchio timer (se
    // c'era) con una nuova generazione, poi ne pianifica uno nuovo se il
    // limite è ancora positivo.
    if (cue.nodeId > 0 && !cue.ended) {
        ++cue.playbackGeneration;
        const quint64 generation = cue.playbackGeneration;
        const quint64 cueId = cue.id;
        if (cue.durationSeconds > 0.0) {
            QTimer::singleShot(static_cast<int>(cue.durationSeconds * 1000.0), this, [this, cueId, generation]() {
                const int idx = findCueIndexById(cueId);
                if (idx >= 0 && m_cues[idx].playbackGeneration == generation)
                    handleCueNaturalEnd(idx);
            });
        }
    }
    emit cuesChanged();
}

void PatchManager::setCuePostWait(int cueIndex, double seconds)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;
    m_cues[cueIndex].postWaitSeconds = std::max(0.0, seconds);
    emit cuesChanged();
}

void PatchManager::applyDesiredConnections(int cueIndex)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    const Cue &cue = m_cues[cueIndex];
    if (cue.nodeId == 0 || cue.desiredOutputNames.isEmpty())
        return;

    if (cue.rotateOutputs) {
        resyncRotationConnection(cueIndex);
        return;
    }

    for (auto it = m_outputNodeNames.constBegin(); it != m_outputNodeNames.constEnd(); ++it) {
        if (cue.desiredOutputNames.contains(it.value()) && !isConnected(cue.nodeId, it.key()))
            toggleConnection(cue.nodeId, it.key());
    }
}

void PatchManager::advanceOutputRotation(int cueIndex)
{
    if (cueIndex < 0 || cueIndex >= m_cues.size())
        return;

    Cue &cue = m_cues[cueIndex];
    if (cue.nodeId == 0 || cue.desiredOutputNames.size() < 2)
        return; // niente tra cui ruotare

    cue.rotateOutputIndex = (cue.rotateOutputIndex + 1) % cue.desiredOutputNames.size();
    resyncRotationConnection(cueIndex);

    // Un giro completo si chiude quando l'indice torna a 0 (ripartito
    // dall'ultimo output della sequenza): se è stato impostato un limite di
    // cicli (Cue::rotationCycleCount, richiesto dall'utente per passare da
    // sola alla prossima traccia invece di dover ruotare all'infinito a
    // mano), questo è il punto in cui verificarlo.
    if (cue.rotateOutputIndex != 0)
        return;

    ++cue.rotationCyclesCompleted;
    if (cue.rotationCycleCount > 0 && cue.rotationCyclesCompleted >= cue.rotationCycleCount) {
        handleCueNaturalEnd(cueIndex); // rispetta un eventuale postWaitSeconds, come durata/loopCount
        playArmedCue(); // "passa alla prossima traccia", richiesto esplicitamente dall'utente
    }
}

void PatchManager::resyncRotationConnection(int cueIndex)
{
    Cue &cue = m_cues[cueIndex];
    if (cue.nodeId == 0)
        return;

    const QString activeName = cue.desiredOutputNames.isEmpty()
        ? QString()
        : cue.desiredOutputNames.at(cue.rotateOutputIndex % cue.desiredOutputNames.size());

    uint32_t activeNodeId = 0;
    for (auto it = m_outputNodeNames.constBegin(); it != m_outputNodeNames.constEnd(); ++it) {
        const bool isActive = !activeName.isEmpty() && it.value() == activeName;
        if (isActive)
            activeNodeId = it.key();
        else if (cue.desiredOutputNames.contains(it.value()) && isConnected(cue.nodeId, it.key()))
            toggleConnection(cue.nodeId, it.key()); // scollega gli output non attivi

    }
    if (activeNodeId != 0 && !isConnected(cue.nodeId, activeNodeId))
        toggleConnection(cue.nodeId, activeNodeId); // collega quello attivo

    emit cuesChanged();
}

void PatchManager::addMicrophoneInput(uint32_t hardwareSourceId)
{
    // TODO(prossimo step): il nodo hardware esiste già in PipeWire (rilevato
    // dal discovery in PipeWireEngine). Se in futuro serve un "capture
    // stream" dedicato (per applicare processing prima del routing) va
    // creato qui.
    Q_UNUSED(hardwareSourceId);
}

void PatchManager::newProject()
{
    for (int i = 0; i < m_cues.size(); ++i)
        stopCueAt(i);

    m_cues.clear();
    m_armedIndex = -1;
    emit cuesChanged();

    if (!m_currentProjectPath.isEmpty()) {
        m_currentProjectPath.clear();
        emit currentProjectPathChanged();
    }
}

bool PatchManager::saveProject(const QUrl &fileUrl)
{
    const QString filePath = fileUrl.toLocalFile();
    if (filePath.isEmpty()) {
        emit patchError(tr("Percorso di salvataggio non valido"));
        return false;
    }
    if (!writeProjectToPath(filePath))
        return false;

    if (m_currentProjectPath != filePath) {
        m_currentProjectPath = filePath;
        emit currentProjectPathChanged();
    }
    return true;
}

bool PatchManager::saveProjectToCurrentPath()
{
    if (m_currentProjectPath.isEmpty())
        return false; // progetto nuovo mai salvato: la UI deve aprire "Salva con nome"
    return writeProjectToPath(m_currentProjectPath);
}

bool PatchManager::writeProjectToPath(const QString &filePath)
{
    QJsonArray cuesArray;
    for (const Cue &c : m_cues) {
        QJsonObject cueObj;
        // Una cue "sorgente app" (Firefox, ecc.) non ha un filePath: al
        // caricamento non c'è nessuno stream live da riprendere, solo il
        // nome tecnico stabile (appStreamMatchName) da ri-risolvere al
        // momento del Play, se e quando l'app sarà di nuovo in esecuzione
        // — vedi PatchManager::startCueNow.
        cueObj[QStringLiteral("isAppStream")] = c.isAppStream;
        cueObj[QStringLiteral("appStreamMatchName")] = c.appStreamMatchName;
        cueObj[QStringLiteral("filePath")] = c.filePath;
        // Il nome può essere stato rinominato dall'utente (doppio click in
        // CueList.qml) e non corrispondere più al nome del file — va
        // salvato esplicitamente, non ricalcolato da QFileInfo al
        // caricamento come accadeva prima.
        cueObj[QStringLiteral("displayName")] = c.displayName;
        QJsonArray outputsArray;
        for (const QString &name : c.desiredOutputNames)
            outputsArray.append(name);
        cueObj[QStringLiteral("outputs")] = outputsArray;
        cueObj[QStringLiteral("loopCount")] = c.loopCount;
        cueObj[QStringLiteral("reverse")] = c.reverse;
        cueObj[QStringLiteral("rotateOutputs")] = c.rotateOutputs;
        cueObj[QStringLiteral("rotationCycleCount")] = c.rotationCycleCount;
        cueObj[QStringLiteral("preWaitSeconds")] = c.preWaitSeconds;
        cueObj[QStringLiteral("durationSeconds")] = c.durationSeconds;
        cueObj[QStringLiteral("postWaitSeconds")] = c.postWaitSeconds;
        cuesArray.append(cueObj);
    }

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("cues")] = cuesArray;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit patchError(tr("Impossibile scrivere il progetto: %1").arg(filePath));
        return false;
    }
    file.write(QJsonDocument(root).toJson());
    file.close();

    addToRecentProjects(filePath);
    return true;
}

bool PatchManager::loadProject(const QUrl &fileUrl)
{
    return loadProjectFromPath(fileUrl.toLocalFile());
}

bool PatchManager::loadProjectFromPath(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        emit patchError(tr("Impossibile aprire il progetto: %1").arg(filePath));
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        emit patchError(tr("File di progetto non valido: %1").arg(filePath));
        return false;
    }

    for (int i = 0; i < m_cues.size(); ++i)
        stopCueAt(i);
    m_cues.clear();
    m_armedIndex = -1;

    const QJsonArray cuesArray = doc.object().value(QStringLiteral("cues")).toArray();
    for (const QJsonValue &v : cuesArray) {
        const bool isAppStream = v.toObject().value(QStringLiteral("isAppStream")).toBool(false);
        const QString path = v.toObject().value(QStringLiteral("filePath")).toString();

        Cue cue;
        cue.id = m_nextCueId++;
        cue.isAppStream = isAppStream;

        if (isAppStream) {
            // Nessuno stream live da riprendere: solo il nome tecnico
            // stabile, ri-risolto a un nodeId vero al momento del Play se
            // e quando l'app sarà di nuovo in esecuzione (vedi
            // PatchManager::startCueNow) — appStreamNodeId/nodeId restano
            // a 0 finché non succede.
            cue.appStreamMatchName = v.toObject().value(QStringLiteral("appStreamMatchName")).toString();
            cue.displayName = v.toObject().value(QStringLiteral("displayName")).toString(cue.appStreamMatchName);
        } else {
            const QFileInfo info(path);
            if (!info.exists()) {
                emit patchError(tr("File non trovato, saltato: %1").arg(path));
                continue;
            }
            cue.filePath = path;
            // Ripiega sul nome del file solo se il progetto non ha un
            // displayName salvato (file di progetto più vecchi di questa
            // funzionalità) — altrimenti un nome scelto dall'utente
            // andrebbe perso ad ogni caricamento.
            cue.displayName = v.toObject().value(QStringLiteral("displayName")).toString(info.fileName());
        }

        const QJsonArray outputsArray = v.toObject().value(QStringLiteral("outputs")).toArray();
        for (const QJsonValue &outputValue : outputsArray)
            cue.desiredOutputNames.append(outputValue.toString());
        cue.loopCount = v.toObject().value(QStringLiteral("loopCount")).toInt(-1);
        cue.reverse = v.toObject().value(QStringLiteral("reverse")).toBool(false);
        cue.rotateOutputs = v.toObject().value(QStringLiteral("rotateOutputs")).toBool(false);
        cue.rotationCycleCount = v.toObject().value(QStringLiteral("rotationCycleCount")).toInt(-1);
        cue.preWaitSeconds = v.toObject().value(QStringLiteral("preWaitSeconds")).toDouble(0.0);
        cue.durationSeconds = v.toObject().value(QStringLiteral("durationSeconds")).toDouble(0.0);
        cue.postWaitSeconds = v.toObject().value(QStringLiteral("postWaitSeconds")).toDouble(0.0);
        m_cues.append(cue);
    }
    if (!m_cues.isEmpty())
        m_armedIndex = 0;

    emit cuesChanged();
    addToRecentProjects(filePath);

    if (m_currentProjectPath != filePath) {
        m_currentProjectPath = filePath;
        emit currentProjectPathChanged();
    }
    return true;
}

void PatchManager::stopAllCues()
{
    for (int i = 0; i < m_cues.size(); ++i)
        stopCueAt(i);
}

void PatchManager::clearAllConnections()
{
    if (m_connections.isEmpty())
        return;

    for (const PatchConnection &c : std::as_const(m_connections))
        m_engine->unlinkNodes(c.linkId);
    m_connections.clear();
    emit connectionsChanged();
}

void PatchManager::addToRecentProjects(const QString &filePath)
{
    m_recentProjects.removeAll(filePath);
    m_recentProjects.prepend(filePath);
    while (m_recentProjects.size() > kMaxRecentProjects)
        m_recentProjects.removeLast();

    QSettings().setValue(kRecentProjectsSettingsKey, m_recentProjects);
    emit recentProjectsChanged();
}

void PatchManager::addJackOutput(uint32_t hardwareSinkId)
{
    // Il sink hardware (jack) è già rilevato dal discovery di PipeWireEngine
    // come PhysicalSink; se per qualche motivo non fosse ancora nel modello
    // (es. avviato dopo il discovery iniziale) andrebbe interrogato di nuovo
    // l'engine qui. Per ora ci affidiamo al segnale nodeAdded.
    Q_UNUSED(hardwareSinkId);
}

void PatchManager::addBluetoothOutput(const QString &deviceObjectPath)
{
    // 1) chiediamo a BlueZ di connettersi al dispositivo
    m_blueZ->connectDevice(deviceObjectPath);

    // 2) quando BlueZ segnala la connessione, PipeWire dovrebbe esporre
    // automaticamente il nuovo sink A2DP: lo intercettiamo tramite
    // PipeWireEngine::nodeAdded (già collegato nel costruttore), quindi qui
    // non serve fare altro se non innescare la connessione.
    connect(m_blueZ, &BluetoothManager::deviceConnectionChanged, this,
            [this, deviceObjectPath](const QString &path, bool connected) {
                if (path != deviceObjectPath)
                    return;
                if (!connected)
                    emit patchError(tr("Connessione al dispositivo Bluetooth fallita"));
                // Se connected == true, il sink apparirà via nodeAdded non
                // appena PipeWire/BlueZ completano la negoziazione A2DP.
            });
}

void PatchManager::removeOutput(uint32_t nodeId)
{
    for (auto it = m_connections.begin(); it != m_connections.end();) {
        if (it->outputNodeId == nodeId) {
            m_engine->unlinkNodes(it->linkId);
            it = m_connections.erase(it);
        } else {
            ++it;
        }
    }

    m_bluetoothOutputNodeIds.remove(nodeId);
    m_engine->setKeepAliveEnabled(nodeId, false); // no-op se non era un sink BT tenuto sveglio
    m_outputs->removeNode(nodeId);
    emit connectionsChanged();
}

void PatchManager::identifySink(uint32_t nodeId)
{
    m_engine->identifySink(nodeId);

    m_identifyingSinkId = nodeId;
    emit identifyingSinkIdChanged();
    // La sequenza di bip dura poco più di mezzo secondo (vedi
    // PipeWireEngine::Impl::onIdentifyProcess) — con un margine, riporta
    // identifyingSinkId a 0 (nessuno in test) una volta finita di suonare,
    // così la UI può evidenziare il pulsante premuto solo per quella
    // finestra di tempo.
    QTimer::singleShot(700, this, [this, nodeId]() {
        if (m_identifyingSinkId == nodeId) {
            m_identifyingSinkId = 0;
            emit identifyingSinkIdChanged();
        }
    });
}

void PatchManager::setOutputMuted(uint32_t nodeId, bool muted)
{
    m_engine->setSinkMuted(nodeId, muted);
}

QString PatchManager::effectiveOutputLabel(uint32_t nodeId, const QString &fallbackName) const
{
    // fallbackName è sempre il nome stabile (AudioNode::name) quando nodeId
    // è 0 — l'unico chiamante (cueModel()) lo passa così per un output non
    // al momento scoperto.
    const QString stableName = (nodeId != 0) ? m_outputNodeNames.value(nodeId) : fallbackName;

    const QString nickname = m_outputNicknames.value(stableName);
    if (!nickname.isEmpty())
        return nickname;

    if (nodeId != 0)
        return m_outputNodeDescriptions.value(nodeId, fallbackName);

    // Non scoperto adesso: preferisci l'ultima descrizione leggibile nota
    // per questo nome stabile al nome tecnico grezzo.
    return m_lastKnownOutputDescriptionByName.value(stableName, fallbackName);
}

void PatchManager::setOutputNickname(uint32_t nodeId, const QString &nickname)
{
    const QString stableName = m_outputNodeNames.value(nodeId);
    if (stableName.isEmpty())
        return; // sink non (più) scoperto: nessun nome stabile a cui agganciare il nickname

    const QString trimmed = nickname.trimmed();
    if (trimmed.isEmpty())
        m_outputNicknames.remove(stableName);
    else
        m_outputNicknames.insert(stableName, trimmed);

    QVariantMap toStore;
    for (auto it = m_outputNicknames.constBegin(); it != m_outputNicknames.constEnd(); ++it)
        toStore.insert(it.key(), it.value());
    QSettings().setValue(kOutputNicknamesSettingsKey, toStore);

    m_outputs->updateDescription(nodeId, effectiveOutputLabel(nodeId, stableName));
    emit cuesChanged(); // ricalcola desiredOutputLabels (etichette rotazione / pannello Trasforma)
}

void PatchManager::setOutputDelayMs(uint32_t nodeId, int delayMs)
{
    const QString stableName = m_outputNodeNames.value(nodeId);
    if (stableName.isEmpty())
        return; // sink non (più) scoperto: nessun nome stabile a cui agganciare il ritardo

    delayMs = std::clamp(delayMs, 0, 2000);
    m_outputDelaysMs.insert(stableName, delayMs);

    QVariantMap toStore;
    for (auto it = m_outputDelaysMs.constBegin(); it != m_outputDelaysMs.constEnd(); ++it)
        toStore.insert(it.key(), it.value());
    QSettings().setValue(kOutputDelaysSettingsKey, toStore);

    m_engine->setOutputDelayMs(nodeId, delayMs);
    m_outputs->updateDelayMs(nodeId, delayMs);
}

void PatchManager::toggleConnection(uint32_t inputNodeId, uint32_t outputNodeId)
{
    auto it = std::find_if(m_connections.begin(), m_connections.end(), [&](const PatchConnection &c) {
        return c.inputNodeId == inputNodeId && c.outputNodeId == outputNodeId;
    });

    if (it != m_connections.end()) {
        m_engine->unlinkNodes(it->linkId);
        m_connections.erase(it);
        emit connectionsChanged();
        return;
    }

    const uint32_t linkId = m_engine->linkNodes(inputNodeId, outputNodeId);
    if (linkId == 0) {
        emit patchError(tr("Impossibile creare il collegamento"));
        return;
    }

    m_connections.append(PatchConnection{ inputNodeId, outputNodeId, linkId });
    emit connectionsChanged();
}

bool PatchManager::isConnected(uint32_t inputNodeId, uint32_t outputNodeId) const
{
    return std::any_of(m_connections.begin(), m_connections.end(), [&](const PatchConnection &c) {
        return c.inputNodeId == inputNodeId && c.outputNodeId == outputNodeId;
    });
}

QVariantList PatchManager::connectionsModel() const
{
    QVariantList result;
    result.reserve(m_connections.size());
    for (const PatchConnection &c : m_connections) {
        QVariantMap entry;
        entry[QStringLiteral("inputNodeId")] = c.inputNodeId;
        entry[QStringLiteral("outputNodeId")] = c.outputNodeId;
        entry[QStringLiteral("linkId")] = c.linkId;
        result.append(entry);
    }
    return result;
}

QVariantList PatchManager::cueModel() const
{
    QVariantList result;
    result.reserve(m_cues.size());
    for (const Cue &c : m_cues) {
        QVariantMap entry;
        entry[QStringLiteral("displayName")] = c.displayName;
        entry[QStringLiteral("isAppStream")] = c.isAppStream;
        entry[QStringLiteral("nodeId")] = c.nodeId;
        // nodeId live (se presenti) di tutti gli output attualmente
        // scoperti il cui nome compare in desiredOutputNames — usata dalla
        // UI per disegnare il cavo del routing "voluto" per questa traccia
        // indipendentemente dal fatto che sia effettivamente in
        // riproduzione: il routing è persistente, non solo il collegamento
        // PipeWire reale (che esiste solo mentre la traccia è live).
        // Costruita nell'ORDINE di desiredOutputNames (ordine di
        // collegamento/rotazione), non per nodeId scoperto: necessario
        // perché desiredOutputLabels (sotto) e l'evidenziazione
        // dell'output attivo in rotazione si basano sulla stessa posizione.
        QVariantList desiredOutputNodeIds;
        // Nome leggibile (AudioNode::description, es. "JBL Xtreme 3") di
        // ogni output in desiredOutputNodeIds, stessa posizione — per le
        // etichette di rotazione sotto la riga e il riepilogo nel pannello
        // Trasforma, invece del nome interno PipeWire. Ripiega sul nome
        // interno se l'output non è al momento scoperto/live.
        QVariantList desiredOutputLabels;
        // Nome stabile grezzo di ogni output in desiredOutputNodeIds,
        // stessa posizione — serve alla UI per poter rimuovere un singolo
        // output "voluto" (PatchManager::removeCueDesiredOutputByName)
        // anche quando non è al momento scoperto/live (nodeId 0, quindi
        // nessun cavo disegnato/cliccabile sul Canvas: l'unico modo di
        // rimuoverlo è dal pallino/etichetta stessa, che ha bisogno del
        // nome per farlo).
        QVariantList desiredOutputNames;
        // nodeId dell'output davvero attivo in questo momento quando
        // rotateOutputs è true (0 se non in rotazione) — utile alla UI per
        // evidenziare quale etichetta/cavo è quello vero, non solo voluto.
        uint32_t activeOutputNodeId = 0;
        for (int oi = 0; oi < c.desiredOutputNames.size(); ++oi) {
            const QString &name = c.desiredOutputNames.at(oi);
            const uint32_t nid = m_outputNodeNames.key(name, 0);
            desiredOutputNodeIds.append(nid);
            desiredOutputLabels.append(effectiveOutputLabel(nid, name));
            desiredOutputNames.append(name);
            if (c.rotateOutputs && oi == c.rotateOutputIndex % c.desiredOutputNames.size())
                activeOutputNodeId = nid;
        }
        entry[QStringLiteral("desiredOutputNodeIds")] = desiredOutputNodeIds;
        entry[QStringLiteral("desiredOutputLabels")] = desiredOutputLabels;
        entry[QStringLiteral("desiredOutputNames")] = desiredOutputNames;
        entry[QStringLiteral("loopCount")] = c.loopCount;
        entry[QStringLiteral("reverse")] = c.reverse;
        entry[QStringLiteral("rotateOutputs")] = c.rotateOutputs;
        entry[QStringLiteral("rotationCycleCount")] = c.rotationCycleCount;
        entry[QStringLiteral("activeOutputNodeId")] = activeOutputNodeId;

        // --- Timing stile QLab (pannello di configurazione a tasto destro) ---
        entry[QStringLiteral("preWaitSeconds")] = c.preWaitSeconds;
        entry[QStringLiteral("durationSeconds")] = c.durationSeconds;
        entry[QStringLiteral("postWaitSeconds")] = c.postWaitSeconds;
        // Stato di trasporto per-cue: più righe possono essere "in
        // riproduzione" (nodeId > 0) in contemporanea, quindi la UI non può
        // più affidarsi a un singolo indice globale per capire quale
        // icona/colore mostrare su ciascuna riga.
        entry[QStringLiteral("waitingToStart")] = c.waitingToStart;
        entry[QStringLiteral("inPostWait")] = c.inPostWait;
        entry[QStringLiteral("paused")] = c.paused;
        result.append(entry);
    }
    return result;
}

void PatchManager::setKeepAliveEnabled(bool enabled)
{
    if (m_keepAliveSettingEnabled == enabled)
        return;

    m_keepAliveSettingEnabled = enabled;
    QSettings().setValue(kKeepAliveSettingsKey, enabled);

    for (uint32_t nodeId : std::as_const(m_bluetoothOutputNodeIds))
        m_engine->setKeepAliveEnabled(nodeId, enabled);

    emit keepAliveEnabledChanged();
}

void PatchManager::setKeepAlivePingFrequencyHz(int hz)
{
    hz = std::clamp(hz, 1, 24000);
    if (m_keepAlivePingFrequencyHz == hz)
        return;
    m_keepAlivePingFrequencyHz = hz;
    QSettings().setValue(kKeepAlivePingFrequencyKey, hz);
    m_engine->setKeepAlivePingFrequency(hz);
    emit keepAlivePingSettingsChanged();
}

void PatchManager::setKeepAlivePingAmplitudeUnits(int units)
{
    units = std::clamp(units, 0, 100000);
    if (m_keepAlivePingAmplitudeUnits == units)
        return;
    m_keepAlivePingAmplitudeUnits = units;
    QSettings().setValue(kKeepAlivePingAmplitudeKey, units);
    m_engine->setKeepAlivePingAmplitude(units / 100000.0f);
    emit keepAlivePingSettingsChanged();
}

void PatchManager::setKeepAlivePingDurationMs(int ms)
{
    ms = std::max(0, ms);
    if (m_keepAlivePingDurationMs == ms)
        return;
    m_keepAlivePingDurationMs = ms;
    QSettings().setValue(kKeepAlivePingDurationKey, ms);
    m_engine->setKeepAlivePingDuration(ms / 1000.0);
    emit keepAlivePingSettingsChanged();
}

void PatchManager::setKeepAlivePingPeriodSeconds(int seconds)
{
    seconds = std::max(1, seconds);
    if (m_keepAlivePingPeriodSeconds == seconds)
        return;
    m_keepAlivePingPeriodSeconds = seconds;
    QSettings().setValue(kKeepAlivePingPeriodKey, seconds);
    m_engine->setKeepAlivePingPeriod(seconds);
    emit keepAlivePingSettingsChanged();
}
