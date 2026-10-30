; Inno Setup script for Visual Notepad. Build with:
;   iscc /DAppVersion=0.0.6 /DSourceDir=<deployed folder> /DOutputDir=<dir> visnotepad.iss
#ifndef AppVersion
  #define AppVersion "0.0.6"
#endif
#ifndef SourceDir
  #error SourceDir must point at the deployed application folder
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif

[Setup]
AppId={{6B0D3E1E-4C0B-4C3A-9D5C-5E1B7B8E2A71}
AppName=Visual Notepad
AppVersion={#AppVersion}
AppPublisher=Hung-Tien Huang
AppPublisherURL=https://github.com/busFred/visual-notepad
AppSupportURL=https://github.com/busFred/visual-notepad/issues
DefaultDirName={autopf}\Visual Notepad
DefaultGroupName=Visual Notepad
DisableProgramGroupPage=yes
LicenseFile={#SourceDir}\licenses\LICENSE
SetupIconFile=visnotepad.ico
UninstallDisplayIcon={app}\visnotepad.exe
UninstallDisplayName=Visual Notepad
OutputDir={#OutputDir}
OutputBaseFilename=visnotepad-{#AppVersion}-windows-x64-setup
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Per-user by default; the wizard offers an all-users install.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
WizardStyle=modern

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\Visual Notepad"; Filename: "{app}\visnotepad.exe"
Name: "{autodesktop}\Visual Notepad"; Filename: "{app}\visnotepad.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\visnotepad.exe"; Description: "{cm:LaunchProgram,Visual Notepad}"; Flags: nowait postinstall skipifsilent
