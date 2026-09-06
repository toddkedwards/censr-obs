; Inno Setup script for Censr Redaction (OBS plugin).
; Build with:  iscc packaging\censr.iss   (after running tools/package.py, which
; stages the plugin under dist/stage/censr). Produces dist/censr-<ver>-installer.exe
; that installs into %ProgramData%\obs-studio\plugins\censr.

#define MyAppName "Censr Redaction for OBS"
#define MyAppVersion "0.1.1"
#define MyPublisher "Todd Edwards"

[Setup]
AppId={{7C0C6E9A-3E1D-4C2B-9A5F-CENSR0000001}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyPublisher}
DefaultDirName={commonappdata}\obs-studio\plugins\censr
DisableDirPage=yes
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
OutputDir=..\dist
OutputBaseFilename=censr-{#MyAppVersion}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Files]
Source: "..\dist\stage\censr\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Messages]
WelcomeLabel2=This will install the Censr Redaction filter into OBS Studio.%n%nClose OBS Studio before continuing. After install, add the "Censr Redaction" filter to any source.
