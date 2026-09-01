; BlueCue — Inno Setup installer script
; Uso da CI: ISCC /DMyAppVersion="1.2.3" /DOutputFile="bluecue-windows-setup" installer.iss

#ifndef MyAppVersion
  #define MyAppVersion "0.1.0"
#endif
#ifndef OutputFile
  #define OutputFile "bluecue-windows-setup"
#endif

#define MyAppName      "BlueCue"
#define MyAppExeName   "bluecue.exe"
#define MyAppPublisher "BlueCue"

[Setup]
AppId={{40D5DA1C-D919-4074-B5B6-D4F920340B60}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL=https://github.com/teonactl/bluecue
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
SetupIconFile=res\icon.ico
AllowNoIcons=yes
OutputDir=.
OutputBaseFilename={#OutputFile}
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
DisableProgramGroupPage=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "italian"; MessagesFile: "compiler:Languages\Italian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "dist\BlueCue\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{commondesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
; Redistributable firmato Microsoft, eseguito al posto di copiare a mano
; msvcp140.dll/vcruntime140.dll: alcuni antivirus quarantinano quelle DLL se
; depositate direttamente da un installer non firmato (stesso motivo per
; cui questo file mirror installer.iss di OQL, github.com/teonactl/oql,
; dove questa scelta è già stata testata in produzione).
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; StatusMsg: "Installazione Visual C++ Redistributable..."; Flags: waituntilterminated
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#MyAppName}}"; Flags: nowait postinstall skipifsilent
