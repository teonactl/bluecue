#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QUrl>

#include "audio/AudioEngine.h"
#include "audio/PatchManager.h"
#include "bluetooth/BluetoothManager.h"

// Selezione del backend a compile-time: stessa interfaccia (AudioEngine/
// BluetoothManager, vedi src/audio/AudioEngine.h e
// src/bluetooth/BluetoothManager.h), implementazione diversa per
// piattaforma — PipeWire+BlueZ su Linux, CoreAudio+IOBluetooth su macOS
// (Apple Silicon incluso), WASAPI+Bluetooth classico su Windows.
#if defined(Q_OS_MACOS)
#include "audio/CoreAudioEngine.h"
#include "bluetooth/AppleBluetoothManager.h"
using PlatformAudioEngine = CoreAudioEngine;
using PlatformBluetoothManager = AppleBluetoothManager;
#elif defined(Q_OS_WIN)
#include "audio/WasapiEngine.h"
#include "bluetooth/WindowsBluetoothManager.h"
using PlatformAudioEngine = WasapiEngine;
using PlatformBluetoothManager = WindowsBluetoothManager;
#else
#include "audio/PipeWireEngine.h"
#include "bluetooth/BlueZManager.h"
using PlatformAudioEngine = PipeWireEngine;
using PlatformBluetoothManager = BlueZManager;
#endif

int main(int argc, char *argv[])
{
    // Il file picker (QtQuick.Dialogs FileDialog) risultava pressoché
    // illeggibile: verificato (find sui plugin installati) che questo
    // sistema usa il proprio FileDialog QML integrato di Qt invece del vero
    // selettore nativo KDE, perché QT_QPA_PLATFORMTHEME non era impostato e
    // Qt sceglieva da solo il plugin "kde" invece di quello per i portali
    // xdg-desktop-portal (xdg-desktop-portal-kde risulta comunque attivo e
    // registrato su D-Bus, quindi disponibile). Forzare qui il platform
    // theme "xdgdesktopportal" instrada FileDialog/FolderDialog sul vero
    // dialog di sistema (già temato correttamente da KDE per definizione),
    // invece del fallback QML di Qt che ignora la palette per alcuni
    // Control (stesso bug di fondo già visto altrove in questo progetto).
    // A differenza del QQuickStyle::setStyle("Basic") rimosso sotto, questo
    // NON tocca lo stile/rendering dei nostri Control QtQuick — è un
    // meccanismo completamente separato (integrazione col desktop, non
    // rendering) — va impostato PRIMA di costruire QGuiApplication.
    qputenv("QT_QPA_PLATFORMTHEME", "xdgdesktopportal");

    // NOTA: qui era stato impostato QQuickStyle::setStyle("Basic") per
    // provare a risolvere in un colpo solo i vari bug di testo invisibile
    // incontrati in sessione (vedi PROJECT_STATUS). Sbagliato: "Basic"
    // legge comunque la QPalette di default dell'applicazione, che su
    // questo sistema segue il tema scuro — risultato, gli sfondi dei
    // Control nativi (pulsanti, dialog, spinbox, menu bar) sono diventati
    // scuri mentre il resto dell'app (i Rectangle colorati a mano ovunque,
    // es. PortRow) è rimasto chiaro come da design: contrasto rotto quasi
    // ovunque invece che solo nel pannello Trasforma. **Rimosso**: lo stile
    // nativo di piattaforma va bene com'era, il problema di leggibilità
    // andava risolto puntualmente dov'era (pannello Trasforma, vedi i fix
    // su TransformPanel.qml), non cambiando lo stile globale.
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("BlueCue"));
    app.setOrganizationName(QStringLiteral("BlueCue"));
    // NOTA: rinominare applicationName/organizationName sposta la posizione
    // di QSettings (~/.config/<organization>/<application>.conf su Linux) —
    // le impostazioni salvate con il vecchio nome "btmultizone" (recenti,
    // nickname degli output, preferenza keepalive) non vengono più lette.
    // Accettato: rinomina esplicitamente richiesta dall'utente, progetto
    // ancora in fase di test.
    app.setWindowIcon(QIcon(QStringLiteral(":/qt/qml/BlueCue/res/icon.svg")));

    // NOTA: qui era stato provato anche un app.setPalette(...) esplicito
    // per il file picker di QtQuick.Dialogs (FileDialog, un componente di
    // Qt stesso — non nostro, quindi non gli si può applicare il fix
    // puntuale a contentItem usato per i Control scritti in questo
    // progetto). **Rimosso**: verificato dall'utente che non ha avuto
    // alcun effetto (stesso testo pressoché invisibile nel listato file),
    // quindi teneva solo rischio (poteva rompere il contrasto di qualche
    // altro dialog nativo non verificato) senza alcun beneficio dimostrato.
    // Il file picker resta un problema aperto, non risolvibile con i mezzi
    // usati finora in questo progetto (vedi PROJECT_STATUS).
    PlatformAudioEngine pwEngine;
    // Prima non era collegato a nulla: qualunque errore dell'engine (file
    // non apribile, link PipeWire fallito, porte non trovate, ...) spariva
    // silenziosamente senza mai comparire in console né in UI.
    QObject::connect(&pwEngine, &AudioEngine::engineError, [](const QString &message) {
        qWarning("AudioEngine: %s", qUtf8Printable(message));
    });
    if (!pwEngine.start()) {
        qWarning("Avvio del motore audio fallito: l'app continua ma senza audio.");
    }

    PlatformBluetoothManager blueZManager;
    PatchManager patchManager(&pwEngine, &blueZManager);
    // DEVE venire dopo aver costruito PatchManager: il suo costruttore
    // collega BlueZManager::devicesChanged a refreshBatteryLevels(), che è
    // il meccanismo che porta la percentuale batteria nella colonna
    // Output. Chiamato prima (come faceva questo codice), il primo
    // refreshDevices() emette devicesChanged mentre PatchManager non esiste
    // ancora — il segnale va perso e la batteria non compare finché
    // l'utente non apre a mano il dialog "Dispositivi Bluetooth" (segnalato
    // dall'utente: "non vedo la batteria").
    blueZManager.refreshDevices();

    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("patchManager"), &patchManager);
    engine.rootContext()->setContextProperty(QStringLiteral("blueZManager"), &blueZManager);

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // ATTENZIONE: NON sostituire con engine.load(QUrl("qrc:/qt/qml/
    // BlueCue/Main.qml")) — provato una volta per un problema di CI (Qt
    // troppo vecchio su Ubuntu apt, richiede 6.5+), ma quel percorso qrc
    // non è quello vero generato da qt_add_qml_module: l'app si rompeva
    // subito ("No such file or directory") su un Qt6 vero e proprio (6.11
    // su questa macchina). loadFromModule() risolve il percorso corretto
    // da solo — il progetto richiede comunque Qt 6.5+ (REQUIRES 6.5 in
    // CMakeLists.txt), quindi qualunque ambiente di build deve già
    // averlo; il problema semmai va risolto nella CI (Qt installato lì),
    // non degradando questa riga.
    engine.loadFromModule("BlueCue", "Main");

    return app.exec();
}
