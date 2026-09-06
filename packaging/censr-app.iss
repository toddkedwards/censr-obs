; Inno Setup script for the Censr standalone app + virtual camera.
; Build with:  iscc packaging\censr-app.iss   (after tools/package.py --app, which
; stages the app under dist/stage-app/censr-app and signs the binaries).
; Produces dist/censr-vcam-<ver>-windows-x64-installer.exe, which installs to
; Program Files\Censr and registers the MF + DirectShow virtual cameras.

#define MyAppName "Censr Virtual Camera"
#define MyAppVersion "0.1.1"
#define MyPublisher "Todd Edwards"

[Setup]
AppId={{7C0C6E9A-3E1D-4C2B-9A5F-CENSR0000002}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyPublisher}
DefaultDirName={autopf}\Censr
DisableProgramGroupPage=yes
UninstallDisplayName={#MyAppName}
OutputDir=..\dist
OutputBaseFilename=censr-vcam-{#MyAppVersion}-windows-x64-installer
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Registering a system camera + COM servers needs administrator rights.
PrivilegesRequired=admin
WizardStyle=modern

[Files]
Source: "..\dist\stage-app\censr-app\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Icons]
Name: "{commonprograms}\Censr"; Filename: "{app}\censr-host.exe"

[Run]
; Register + create the Media Foundation virtual camera (modern apps). This also
; registers the media-source COM server (censr-mfcam.dll).
Filename: "{app}\censr-vcam.exe"; Parameters: "install ""{app}\censr-mfcam.dll"""; StatusMsg: "Registering Censr Virtual Camera (Media Foundation)..."; Flags: runhidden
; Register the DirectShow source filter (legacy apps).
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\censr-dshowcam.dll"""; StatusMsg: "Registering Censr Virtual Camera (DirectShow)..."; Flags: runhidden
; Offer to launch the capture app.
Filename: "{app}\censr-host.exe"; Description: "Launch Censr now"; Flags: postinstall nowait skipifsilent

[UninstallRun]
Filename: "{app}\censr-vcam.exe"; Parameters: "uninstall ""{app}\censr-mfcam.dll"""; Flags: runhidden; RunOnceId: "CensrMF"
Filename: "{sys}\regsvr32.exe"; Parameters: "/u /s ""{app}\censr-dshowcam.dll"""; Flags: runhidden; RunOnceId: "CensrDShow"

[Messages]
WelcomeLabel2=This installs the Censr capture app and the "Censr Virtual Camera".%n%nAfter install, run Censr, choose a screen or window to redact, then pick "Censr Virtual Camera" as your webcam in Zoom, Meet, Teams, Discord, and more.
