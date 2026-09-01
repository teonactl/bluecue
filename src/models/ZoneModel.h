#pragma once

#include <QAbstractListModel>

#include "audio/ZoneManager.h"

// Espone la lista di Zone gestite da ZoneManager come modello QML,
// così che ZoneCard.qml possa mostrarle in un Repeater/ListView.
class ZoneModel : public QAbstractListModel
{
    Q_OBJECT

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DisplayNameRole,
        LinkedNodeIdRole,
        IsConnectedRole,
    };

    explicit ZoneModel(ZoneManager *zoneManager, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private slots:
    void onZoneAdded(const Zone &zone);
    void onZoneRemoved(const QString &zoneId);
    void onZoneUpdated(const Zone &zone);

private:
    ZoneManager *m_zoneManager;
    QVector<Zone> m_zones;
};
