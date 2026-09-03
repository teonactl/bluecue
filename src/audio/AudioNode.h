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
        Source,         // stream applicativo (musica, voce, ecc.)
        // Stream di riproduzione di un'altra applicazione già in esecuzione
        // sul sistema (es. Firefox, VLC) — media.class "Stream/Output/Audio"
        // in PipeWire, distinto da Source (le nostre tracce file, sempre
        // "Audio/Source") perché richiede un trattamento diverso:
        // catturabile nella playlist come sorgente "sposta l'audio qui"
        // (vedi PatchManager::addAppStreamCue), non riproducibile/
        // controllabile da noi come un file.
        AppStream
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
    // Ritardo (ms) applicato all'audio diretto verso questo sink, per
    // compensare la latenza aggiuntiva di un altro output (tipicamente un
    // sink Bluetooth, che aggiunge trasporto+decodifica A2DP) quando la
    // stessa traccia suona su entrambi contemporaneamente — richiesto
    // esplicitamente dall'utente. Non letto da PipeWire: è un parametro
    // applicativo, valorizzato/persistito da PatchManager (vedi
    // PatchManager::setOutputDelayMs) e realizzato da
    // PipeWireEngine tramite un filtro di ritardo interposto (vedi
    // PipeWireEngine::Impl::DelayFilter). 0 = nessun ritardo (passthrough
    // diretto, comportamento storico).
    int delayMs = 0;
    // PID del processo proprietario (PW_KEY_APP_PROCESS_ID), solo per
    // Kind::AppStream — 0 se non esposto dal client. Usato da
    // PatchManager per "seguire" lo stream di un'app quando il suo nodeId
    // sparisce e ne appare uno nuovo dallo stesso processo: comune con
    // client che passano da pipewire-pulse (es. Firefox, "client.api" =
    // "pipewire-pulse"), che possono ricreare il proprio stream più volte
    // durante una riproduzione continua e ininterrotta dal punto di vista
    // dell'utente — scoperto testando la cattura di un video YouTube in
    // Firefox, dove il nodeId scelto inizialmente spariva in pochi istanti
    // pur senza alcuna interruzione udibile della riproduzione.
    uint32_t appProcessId = 0;
};

Q_DECLARE_METATYPE(AudioNode)
