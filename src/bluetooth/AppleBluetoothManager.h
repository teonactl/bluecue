#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <memory>

#include "BluetoothManager.h"

// Backend BluetoothManager per macOS, sopra IOBluetooth (framework
// Objective-C — l'implementazione vera è in AppleBluetoothManager.mm,
// questo header resta puro C++/Qt così può essere incluso anche da
// main.cpp senza portarsi dietro Objective-C).
//
// Limiti noti rispetto a BlueZManager (Linux):
//  - Nessuna percentuale batteria: macOS non espone un'API pubblica per la
//    batteria di dispositivi Bluetooth di terze parti (solo per gli
//    auricolari Apple stessi, via un'API privata non utilizzabile qui) —
//    BluetoothDevice::batteryPercentage resta sempre -1 su questo backend.
//  - objectPath qui è l'indirizzo MAC del dispositivo (IOBluetooth non ha
//    un concetto di D-Bus object path): trattato comunque come chiave
//    opaca dal resto dell'app, che non lo interpreta mai.
//
// NON compilato/testato in questo ambiente (Linux, nessun SDK macOS
// disponibile) — vedi la stessa nota in CoreAudioEngine.h.
class AppleBluetoothManager : public BluetoothManager
{
    Q_OBJECT

public:
    explicit AppleBluetoothManager(QObject *parent = nullptr);
    ~AppleBluetoothManager() override;

    bool refreshDevices() override;
    QVector<BluetoothDevice> devices() const override;
    QVariantList deviceModel() const override;
    void connectDevice(const QString &objectPath) override;
    void disconnectDevice(const QString &objectPath) override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
