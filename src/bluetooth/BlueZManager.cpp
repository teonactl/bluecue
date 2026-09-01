#include "BlueZManager.h"

#include <algorithm>

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDebug>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QVariantMap>

namespace {

// UUID standard del profilo A2DP Sink (dispositivo che RICEVE audio, es.
// una cassa/cuffia) — è quello che ci interessa come output. Confronto
// case-insensitive: BlueZ lo espone in minuscolo, ma non è garantito.
const QString kA2dpSinkUuid = QStringLiteral("0000110b-0000-1000-8000-00805f9b34fb");

QStringList extractStringList(const QVariant &value)
{
    if (value.canConvert<QStringList>())
        return value.toStringList();

    QStringList result;
    if (value.canConvert<QDBusArgument>()) {
        QDBusArgument arg = value.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QString item;
            arg >> item;
            result << item;
        }
        arg.endArray();
    }
    return result;
}

} // namespace

struct BlueZManager::Impl
{
    QDBusConnection bus = QDBusConnection::systemBus();
    QVector<BluetoothDevice> devices;
};

BlueZManager::BlueZManager(QObject *parent)
    : BluetoothManager(parent)
    , d(std::make_unique<Impl>())
{
}

BlueZManager::~BlueZManager() = default;

bool BlueZManager::refreshDevices()
{
    if (!d->bus.isConnected()) {
        emit managerError(QStringLiteral("Impossibile connettersi al system bus D-Bus"));
        return false;
    }

    QDBusMessage call = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"), QStringLiteral("/"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    QDBusMessage reply = d->bus.call(call);

    if (reply.type() == QDBusMessage::ErrorMessage) {
        emit managerError(QStringLiteral("GetManagedObjects fallita: %1").arg(reply.errorMessage()));
        return false;
    }
    if (reply.arguments().isEmpty()) {
        emit managerError(QStringLiteral("Risposta GetManagedObjects vuota"));
        return false;
    }

    // a{oa{sa{sv}}}: object path -> (interface name -> (property name ->
    // value)). Estratto con l'operator>> generico di Qt per QMap (ricorsivo,
    // supporta QVariantMap per a{sv} nativamente) invece di un parsing
    // manuale con beginMap/beginMapEntry: quel codice, con la combinazione
    // di Qt/ambiente di questa macchina, restituiva silenziosamente chiavi
    // vuote senza mai avanzare lo stream (diagnosticato con logging
    // temporaneo — beginMapEntry() seguito da una lettura di tipo base
    // tornava sempre vuoto, indipendentemente dal tipo C++ richiesto).
    // L'estrazione tipizzata nativa di Qt evita del tutto quel codice a
    // basso livello.
    using InterfacesAndProperties = QMap<QString, QVariantMap>;
    QMap<QDBusObjectPath, InterfacesAndProperties> managedObjects;
    QDBusArgument arg = reply.arguments().at(0).value<QDBusArgument>();
    arg >> managedObjects;

    QVector<BluetoothDevice> newDevices;
    for (auto objIt = managedObjects.constBegin(); objIt != managedObjects.constEnd(); ++objIt) {
        const InterfacesAndProperties &interfaces = objIt.value();
        if (!interfaces.contains(QLatin1String("org.bluez.Device1")))
            continue;

        const QVariantMap &deviceProps = interfaces.value(QLatin1String("org.bluez.Device1"));

        BluetoothDevice dev;
        dev.objectPath = objIt.key().path();
        dev.address = deviceProps.value(QStringLiteral("Address")).toString();
        dev.name = deviceProps.value(QStringLiteral("Name")).toString();
        if (dev.name.isEmpty())
            dev.name = deviceProps.value(QStringLiteral("Alias")).toString();
        if (dev.name.isEmpty())
            dev.name = dev.address;
        dev.paired = deviceProps.value(QStringLiteral("Paired")).toBool();
        dev.connected = deviceProps.value(QStringLiteral("Connected")).toBool();

        // org.bluez.Battery1 è un'interfaccia separata sullo STESSO object
        // path del dispositivo (non un oggetto figlio), esposta solo dai
        // dispositivi che supportano il Battery Service.
        if (interfaces.contains(QLatin1String("org.bluez.Battery1"))) {
            dev.batteryPercentage = interfaces.value(QLatin1String("org.bluez.Battery1"))
                                         .value(QStringLiteral("Percentage")).toInt();
        }

        const QStringList uuids = extractStringList(deviceProps.value(QStringLiteral("UUIDs")));
        for (const QString &uuid : uuids) {
            if (uuid.compare(kA2dpSinkUuid, Qt::CaseInsensitive) == 0) {
                dev.isAudioSink = true;
                break;
            }
        }

        // Solo dispositivi già accoppiati: quelli in scoperta pura (non
        // accoppiati) non sono ancora utilizzabili come output stabile.
        if (dev.paired)
            newDevices.append(dev);
    }

    // Diff con lo stato precedente per emettere deviceAdded/deviceRemoved
    // solo per i cambiamenti reali (la UI può ascoltarli per aggiornamenti
    // incrementali oltre al deviceModel completo).
    QVector<BluetoothDevice> oldDevices = d->devices;
    d->devices = newDevices;

    for (const BluetoothDevice &oldDev : oldDevices) {
        const bool stillPresent = std::any_of(newDevices.cbegin(), newDevices.cend(),
            [&](const BluetoothDevice &d) { return d.objectPath == oldDev.objectPath; });
        if (!stillPresent)
            emit deviceRemoved(oldDev.objectPath);
    }
    for (const BluetoothDevice &newDev : newDevices) {
        const bool existedBefore = std::any_of(oldDevices.cbegin(), oldDevices.cend(),
            [&](const BluetoothDevice &d) { return d.objectPath == newDev.objectPath; });
        if (!existedBefore)
            emit deviceAdded(newDev);
    }

    emit devicesChanged();
    return true;
}

