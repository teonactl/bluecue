#include "ZoneModel.h"

#include <algorithm>

ZoneModel::ZoneModel(ZoneManager *zoneManager, QObject *parent)
    : QAbstractListModel(parent)
    , m_zoneManager(zoneManager)
{
    m_zones = m_zoneManager->zones();

    connect(m_zoneManager, &ZoneManager::zoneAdded, this, &ZoneModel::onZoneAdded);
    connect(m_zoneManager, &ZoneManager::zoneRemoved, this, &ZoneModel::onZoneRemoved);
    connect(m_zoneManager, &ZoneManager::zoneUpdated, this, &ZoneModel::onZoneUpdated);
}

int ZoneModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_zones.size();
}

QVariant ZoneModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_zones.size())
        return {};

    const Zone &zone = m_zones.at(index.row());
    switch (role) {
    case IdRole:
        return zone.id;
    case DisplayNameRole:
        return zone.displayName;
    case LinkedNodeIdRole:
        return zone.linkedNodeId;
    case IsConnectedRole:
        return zone.activeLinkId != 0;
    default:
        return {};
    }
}

QHash<int, QByteArray> ZoneModel::roleNames() const
{
    return {
        { IdRole, "zoneId" },
        { DisplayNameRole, "displayName" },
        { LinkedNodeIdRole, "linkedNodeId" },
        { IsConnectedRole, "isConnected" },
    };
}

void ZoneModel::onZoneAdded(const Zone &zone)
{
    beginInsertRows(QModelIndex(), m_zones.size(), m_zones.size());
    m_zones.append(zone);
    endInsertRows();
}

void ZoneModel::onZoneRemoved(const QString &zoneId)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
                            [&](const Zone &z) { return z.id == zoneId; });
    if (it == m_zones.end())
        return;

    const int row = static_cast<int>(std::distance(m_zones.begin(), it));
    beginRemoveRows(QModelIndex(), row, row);
    m_zones.erase(it);
    endRemoveRows();
}

void ZoneModel::onZoneUpdated(const Zone &zone)
{
    auto it = std::find_if(m_zones.begin(), m_zones.end(),
                            [&](const Zone &z) { return z.id == zone.id; });
    if (it == m_zones.end())
        return;

    *it = zone;
    const int row = static_cast<int>(std::distance(m_zones.begin(), it));
    const QModelIndex idx = index(row);
    emit dataChanged(idx, idx);
}
