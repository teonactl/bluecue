#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QMetaType>
#include <memory>

#include "BluetoothManager.h"

// Wrapper Qt D-Bus attorno a org.bluez per scoprire, connettere e monitorare
// i dispositivi audio Bluetooth accoppiati — backend BluetoothManager per
// Linux.
//
// Scheletro: la logica di parsing degli oggetti D-Bus (GetManagedObjects,
// PropertiesChanged) va implementata nel .cpp; qui sono già definiti i punti
// di estensione pubblici usati da ZoneManager / UI.
class BlueZManager : public BluetoothManager
{
    Q_OBJECT

public:
    explicit BlueZManager(QObject *parent = nullptr);
    ~BlueZManager() override;

    // Interroga BlueZ via D-Bus (org.freedesktop.DBus.ObjectManager
    // .GetManagedObjects su "org.bluez") e popola la lista dei dispositivi
    // accoppiati. Chiamata sincrona (D-Bus locale, tipicamente <10ms).
    bool refreshDevices() override;

    QVector<BluetoothDevice> devices() const override;

    QVariantList deviceModel() const override;

    // Avvia la connessione a un dispositivo già accoppiato (async su D-Bus).
    void connectDevice(const QString &objectPath) override;

    // Disconnette un dispositivo attualmente connesso.
    void disconnectDevice(const QString &objectPath) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
