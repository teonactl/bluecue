#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVector>
#include <QMetaType>

// Rappresenta un dispositivo Bluetooth audio, indipendentemente dal backend
// di sistema che lo espone (BlueZ su Linux, IOBluetooth su macOS, WinRT
// Bluetooth su Windows) — limitato ai campi che ci servono in UI.
struct BluetoothDevice
{
    // Identificatore stabile per il backend corrente: su Linux il D-Bus
    // object path (es. /org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF), su altri
    // backend un identificatore equivalente specifico della piattaforma —
    // il resto dell'app lo tratta come una chiave opaca, mai la interpreta.
    QString objectPath;
    QString address;     // MAC, es. AA:BB:CC:DD:EE:FF
    QString name;
    bool paired = false;
    bool connected = false;
    bool isAudioSink = false; // true se espone il profilo A2DP sink
    // Percentuale batteria (0-100) se il backend la espone per questo
    // dispositivo (non tutte le casse/cuffie lo supportano, e non tutti i
    // backend hanno un modo di leggerla) — -1 = sconosciuta/non disponibile.
    int batteryPercentage = -1;
};

// Interfaccia astratta del backend Bluetooth: scoperta, connessione e
// monitoraggio dei dispositivi audio accoppiati, senza sapere se dietro
// c'è BlueZ via D-Bus (Linux, vedi BlueZManager), IOBluetooth (macOS) o
// un futuro backend WinRT (Windows).
//
// Estratta a partire dall'API pubblica di BlueZManager — l'unico backend
// esistente finché il porting multipiattaforma non introduce gli altri.
class BluetoothManager : public QObject
{
    Q_OBJECT
    // Vista QML-friendly di devices(): un QVariantList di QVariantMap
    // (objectPath/address/name/paired/connected/isAudioSink/batteryPercentage).
    Q_PROPERTY(QVariantList deviceModel READ deviceModel NOTIFY devicesChanged)

public:
    explicit BluetoothManager(QObject *parent = nullptr) : QObject(parent) {}
    ~BluetoothManager() override = default;

    // Interroga il sistema e popola la lista dei dispositivi accoppiati.
    Q_INVOKABLE virtual bool refreshDevices() = 0;

    Q_INVOKABLE virtual QVector<BluetoothDevice> devices() const = 0;

    virtual QVariantList deviceModel() const = 0;

    // Avvia la connessione a un dispositivo già accoppiato (asincrona).
    Q_INVOKABLE virtual void connectDevice(const QString &objectPath) = 0;

    // Disconnette un dispositivo attualmente connesso.
    Q_INVOKABLE virtual void disconnectDevice(const QString &objectPath) = 0;

signals:
    void deviceAdded(const BluetoothDevice &device);
    void deviceRemoved(const QString &objectPath);
    void deviceConnectionChanged(const QString &objectPath, bool connected);
    void managerError(const QString &message);
    void devicesChanged();
};

Q_DECLARE_METATYPE(BluetoothDevice)