QVector<BluetoothDevice> BlueZManager::devices() const
{
    return d->devices;
}

QVariantList BlueZManager::deviceModel() const
{
    QVariantList list;
    list.reserve(d->devices.size());
    for (const BluetoothDevice &dev : d->devices) {
        QVariantMap entry;
        entry.insert(QStringLiteral("objectPath"), dev.objectPath);
        entry.insert(QStringLiteral("address"), dev.address);
        entry.insert(QStringLiteral("name"), dev.name);
        entry.insert(QStringLiteral("paired"), dev.paired);
        entry.insert(QStringLiteral("connected"), dev.connected);
        entry.insert(QStringLiteral("isAudioSink"), dev.isAudioSink);
        entry.insert(QStringLiteral("batteryPercentage"), dev.batteryPercentage);
        list.append(entry);
    }
    return list;
}

void BlueZManager::connectDevice(const QString &objectPath)
{
    auto *iface = new QDBusInterface(QStringLiteral("org.bluez"), objectPath,
                                      QStringLiteral("org.bluez.Device1"), d->bus, this);
    if (!iface->isValid()) {
        emit managerError(QStringLiteral("Interfaccia D-Bus non valida per %1").arg(objectPath));
        iface->deleteLater();
        return;
    }

    // Connect() su BlueZ può richiedere diversi secondi (negoziazione A2DP):
    // chiamata asincrona per non bloccare il thread Qt/UI.
    QDBusPendingCall pending = iface->asyncCall(QStringLiteral("Connect"));
    auto *watcher = new QDBusPendingCallWatcher(pending, iface);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, objectPath, iface](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        const bool ok = !reply.isError();
        if (!ok) {
            emit managerError(QStringLiteral("Connessione a %1 fallita: %2")
                                   .arg(objectPath, reply.error().message()));
        } else {
            for (BluetoothDevice &dev : d->devices) {
                if (dev.objectPath == objectPath) {
                    dev.connected = true;
                    break;
                }
            }
            emit devicesChanged();
        }
        emit deviceConnectionChanged(objectPath, ok);
        w->deleteLater();
        iface->deleteLater();
    });
}

void BlueZManager::disconnectDevice(const QString &objectPath)
{
    auto *iface = new QDBusInterface(QStringLiteral("org.bluez"), objectPath,
                                      QStringLiteral("org.bluez.Device1"), d->bus, this);
    if (!iface->isValid()) {
        emit managerError(QStringLiteral("Interfaccia D-Bus non valida per %1").arg(objectPath));
        iface->deleteLater();
        return;
    }

    QDBusPendingCall pending = iface->asyncCall(QStringLiteral("Disconnect"));
    auto *watcher = new QDBusPendingCallWatcher(pending, iface);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, objectPath, iface](QDBusPendingCallWatcher *w) {
        QDBusPendingReply<> reply = *w;
        const bool ok = !reply.isError();
        if (!ok) {
            emit managerError(QStringLiteral("Disconnessione da %1 fallita: %2")
                                   .arg(objectPath, reply.error().message()));
        } else {
            for (BluetoothDevice &dev : d->devices) {
                if (dev.objectPath == objectPath) {
                    dev.connected = false;
                    break;
                }
            }
            emit devicesChanged();
        }
        emit deviceConnectionChanged(objectPath, !ok);
        w->deleteLater();
        iface->deleteLater();
    });
}
