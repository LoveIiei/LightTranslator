; LightTranslate installer (Inno Setup 6).
; Built by tools\build-release.ps1, which passes /DAppVersion, /DSourceExe and /DOutputDir.
;
; Per-user install (no admin / UAC): the app keeps everything in HKCU and the user's
; Credential Manager, so a per-machine install would gain nothing.

#ifndef SourceExe
  #define SourceExe "..\build\dist\LightTranslate.exe"
#endif
#ifndef AppVersion
  #define AppVersion GetVersionNumbersString(SourceExe)
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

#define AppName "LightTranslate"
#define AppExe "LightTranslate.exe"
#define AppUrl "https://github.com/LoveIiei/LightTranslator"
; Must match AppInfo::kWindowClass / kRunKey / kRegistryKey in src/app/AppInfo.h.
#define AppWindowClass "LightTranslate.AppWindow"
#define RunKey "Software\Microsoft\Windows\CurrentVersion\Run"
#define SettingsKey "Software\LightTranslate"

[Setup]
; Never change AppId: it is how upgrades find the existing installation.
AppId={{B7A6E5C2-3F1D-4E8A-9C47-5D2E1F0A8B63}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppName}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}/issues
AppUpdatesURL={#AppUrl}/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
OutputDir={#OutputDir}
OutputBaseFilename={#AppName}-{#AppVersion}-setup
SetupIconFile=..\res\app.ico
UninstallDisplayIcon={app}\{#AppExe}
UninstallDisplayName={#AppName}
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes

[Tasks]
Name: "autostart"; Description: "Start {#AppName} when I sign in to Windows"; GroupDescription: "Options:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Options:"; Flags: unchecked

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExe}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExe}"; Tasks: desktopicon

[Registry]
; Same value the app's own "Start with Windows" switch writes, so both stay in sync.
Root: HKCU; Subkey: "{#RunKey}"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExe}"""; Tasks: autostart

[Run]
Filename: "{app}\{#AppExe}"; Description: "Launch {#AppName} now"; Flags: nowait postinstall skipifsilent

[Code]
const
  WM_CLOSE = $0010;

// Ask a running copy to quit the normal way (WM_CLOSE -> WM_DESTROY -> Shutdown), so the
// tray icon and hotkey are cleaned up and the exe can be replaced or removed.
procedure CloseRunningApp;
var
  Wnd: HWND;
  I: Integer;
begin
  Wnd := FindWindowByClassName('{#AppWindowClass}');
  if Wnd = 0 then
    exit;
  PostMessage(Wnd, WM_CLOSE, 0, 0);
  for I := 1 to 50 do
  begin
    Sleep(100);
    if FindWindowByClassName('{#AppWindowClass}') = 0 then
      break;
  end;
  Sleep(500); // let the process finish exiting and release the exe
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningApp;
  Result := '';
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  Existing: String;
begin
  // Autostart was switched on from inside the app (possibly for an older or portable copy):
  // point it at the installed exe so it does not start a stale path.
  if (CurStep = ssPostInstall) and not WizardIsTaskSelected('autostart') then
    if RegQueryStringValue(HKCU, '{#RunKey}', '{#AppName}', Existing) then
      RegWriteStringValue(HKCU, '{#RunKey}', '{#AppName}', '"' + ExpandConstant('{app}\{#AppExe}') + '"');
end;

function InitializeUninstall(): Boolean;
begin
  CloseRunningApp;
  Result := True;
end;

procedure DeleteCredential(const Target: String);
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{sys}\cmdkey.exe'), '/delete:' + Target, '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Existing: String;
begin
  if CurUninstallStep <> usUninstall then
    exit;

  // Remove autostart only if it points at this installation (not at a separate portable copy).
  if RegQueryStringValue(HKCU, '{#RunKey}', '{#AppName}', Existing) then
    if Pos(Lowercase(ExpandConstant('{app}')), Lowercase(Existing)) > 0 then
      RegDeleteValue(HKCU, '{#RunKey}', '{#AppName}');

  // Settings and API keys are kept unless the user asks (a silent uninstall keeps them).
  if not UninstallSilent and
     (MsgBox('Also remove your {#AppName} settings and the API keys saved in Credential Manager?',
             mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES) then
  begin
    RegDeleteKeyIncludingSubkeys(HKCU, '{#SettingsKey}');
    // Must match SecretStore's TargetName(): "LightTranslate/<provider id>".
    DeleteCredential('{#AppName}/deepl');
    DeleteCredential('{#AppName}/google');
  end;
end;
