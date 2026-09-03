#include "PortModel.h"

#include <algorithm>

PortModel::PortModel(Direction direction, QObject *parent)
    : QAbstractListModel(parent)
    , m_direction(direction)
{
}

int PortModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_nodes.size();
}

QVariant PortModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_nodes.size())
        return {};

    const AudioNode &node = m_nodes.at(index.row());
    switch (role) {
    case NodeIdRole:
        return node.id;
    case NameRole:
        return node.name;
    case DescriptionRole:
        return node.description.isEmpty() ? node.name : node.description;
    case IsBluetoothRole:
        return node.isBluetooth;
    case KindRole:
        return static_cast<int>(node.kind);
    case BatteryPercentageRole:
        return node.batteryPercentage;
    case MutedRole:
        return node.muted;
    case DelayMsRole:
        return node.delayMs;
    default:
        return {};
    }
}

QHash<int, QByteArray> PortModel::roleNames() const
{
    return {
        { NodeIdRole, "nodeId" },
        { NameRole, "name" },
        { DescriptionRole, "description" },
        { IsBluetoothRole, "isBluetooth" },
        { KindRole, "kind" },
        { BatteryPercentageRole, "batteryPercentage" },
        { MutedRole, "muted" },
        { DelayMsRole, "delayMs" },
    };
}

void PortModel::upsertNode(const AudioNode &node)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                            [&](const AudioNode &n) { return n.id == node.id; });

    if (it != m_nodes.end()) {
        *it = node;
        const int row = static_cast<int>(std::distance(m_nodes.begin(), it));
        const QModelIndex idx = index(row);
        emit dataChanged(idx, idx);
        return;
    }

    beginInsertRows(QModelIndex(), m_nodes.size(), m_nodes.size());
    m_nodes.append(node);
    endInsertRows();
}

void PortModel::updateDescription(uint32_t nodeId, const QString &description)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                            [&](const AudioNode &n) { return n.id == nodeId; });
    if (it == m_nodes.end() || it->description == description)
        return;

    it->description = description;
    const int row = static_cast<int>(std::distance(m_nodes.begin(), it));
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { DescriptionRole });
}

void PortModel::updateBatteryPercentage(uint32_t nodeId, int percentage)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                            [&](const AudioNode &n) { return n.id == nodeId; });
    if (it == m_nodes.end() || it->batteryPercentage == percentage)
        return;

    it->batteryPercentage = percentage;
    const int row = static_cast<int>(std::distance(m_nodes.begin(), it));
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { BatteryPercentageRole });
}

void PortModel::updateDelayMs(uint32_t nodeId, int delayMs)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                            [&](const AudioNode &n) { return n.id == nodeId; });
    if (it == m_nodes.end() || it->delayMs == delayMs)
        return;

    it->delayMs = delayMs;
    const int row = static_cast<int>(std::distance(m_nodes.begin(), it));
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx, { DelayMsRole });
}

void PortModel::moveNode(int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= m_nodes.size()
            || toIndex < 0 || toIndex >= m_nodes.size() || fromIndex == toIndex)
        return;

    // beginMoveRows vuole la posizione di destinazione ESPRESSA PRIMA dello
    // spostamento: per uno spostamento in avanti (toIndex > fromIndex) va
    // indicata la riga successiva a quella finale desiderata, altrimenti
    // Qt la interpreta come "prima dell'elemento che sta per spostarsi via"
    // e il risultato finisce shiftato di una posizione.
    const int destination = toIndex > fromIndex ? toIndex + 1 : toIndex;
    beginMoveRows(QModelIndex(), fromIndex, fromIndex, QModelIndex(), destination);
    m_nodes.move(fromIndex, toIndex);
    endMoveRows();
}

void PortModel::removeNode(uint32_t nodeId)
{
    auto it = std::find_if(m_nodes.begin(), m_nodes.end(),
                            [&](const AudioNode &n) { return n.id == nodeId; });
    if (it == m_nodes.end())
        return;

    const int row = static_cast<int>(std::distance(m_nodes.begin(), it));
    beginRemoveRows(QModelIndex(), row, row);
    m_nodes.erase(it);
    endRemoveRows();
}
