; Inno Setup script for Inventory Manager. Built by build.bat (needs Inno Setup 6: https://jrsoftware.org/isdl.php).
; Installs for the current user by default (no administrator rights needed); the setup
; wizard also offers "install for all users".

#define AppName "Inventory Manager"
#define AppVersion "2.3.0"
#define AppExe "InventoryManager.exe"
; Overridable so test builds can install side by side without touching a real installation.
#ifndef AppGuid
  #define AppGuid "{{00DB0E25-8CD3-4641-9599-EC7D1561E20C}"
#endif

[Setup]
AppId={#AppGuid}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
OutputDir=..\build\installer
OutputBaseFilename=InventoryManager-Setup-{#AppVersion}
SetupIconFile=..\packaging\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
; Asks the user to close the app if it's running during install or uninstall.
AppMutex=InventoryManagerSingleInstance
CloseApplications=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"

[Files]
Source: "..\build\{#AppExe}"; DestDir: "{app}"; Flags: ignoreversion
; Starter data: imported automatically the first time the app runs.
Source: "..\inventory_management.xlsx"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#AppExe}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent

; Your inventory data (in %LOCALAPPDATA%\Inventory Manager) is deliberately left in place on
; uninstall, so reinstalling or upgrading never loses it.
