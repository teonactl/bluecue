#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include "AudioNode.h"

class PipeWireEngine;

// Rappresenta una zona logica (es. "Cucina", "Salotto"): un sink virtuale
// PipeWire più il dispositivo Bluetooth a cui è attualmente collegata.
struct Zone
{
    QString id;                 // identificativo stabile, es. "cucina"
    QString displayName;        // nome mostrato in UI, es. "Cucina"
    uint32_t virtualSinkId = 0; // id del nodo PipeWire creato per questa zona
    uint32_t linkedNodeId = 0;  // id del sink Bluetooth attualmente collegato (0 = nessuno)
    uint32_t activeLinkId = 0;  // id del link PipeWire attivo (0 = nessuno)
};

// Orchestratore ad alto livello: crea/distrugge zone e ne gestisce
// l'assegnazione a un dispositivo Bluetooth, delegando le operazioni
// tecniche a PipeWireEngine.
class ZoneManager : public QObject
{
    Q_OBJECT

public:
    explicit ZoneManager(PipeWireEngine *engine, QObject *parent = nullptr);

    // Crea una nuova zona con un sink virtuale dedicato.
    Q_INVOKABLE QString addZone(const QString &displayName);

    // Rimuove una zona esistente e il suo sink virtuale.
    Q_INVOKABLE void removeZone(const QString &zoneId);

    // Assegna (o riassegna) una zona a un dispositivo Bluetooth (nodo PipeWire).
    // Rimuove automaticamente il link precedente, se presente.
    Q_INVOKABLE void assignZoneToDevice(const QString &zoneId, uint32_t bluetoothNodeId);

    Q_INVOKABLE QVector<Zone> zones() const;

signals:
    void zoneAdded(const Zone &zone);
    void zoneRemoved(const QString &zoneId);
    void zoneUpdated(const Zone &zone);

private:
    PipeWireEngine *m_engine;
    QVector<Zone> m_zones;
};
