; Inno Setup script for SnapCap. Built by package.ps1 (run build.ps1 first).
; Installs for the current user only, so there is no administrator prompt, and
; into a folder the user can write to, because snapcap.ini is kept next to the exe.

#define AppName "SnapCap"
#define AppVersion "1.0"

[Setup]
AppId={{DEBA0015-D524-4EEF-95FD-26B801ADADD4}
AppName={#AppName}
AppVersion={#AppVersion}
DefaultDirName={localappdata}\Programs\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir=..\dist
OutputBaseFilename=SnapCap-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\snapcap.exe

[Tasks]
Name: "startup"; Description: "Start SnapCap with Windows"; Flags: unchecked

[Files]
Source: "..\bin\snapcap.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\bin\snapcap-fps.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\snapcap.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; \
    ValueData: """{app}\snapcap.exe"""; Tasks: startup; Flags: uninsdeletevalue

[Run]
Filename: "{app}\snapcap.exe"; Description: "Start SnapCap now"; Flags: postinstall nowait skipifsilent

[Code]
// SnapCap has no window, so the installer can't ask it to close. Stop it before
// files are replaced or removed (its FPS helper exits by itself when it goes).
procedure StopSnapCap;
var
  rc: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM snapcap.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /IM snapcap-fps.exe', '', SW_HIDE, ewWaitUntilTerminated, rc);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopSnapCap;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopSnapCap;
  Result := True;
end;
