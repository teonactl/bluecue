#include "ZoneManager.h"
#include "PipeWireEngine.h"

#include <QUuid>
#include <algorithm>

ZoneManager::ZoneManager(PipeWireEngine *engine, QObject *parent)
    : QObject(parent)
    , m_engine(engine)
{
}

QString ZoneManager::addZone(const QString &displayName)
{
    Zone zone;
    zone.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    zone.displayName = displayName;

    // La creazione del sink virtuale è asincrona: PipeWireEngine notificherà
    // nodeAdded() quando il nodo sarà effettivamente pronto. Per semplicità
    // in questo scheletro registriamo subito la zona; il collegamento
    // zone.virtualSinkId andrà completato ascoltando nodeAdded() e
    // riconoscendo il nodo per nome (es. "zone-<id>").
    m_engine->createVirtualSink(QStringLiteral("zone-%1").arg(zone.id), displayName);

    m_zones.append(zone);
    emit zoneAdded(zone);
    return zone.id;
}

void ZoneManager::removeZone(const QString &zoneId)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
                            [&](const Zone &z) { return z.id == zoneId; });
    if (it == m_zones.end())
        return;

    if (it->activeLinkId != 0)
        m_engine->unlinkNodes(it->activeLinkId);
    if (it->virtualSinkId != 0)
        m_engine->removeVirtualSink(it->virtualSinkId);

    m_zones.erase(it);
    emit zoneRemoved(zoneId);
}

void ZoneManager::assignZoneToDevice(const QString &zoneId, uint32_t bluetoothNodeId)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
                            [&](const Zone &z) { return z.id == zoneId; });
    if (it == m_zones.end())
        return;

    if (it->activeLinkId != 0) {
        m_engine->unlinkNodes(it->activeLinkId);
        it->activeLinkId = 0;
    }

    it->linkedNodeId = bluetoothNodeId;
    it->activeLinkId = m_engine->linkNodes(it->virtualSinkId, bluetoothNodeId);

    emit zoneUpdated(*it);
}

QVector<Zone> ZoneManager::zones() const
{
    return m_zones;
}
