#pragma once

#include <QString>
#include <cstdint>
#include <cstring>
#include <QMetaType>

// Rappresenta un nodo audio PipeWire (sink fisico, sink virtuale, o sorgente).
// id corrisponde al pw_global id assegnato dal server PipeWire.
struct AudioNode
{
    enum class Kind {
        Unknown,
        PhysicalSink,   // es. speaker Bluetooth A2DP
        VirtualSink,    // sink di zona creato da noi
        Source          // stream applicativo (musica, voce, ecc.)
    };

    uint32_t id = 0;
    QString name;          // node.name di PipeWire
    QString description;   // node.description, più leggibile in UI
    Kind kind = Kind::Unknown;
    bool isBluetooth = false;
    QString bluetoothMac;  // valorizzato solo se isBluetooth == true
    // Percentuale batteria (0-100) se BlueZ la espone per questo dispositivo
    // (org.bluez.Battery1, non tutte le casse la supportano) — -1 =
    // sconosciuta/non disponibile. Non viene da PipeWire: PatchManager la
    // valorizza correlando bluetoothMac con BlueZManager::devices() prima
    // di passare il nodo a PortModel (vedi PatchManager::handleSinkNode).
    int batteryPercentage = -1;
    // Muto (SPA_PROP_mute), letto da PipeWire — solo per i sink Output.
    // Sostituisce un precedente slider di volume (SPA_PROP_channelVolumes):
    // rimosso su richiesta esplicita dell'utente dopo aver verificato che
    // il volume software del nodo agisce solo DENTRO il range già limitato
    // dal mixer di sistema (a monte, non controllabile da qui) — un
    // semplice muto/non muto è l'unico controllo che l'app può offrire in
    // modo affidabile, il volume vero e proprio resta al mixer di sistema.
    bool muted = false;
};

Q_DECLARE_METATYPE(AudioNode)
