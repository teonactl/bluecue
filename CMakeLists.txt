cmake_minimum_required(VERSION 3.21)
project(BlueCue VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ---------------------------------------------------------------------------
# Dipendenze
# ---------------------------------------------------------------------------
# Qt6::DBus e libpipewire/libsndfile servono solo al backend Linux
# (PipeWireEngine/BlueZManager parlano D-Bus e libpipewire). Il backend
# macOS (CoreAudioEngine/AppleBluetoothManager, vedi src/audio/
# CoreAudioEngine.h) usa solo framework di sistema Apple — CoreAudio/
# AudioToolbox per l'audio (incluso il decoding via ExtAudioFile, che
# sostituisce libsndfile) e IOBluetooth per il Bluetooth — nessuna
# dipendenza esterna da installare.

if(APPLE)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2 DBus)
    find_package(PkgConfig REQUIRED)

    # libpipewire espone il proprio .pc file (di solito libpipewire-0.3)
    pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

    # libsndfile per decodificare i file audio (WAV/FLAC/OGG/...) riprodotti
    # come stream di input nella patch bay.
    pkg_check_modules(SNDFILE REQUIRED IMPORTED_TARGET sndfile)
endif()

qt_standard_project_setup(REQUIRES 6.5)

# ---------------------------------------------------------------------------
# Sorgenti
# ---------------------------------------------------------------------------
# AudioEngine/BluetoothManager (src/audio/AudioEngine.h, src/bluetooth/
# BluetoothManager.h) sono le interfacce astratte usate da PatchManager:
# stesse per qualunque piattaforma. Solo l'implementazione concreta cambia.

set(APP_SOURCES
    src/main.cpp
    src/audio/AudioEngine.h
    src/audio/PatchManager.cpp
    src/audio/PatchManager.h
    src/audio/AudioNode.h
    src/bluetooth/BluetoothManager.h
    src/models/PortModel.cpp
    src/models/PortModel.h
)

if(APPLE)
    list(APPEND APP_SOURCES
        src/audio/CoreAudioEngine.cpp
        src/audio/CoreAudioEngine.h
        src/bluetooth/AppleBluetoothManager.mm
        src/bluetooth/AppleBluetoothManager.h
    )
else()
    list(APPEND APP_SOURCES
        src/audio/PipeWireEngine.cpp
        src/audio/PipeWireEngine.h
        src/bluetooth/BlueZManager.cpp
        src/bluetooth/BlueZManager.h
    )
endif()

qt_add_executable(bluecue ${APP_SOURCES})

if(APPLE)
    # qt_add_qml_module (sotto) genera una cartella "BlueCue/" per il
    # modulo QML (dall'URI) nella stessa build dir dove va scritto
    # l'eseguibile "bluecue" — su APFS, il filesystem di default di
    # macOS, i nomi non sono case-sensitive: "BlueCue" e "bluecue"
    # risultano lo STESSO percorso, e il linker fallisce
    # ("ld: open() failed, errno=21", EISDIR — trova una cartella dove si
    # aspettava di scrivere il file) — scoperto dalla CI macOS, mai un
    # problema su Linux (ext4, case-sensitive). Soluzione minima: solo su
    # Apple, il binario prodotto si chiama diversamente dall'URI del
    # modulo quanto basta a non collidere nemmeno ignorando le
    # maiuscole/minuscole.
    set_target_properties(bluecue PROPERTIES OUTPUT_NAME "bluecue-app")
endif()

qt_add_qml_module(bluecue
    URI BlueCue
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/CueList.qml
        qml/PortColumn.qml
        qml/PortRow.qml
        qml/PatchGrid.qml
        qml/TransformPanel.qml
        qml/DeviceSelector.qml
        qml/DurationField.qml
    RESOURCES
        res/icon.svg
)

target_include_directories(bluecue PRIVATE src)

target_link_libraries(bluecue PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
)

if(APPLE)
    target_link_libraries(bluecue PRIVATE
        "-framework CoreAudio"
        "-framework AudioToolbox"
        "-framework CoreFoundation"
        "-framework IOBluetooth"
        "-framework Foundation"
    )
else()
    target_include_directories(bluecue PRIVATE ${PIPEWIRE_INCLUDE_DIRS})
    target_link_libraries(bluecue PRIVATE
        Qt6::DBus
        PkgConfig::PIPEWIRE
        PkgConfig::SNDFILE
    )
    # Su alcune distro pipewire richiede anche libspa in include path separato
    target_compile_options(bluecue PRIVATE ${PIPEWIRE_CFLAGS_OTHER})
endif()
