# BlueCue

App desktop (Linux, macOS, Windows) per instradare più sorgenti audio — file,
stream di un'altra applicazione (es. Firefox), microfono — verso più output
contemporaneamente (jack del computer, più altoparlanti Bluetooth insieme),
con un'interfaccia a **patch bay** stile mixer e una playlist di cue in
stile QLab/Cuelab.

Nato per un caso d'uso specifico: far suonare la stessa musica su più casse
Bluetooth indipendenti (non un multi-room sincronizzato con lo stesso
stream ovunque) contemporaneamente all'audio interno, con la possibilità di
compensare il ritardo che il Bluetooth introduce rispetto all'uscita
interna.

[![CI](https://github.com/teonactl/bluecue/actions/workflows/ci.yml/badge.svg)](https://github.com/teonactl/bluecue/actions/workflows/ci.yml)

## Cosa fa

- **Patch bay a trascinamento**: colonna sorgenti a sinistra, colonna
  output a destra, si trascina un cavo dal connettore di una sorgente a un
  output per collegarli; trascinare di nuovo lo scollega.
- **Playlist/cue list stile QLab**: sorgenti organizzate come cue
  riproducibili (play/stop, loop count, riproduzione all'indietro,
  rotazione automatica dell'output), con pre-wait/durata/post-wait e
  riproduzione polifonica (più cue insieme).
- **Cattura audio di un'altra applicazione**: sposta lo stream audio di
  un'app già in esecuzione (es. una scheda Firefox che riproduce YouTube)
  dentro la patch bay come se fosse un file, invece di duplicarlo.
- **Bluetooth**: scoperta/connessione/disconnessione dispositivi, stato
  batteria dove disponibile, keepalive automatico (un ping quasi
  inudibile per impedire ad alcune casse di autospegnersi/disconnettersi
  per inattività) e un tasto "identifica" per far suonare fisicamente la
  cassa selezionata.
- **Ritardo di output regolabile, con calibrazione automatica**: il
  Bluetooth introduce latenza rispetto all'audio interno; l'app misura
  acusticamente lo sfasamento reale tra due output (un click di rumore
  bianco + un microfono) e applica in automatico il ritardo necessario per
  farli suonare in sincrono.
- **Progetti salvabili**: routing, cue, ritardi e impostazioni si salvano e
  ricaricano da file.

## Stato per piattaforma

| Piattaforma | Audio | Bluetooth | Stato |
|---|---|---|---|
| Linux | PipeWire (libpipewire nativa) | BlueZ (D-Bus) | Piattaforma di sviluppo principale, testata dal vivo |
| macOS | CoreAudio + AudioQueue/Aggregate Device | IOBluetooth | Compila in CI (Apple Silicon), mai verificata su hardware reale |
| Windows | WASAPI + Media Foundation | API Bluetooth classiche (bluetoothapis.h) | Compila in CI, mai verificata su hardware reale |

I backend macOS e Windows sono scritti seguendo la documentazione ufficiale
delle rispettive API ma — non avendo hardware Apple/Windows a disposizione
in sviluppo — sono verificati solo tramite build automatica su CI, non
tramite un test dal vivo con audio/Bluetooth reali. Funzionalità e stato
dettagliato di ogni feature sono tracciati in [`PROJECT_STATUS.md`](PROJECT_STATUS.md).

## Build dei pacchetti pronti all'uso

Ogni push su `main` pubblica una build automatica (tag `dev-latest`, sempre
sovrascritta, nessuna garanzia di stabilità) nella pagina
[Releases](https://github.com/teonactl/bluecue/releases): AppImage per
Linux, DMG per macOS, zip portatile e installer Inno Setup per Windows.

## Compilare da sorgente

Requisiti comuni: **Qt 6.5+** (Core, Gui, Qml, Quick, QuickControls2) e
**CMake 3.21+**.

### Linux

```sh
# Arch Linux
sudo pacman -S cmake ninja qt6-base qt6-declarative pipewire libsndfile

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/bluecue
```

Dipendenze aggiuntive rispetto a macOS/Windows: `libpipewire-0.3`,
`libsndfile`, Qt6::DBus (per BlueZ).

### macOS

```sh
brew install qt ninja
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$(brew --prefix qt)"
cmake --build build
```

Solo framework di sistema (CoreAudio, AudioToolbox, IOBluetooth,
Foundation), nessuna dipendenza esterna da installare.

### Windows

Installare Qt 6.5+ (con Ninja e i tool MSVC nel `PATH`, es. tramite il
Qt online installer o `aqt`), poi:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Solo componenti di sistema (WASAPI, Media Foundation, API Bluetooth
classiche), nessuna dipendenza esterna da installare.

## Architettura

Interfaccia comune `AudioEngine`/`BluetoothManager` (`src/audio/AudioEngine.h`,
`src/bluetooth/BluetoothManager.h`), un'implementazione diversa per
piattaforma selezionata a compile-time in `src/main.cpp`:

```
PipeWireEngine + BlueZManager           (Linux)
CoreAudioEngine + AppleBluetoothManager (macOS)
WasapiEngine + WindowsBluetoothManager  (Windows)
```

`PatchManager` (`src/audio/PatchManager.*`) orchestra il routing e la cue
list sopra l'engine attivo, senza conoscere quale backend è in uso.
L'interfaccia è in QML (`qml/`), esposta come context property.

Per il dettaglio di ogni feature, le decisioni prese e i bug noti/risolti,
vedi [`PROJECT_STATUS.md`](PROJECT_STATUS.md).

## Licenza

[GNU GPLv3](LICENSE).
