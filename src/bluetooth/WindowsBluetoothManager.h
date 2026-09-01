#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <memory>

#include "BluetoothManager.h"

// Backend BluetoothManager per Windows, sopra le API Win32 Bluetooth
// classiche (bluetoothapis.h/bthdef.h — non WinRT: evita di trascinarsi
// dietro le proiezioni C++/WinRT solo per questo).
//
// Limiti noti rispetto a BlueZManager (Linux):
//  - Nessuna percentuale batteria: come su macOS, Windows non espone
//    un'API pubblica generica per la batteria di dispositivi Bluetooth di
//    terze parti — BluetoothDevice::batteryPercentage resta sempre -1.
//  - objectPath qui è l'indirizzo MAC (formattato "AA:BB:CC:DD:EE:FF"),
//    non un vero object path — trattato comunque come chiave opaca dal
//    resto dell'app.
//
// NON compilato/testato in questo ambiente (Linux, nessun SDK Windows
// disponibile) — vedi la stessa nota in WasapiEngine.h.
class WindowsBluetoothManager : public BluetoothManager
{
    Q_OBJECT

public:
    explicit WindowsBluetoothManager(QObject *parent = nullptr);
    ~WindowsBluetoothManager() override;

    bool refreshDevices() override;
    QVector<BluetoothDevice> devices() const override;
    QVariantList deviceModel() const override;
    void connectDevice(const QString &objectPath) override;
    void disconnectDevice(const QString &objectPath) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
