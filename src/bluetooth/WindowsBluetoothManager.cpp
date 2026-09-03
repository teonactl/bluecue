#include "WindowsBluetoothManager.h"

// NOTA: vedi il commento in cima a WindowsBluetoothManager.h — scritto
// seguendo la documentazione Win32 Bluetooth ma mai compilato (nessun SDK
// Windows in questo ambiente di sviluppo, Linux). Da validare su Windows
// reale con un dispositivo A2DP accoppiato.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bluetoothapis.h>

#include <QMap>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QString>
#include <QThread>
#include <QVariantMap>
#include <vector>

#pragma comment(lib, "Bthprops.lib")

namespace {

QString formatAddress(const BLUETOOTH_ADDRESS &address)
{
    // BLUETOOTH_ADDRESS.rgBytes è in ordine little-endian rispetto alla
    // notazione "AA:BB:CC:DD:EE:FF" convenzionale — va stampato al
    // contrario (byte 5 per primo), stessa convenzione usata da ogni tool
    // Win32 Bluetooth.
    return QStringLiteral("%1:%2:%3:%4:%5:%6")
        .arg(address.rgBytes[5], 2, 16, QLatin1Char('0'))
        .arg(address.rgBytes[4], 2, 16, QLatin1Char('0'))
        .arg(address.rgBytes[3], 2, 16, QLatin1Char('0'))
        .arg(address.rgBytes[2], 2, 16, QLatin1Char('0'))
        .arg(address.rgBytes[1], 2, 16, QLatin1Char('0'))
        .arg(address.rgBytes[0], 2, 16, QLatin1Char('0'))
        .toUpper();
}

bool deviceSupportsA2dpSink(HANDLE radio, BLUETOOTH_DEVICE_INFO &deviceInfo)
{
    DWORD serviceCount = 0;
    if (BluetoothEnumerateInstalledServices(radio, &deviceInfo, &serviceCount, nullptr) != ERROR_SUCCESS
        && serviceCount == 0) {
        return false;
    }
    if (serviceCount == 0)
        return false;

    std::vector<GUID> guids(serviceCount);
    if (BluetoothEnumerateInstalledServices(radio, &deviceInfo, &serviceCount, guids.data()) != ERROR_SUCCESS)
        return false;

    for (DWORD i = 0; i < serviceCount; ++i) {
        if (IsEqualGUID(guids[i], AudioSinkServiceClass_UUID))
            return true;
    }
    return false;
}

} // namespace

struct WindowsBluetoothManager::Impl
{
    mutable QMutex mutex;
    QVector<BluetoothDevice> devices;

    // Per address: radio + ultima BLUETOOTH_DEVICE_INFO nota, necessari a
    // connectDevice/disconnectDevice (BluetoothSetServiceState vuole
    // entrambi). I radio handle restano aperti finché non sostituiti da un
    // refresh successivo — vedi refreshDevices().
    struct DeviceRecord { HANDLE radio = nullptr; BLUETOOTH_DEVICE_INFO info{}; };
    QMap<QString, DeviceRecord> recordsByAddress;

    void clearRadioHandles()
    {
        QSet<HANDLE> closed;
        for (auto it = recordsByAddress.begin(); it != recordsByAddress.end(); ++it) {
            if (it->radio && !closed.contains(it->radio)) {
                CloseHandle(it->radio);
                closed.insert(it->radio);
            }
        }
        recordsByAddress.clear();
    }
};

WindowsBluetoothManager::WindowsBluetoothManager(QObject *parent)
    : BluetoothManager(parent)
    , d(std::make_unique<Impl>())
{
}

WindowsBluetoothManager::~WindowsBluetoothManager()
{
    d->clearRadioHandles();
}

bool WindowsBluetoothManager::refreshDevices()
{
    QVector<BluetoothDevice> discovered;
    QMap<QString, Impl::DeviceRecord> newRecords;

    BLUETOOTH_FIND_RADIO_PARAMS radioParams{};
    radioParams.dwSize = sizeof(radioParams);
    HANDLE radioHandle = nullptr;
    HBLUETOOTH_RADIO_FIND radioFind = BluetoothFindFirstRadio(&radioParams, &radioHandle);
    if (!radioFind) {
        emit managerError(QStringLiteral("Nessun radio Bluetooth trovato (spento o assente?)"));
        return false;
    }

    do {
        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams{};
        searchParams.dwSize = sizeof(searchParams);
        searchParams.fReturnAuthenticated = TRUE;
        searchParams.fReturnRemembered = TRUE;
        searchParams.fReturnConnected = TRUE;
        searchParams.fReturnUnknown = FALSE;
        searchParams.fIssueInquiry = FALSE;
        searchParams.hRadio = radioHandle;

        BLUETOOTH_DEVICE_INFO deviceInfo{};
        deviceInfo.dwSize = sizeof(deviceInfo);
        HBLUETOOTH_DEVICE_FIND deviceFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
        if (deviceFind) {
            do {
                const QString address = formatAddress(deviceInfo.Address);

                BluetoothDevice dev;
                dev.objectPath = address;
                dev.address = address;
                dev.name = QString::fromWCharArray(deviceInfo.szName);
                dev.paired = deviceInfo.fRemembered;
                dev.connected = deviceInfo.fConnected;
                dev.isAudioSink = deviceSupportsA2dpSink(radioHandle, deviceInfo);
                // Nessuna API pubblica per la batteria di dispositivi di
                // terze parti su Windows — vedi WindowsBluetoothManager.h.
                dev.batteryPercentage = -1;
                discovered.append(dev);

                newRecords.insert(address, Impl::DeviceRecord{ radioHandle, deviceInfo });

                deviceInfo.dwSize = sizeof(deviceInfo);
            } while (BluetoothFindNextDevice(deviceFind, &deviceInfo));
            BluetoothFindDeviceClose(deviceFind);
        }
    } while (BluetoothFindNextRadio(radioFind, &radioHandle));
    BluetoothFindRadioClose(radioFind);

    {
        QMutexLocker locker(&d->mutex);
        d->clearRadioHandles();
        // clearRadioHandles() chiuderebbe anche gli handle appena raccolti
        // in newRecords se coincidessero con quelli vecchi (stesso radio
        // riaperto a ogni refresh, handle diversi ogni volta) — sono due
        // insiemi di handle SEPARATI (i vecchi sono già stati chiusi sopra
        // PRIMA di questa riga, i nuovi restano validi), quindi va bene
        // assegnare direttamente.
        d->recordsByAddress = newRecords;
        d->devices = discovered;
    }
    emit devicesChanged();
    return true;
}

