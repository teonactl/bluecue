#pragma once

#include <QAbstractListModel>
#include <QVector>

#include "audio/AudioNode.h"

// Modello generico per una colonna di porte audio (input o output).
// Usato sia per la colonna "Input" (file, microfono, ...) sia per la
// colonna "Output" (jack computer, dispositivi Bluetooth, ...): la
// direzione è fissata alla costruzione e determina solo quali nodi
// PipeWire vengono mostrati (source vs sink).
class PortModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum class Direction { Input, Output };

    enum Roles {
        NodeIdRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        IsBluetoothRole,
        KindRole,
        BatteryPercentageRole,
        MutedRole,
    };

    explicit PortModel(Direction direction, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Aggiunge/aggiorna un nodo nel modello (chiamato quando PipeWireEngine
    // o BlueZManager segnalano un nuovo device/nodo pertinente a questa colonna).
    void upsertNode(const AudioNode &node);
    void removeNode(uint32_t nodeId);

    // Aggiorna solo il campo description di un nodo già presente (es. un
    // nickname assegnato dall'utente a un sink Output), senza toccare gli
    // altri campi (kind/isBluetooth/bluetoothMac) — a differenza di
    // upsertNode, che sostituisce l'intero nodo e richiederebbe di
    // conoscerli tutti di nuovo solo per cambiare il nome mostrato.
    void updateDescription(uint32_t nodeId, const QString &description);

    // Aggiorna solo batteryPercentage di un nodo già presente — arriva da
    // BlueZManager in modo asincrono rispetto alla scoperta PipeWire del
    // nodo (vedi PatchManager), quindi va spesso applicato dopo che il nodo
    // esiste già nel modello, non solo alla creazione.
    void updateBatteryPercentage(uint32_t nodeId, int percentage);

    // Sposta il nodo dalla posizione fromIndex a toIndex (riordino manuale
    // via i pulsanti ▲/▼ della colonna Output/Playlist) — puro riordino
    // visivo, non tocca lo stato PipeWire del nodo. No-op se gli indici
    // sono fuori range o uguali.
    Q_INVOKABLE void moveNode(int fromIndex, int toIndex);

    Direction direction() const { return m_direction; }

private:
    Direction m_direction;
    QVector<AudioNode> m_nodes;
};
