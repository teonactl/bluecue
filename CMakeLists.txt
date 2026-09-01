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
# CoreAudioEngine.h) usa solo framework di sistema Apple, e quello Windows
# (WasapiEngine/WindowsBluetoothManager, vedi src/audio/WasapiEngine.h)
# solo componenti di sistema Windows (WASAPI, Media Foundation, Bluetooth
# classico via bluetoothapis.h) — nessuna dipendenza esterna da installare
# su nessuno dei due.

if(APPLE)
    find_package(Qt6 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
elseif(WIN32)
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
elseif(WIN32)
    list(APPEND APP_SOURCES
        src/audio/WasapiEngine.cpp
        src/audio/WasapiEngine.h
        src/bluetooth/WindowsBluetoothManager.cpp
        src/bluetooth/WindowsBluetoothManager.h
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

if(WIN32)
    # Icona dell'eseguibile: icon.ico va generato dalla CI (da res/icon.svg)
    # PRIMA della vera compilazione, ma non serve già al configure — il
    # file .rc viene compilato solo in fase di build. Percorso relativo
    # risolto dal compilatore risorse rispetto alla cartella di bluecue.rc
    # stesso, quindi icon.ico va generato dentro res/, non altrove.
    target_sources(bluecue PRIVATE res/bluecue.rc)
endif()

# WIN32_EXECUTABLE (niente console dietro la finestra) e MACOSX_BUNDLE
# (vero .app invece di un binario nudo, necessario per macdeployqt/DMG) —
# proprietà ignorate silenziosamente sulle piattaforme dove non si
# applicano, quindi impostate qui una volta sola invece che per-piattaforma.
set_target_properties(bluecue PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

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
    set_target_properties(bluecue PROPERTIES
        OUTPUT_NAME "bluecue-app"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_SOURCE_DIR}/res/Info.plist.in"
        MACOSX_BUNDLE_ICON_FILE icon
    )

    # icon.icns va generato dalla CI (da res/icon.svg) PRIMA del configure,
    # a differenza di icon.ico: MACOSX_PACKAGE_LOCATION è letta al
    # configure, non al build — se il file non esiste ancora qui, va
    # semplicemente saltato (bundle senza icona custom) invece di rompere
    # la build in locale, dove nessuno lo genera mai.
    set(_MACOS_ICON "${CMAKE_SOURCE_DIR}/res/icon.icns")
    if(EXISTS "${_MACOS_ICON}")
        set_source_files_properties("${_MACOS_ICON}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        target_sources(bluecue PRIVATE "${_MACOS_ICON}")
    endif()
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
elseif(WIN32)
    target_link_libraries(bluecue PRIVATE
        ole32       # CoInitializeEx/CoCreateInstance/PropVariantClear (WasapiEngine)
        mfplat      # MFStartup/MFShutdown/MFCreateMediaType
        mfreadwrite # MFCreateSourceReaderFromURL
        mfuuid      # costanti MF_MT_*/MFAudioFormat_Float
        Bthprops    # BluetoothFind*/BluetoothSetServiceState (WindowsBluetoothManager)
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