QVector<BluetoothDevice> WindowsBluetoothManager::devices() const
{
    QMutexLocker locker(&d->mutex);
    return d->devices;
}

QVariantList WindowsBluetoothManager::deviceModel() const
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

void WindowsBluetoothManager::connectDevice(const QString &objectPath)
{
    Impl::DeviceRecord record;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->recordsByAddress.contains(objectPath)) {
            locker.unlock();
            emit managerError(QStringLiteral("Dispositivo non trovato: %1").arg(objectPath));
            return;
        }
        record = d->recordsByAddress.value(objectPath);
    }

    // record.radio è lo stesso HANDLE tenuto in Impl::recordsByAddress: se
    // l'utente fa refresh (bottone "Aggiorna" o semplice riapertura del
    // dialog Bluetooth, vedi Main.qml) mentre BluetoothSetServiceState sta
    // ancora girando sul thread qui sotto, refreshDevices() lo chiude
    // (clearRadioHandles()) e magari l'OS lo riassegna subito ad altro —
    // il thread lo userebbe già chiuso/altrui. Duplicato qui, in modo
    // sincrono sul thread Qt (nessuno yield possibile prima di questa
    // riga), cattura una copia indipendente che resta valida qualunque cosa
    // faccia refreshDevices() nel frattempo; chiusa dal thread stesso a
    // lavoro finito.
    HANDLE radioDup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), record.radio, GetCurrentProcess(), &radioDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        emit managerError(QStringLiteral("Impossibile duplicare l'handle del radio Bluetooth"));
        return;
    }

    WindowsBluetoothManager *self = this;
    // BluetoothSetServiceState può bloccare per la negoziazione della
    // connessione: eseguita su un thread dedicato per non bloccare la UI,
    // stesso contratto "asincrono" di BlueZManager/AppleBluetoothManager.
    QThread *thread = QThread::create([self, record, objectPath, radioDup]() mutable {
        const DWORD result = BluetoothSetServiceState(radioDup, &record.info,
                                                        &AudioSinkServiceClass_UUID, BLUETOOTH_SERVICE_ENABLE);
        CloseHandle(radioDup);
        const bool connected = (result == ERROR_SUCCESS);
        QMetaObject::invokeMethod(self, [self, objectPath, connected, result]() {
            if (!connected)
                emit self->managerError(QStringLiteral("Connessione fallita (errore %1)").arg(result));
            self->refreshDevices();
            emit self->deviceConnectionChanged(objectPath, connected);
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void WindowsBluetoothManager::disconnectDevice(const QString &objectPath)
{
    Impl::DeviceRecord record;
    {
        QMutexLocker locker(&d->mutex);
        if (!d->recordsByAddress.contains(objectPath)) {
            locker.unlock();
            emit managerError(QStringLiteral("Dispositivo non trovato: %1").arg(objectPath));
            return;
        }
        record = d->recordsByAddress.value(objectPath);
    }

    // Vedi la stessa nota in connectDevice: duplica l'handle per non
    // dipendere dalla sua vita in Impl::recordsByAddress durante l'attesa
    // asincrona.
    HANDLE radioDup = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), record.radio, GetCurrentProcess(), &radioDup, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        emit managerError(QStringLiteral("Impossibile duplicare l'handle del radio Bluetooth"));
        return;
    }

    WindowsBluetoothManager *self = this;
    QThread *thread = QThread::create([self, record, objectPath, radioDup]() mutable {
        BluetoothSetServiceState(radioDup, &record.info, &AudioSinkServiceClass_UUID, BLUETOOTH_SERVICE_DISABLE);
        CloseHandle(radioDup);
        QMetaObject::invokeMethod(self, [self, objectPath]() {
            self->refreshDevices();
            emit self->deviceConnectionChanged(objectPath, false);
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}
