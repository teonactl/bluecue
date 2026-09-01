#include "AppleBluetoothManager.h"

// NOTA: vedi il commento in cima a AppleBluetoothManager.h — scritto
// seguendo la documentazione IOBluetooth ma mai compilato (nessun SDK
// macOS in questo ambiente di sviluppo, Linux). Da validare su un Mac
// reale con un dispositivo A2DP accoppiato prima di considerarlo pronto.

#import <IOBluetooth/IOBluetooth.h>
#import <Foundation/Foundation.h>

#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QVariantMap>

namespace {

// IOBluetooth restituisce l'indirizzo nel formato "aa-bb-cc-dd-ee-ff":
// normalizzato qui a maiuscolo/due punti per coerenza con l'address di
// BlueZManager (Linux) — usato ovunque nell'app come chiave di
// correlazione stabile (es. PatchManager::m_outputNodeMacs).
QString normalizeMac(NSString *raw)
{
    QString s = QString::fromNSString(raw).toUpper();
    s.replace(QLatin1Char('-'), QLatin1Char(':'));
    return s;
}

bool deviceSupportsA2dpSink(IOBluetoothDevice *device)
{
    IOBluetoothSDPUUID *uuid = [IOBluetoothSDPUUID uuid16:kBluetoothSDPUUID16ServiceClassAudioSink];
    return [device getServiceRecordForUUID:uuid] != nil;
}

BluetoothDevice toBluetoothDevice(IOBluetoothDevice *device)
{
    BluetoothDevice result;
    result.address = normalizeMac(device.addressString);
    result.objectPath = result.address; // IOBluetooth non ha un "object path": si usa il MAC come chiave opaca
    result.name = QString::fromNSString(device.name ?: device.addressString);
    result.paired = device.isPaired;
    result.connected = device.isConnected;
    result.isAudioSink = deviceSupportsA2dpSink(device);
    // Nessuna API pubblica per la batteria di dispositivi di terze parti
    // su macOS — vedi il commento in AppleBluetoothManager.h.
    result.batteryPercentage = -1;
    return result;
}

} // namespace

struct AppleBluetoothManager::Impl
{
    mutable QMutex mutex;
    QVector<BluetoothDevice> devices;
};

AppleBluetoothManager::AppleBluetoothManager(QObject *parent)
    : BluetoothManager(parent)
    , d(std::make_unique<Impl>())
{
}

AppleBluetoothManager::~AppleBluetoothManager() = default;

bool AppleBluetoothManager::refreshDevices()
{
    NSArray<IOBluetoothDevice *> *paired = [IOBluetoothDevice pairedDevices];
    if (!paired) {
        emit managerError(QStringLiteral("IOBluetoothDevice pairedDevices ha restituito nil "
                                          "(Bluetooth disattivato o permesso negato all'app?)"));
        return false;
    }

    QVector<BluetoothDevice> discovered;
    discovered.reserve(static_cast<int>(paired.count));
    for (IOBluetoothDevice *device in paired)
        discovered.append(toBluetoothDevice(device));

    {
        QMutexLocker locker(&d->mutex);
        d->devices = discovered;
    }
    emit devicesChanged();
    return true;
}

QVector<BluetoothDevice> AppleBluetoothManager::devices() const
{
    QMutexLocker locker(&d->mutex);
    return d->devices;
}

QVariantList AppleBluetoothManager::deviceModel() const
{
    QVariantList result;
    QMutexLocker locker(&d->mutex);
    for (const BluetoothDevice &dev : d->devices) {
        QVariantMap entry;
        entry.insert(QStringLiteral("objectPath"), dev.objectPath);
        entry.insert(QStringLiteral("address"), dev.address);
        entry.insert(QStringLiteral("name"), dev.name);
        entry.insert(QStringLiteral("paired"), dev.paired);
        entry.insert(QStringLiteral("connected"), dev.connected);
        entry.insert(QStringLiteral("isAudioSink"), dev.isAudioSink);
        entry.insert(QStringLiteral("batteryPercentage"), dev.batteryPercentage);
        result.append(entry);
    }
    return result;
}

void AppleBluetoothManager::connectDevice(const QString &objectPath)
{
    NSArray<IOBluetoothDevice *> *paired = [IOBluetoothDevice pairedDevices];
    IOBluetoothDevice *target = nil;
    for (IOBluetoothDevice *device in paired) {
        if (normalizeMac(device.addressString) == objectPath) {
            target = device;
            break;
        }
    }
    if (!target) {
        emit managerError(QStringLiteral("Dispositivo non trovato: %1").arg(objectPath));
        return;
    }

    // -openConnection è sincrona/bloccante lato IOBluetooth: eseguita su una
    // coda di background per non bloccare il thread Qt, poi si torna sul
    // main-thread per aggiornare stato/segnali — stesso contratto
    // "asincrono" di BlueZManager::connectDevice (che usa
    // QDBusPendingCallWatcher).
    AppleBluetoothManager *self = this;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        const IOReturn status = [target openConnection];
        const bool connected = (status == kIOReturnSuccess);
        dispatch_async(dispatch_get_main_queue(), ^{
            QMetaObject::invokeMethod(self, [self, objectPath, connected, status]() {
                if (!connected) {
                    emit self->managerError(QStringLiteral("Connessione fallita (IOReturn %1)").arg(status));
                }
                self->refreshDevices();
                emit self->deviceConnectionChanged(objectPath, connected);
            }, Qt::QueuedConnection);
        });
    });
}

void AppleBluetoothManager::disconnectDevice(const QString &objectPath)
{
    NSArray<IOBluetoothDevice *> *paired = [IOBluetoothDevice pairedDevices];
    IOBluetoothDevice *target = nil;
    for (IOBluetoothDevice *device in paired) {
        if (normalizeMac(device.addressString) == objectPath) {
            target = device;
            break;
        }
    }
    if (!target) {
        emit managerError(QStringLiteral("Dispositivo non trovato: %1").arg(objectPath));
        return;
    }

    AppleBluetoothManager *self = this;
    dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), ^{
        [target closeConnection];
        dispatch_async(dispatch_get_main_queue(), ^{
            QMetaObject::invokeMethod(self, [self, objectPath]() {
                self->refreshDevices();
                emit self->deviceConnectionChanged(objectPath, false);
            }, Qt::QueuedConnection);
        });
    });
}
