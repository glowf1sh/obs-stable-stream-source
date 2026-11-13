; Windows installer for Stable Stream Source
; Copyright (C) 2026 Glowfish GmbH & Co. KG
;
; Installs into the OBS Studio folder. Running a newer installer updates the
; plugin, running it again on an installed system offers update or removal.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\release\RelWithDebInfo"
#endif

[Setup]
AppId={{6F3A2C1E-8B4D-4E7A-9C25-5D1F0B8E3A47}
AppName=Stable Stream Source for OBS Studio
AppVersion={#AppVersion}
AppVerName=Stable Stream Source {#AppVersion}
AppPublisher=Glowfish GmbH & Co. KG
AppPublisherURL=https://www.glowfish.de
AppSupportURL=https://github.com/glowf1sh/obs-stable-stream-source
AppUpdatesURL=https://github.com/glowf1sh/obs-stable-stream-source/releases
AppCopyright=Copyright (C) 2026 Glowfish GmbH & Co. KG
VersionInfoVersion={#AppVersion}
DefaultDirName={code:ObsDir}
DirExistsWarning=no
AppendDefaultDirName=no
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
OutputDir=..\release
OutputBaseFilename=obs-stable-stream-source-{#AppVersion}-windows-x64-installer
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=no
UninstallDisplayName=Stable Stream Source for OBS Studio
UninstallFilesDir={app}\obs-plugins\64bit\obs-stable-stream-source-uninstall
WizardStyle=modern
Compression=lzma2
SolidCompression=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"; InfoBeforeFile: "info-en.txt"
Name: "de"; MessagesFile: "compiler:Languages\German.isl"; InfoBeforeFile: "info-de.txt"

[CustomMessages]
en.NoObs=OBS Studio was not found in the selected folder. Please choose the OBS Studio installation folder (it contains bin\64bit\obs64.exe).
de.NoObs=OBS Studio wurde im gewählten Ordner nicht gefunden. Bitte den Installationsordner von OBS Studio wählen (er enthält bin\64bit\obs64.exe).
en.AlreadyInstalled=Stable Stream Source %1 is already installed.%n%nYes: update to version %2%nNo: uninstall%nCancel: exit
en.ObsRunning=OBS Studio is still running. Please close OBS Studio completely and click Retry.
de.ObsRunning=OBS Studio läuft noch. Bitte OBS Studio vollständig beenden und auf „Wiederholen“ klicken.
de.AlreadyInstalled=Stable Stream Source %1 ist bereits installiert.%n%nJa: auf Version %2 aktualisieren%nNein: deinstallieren%nAbbrechen: beenden

[Files]
Source: "{#SourceDir}\obs-stable-stream-source\bin\64bit\obs-stable-stream-source.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion restartreplace uninsrestartdelete
Source: "{#SourceDir}\obs-stable-stream-source\data\*"; DestDir: "{app}\data\obs-plugins\obs-stable-stream-source"; Flags: ignoreversion recursesubdirs createallsubdirs

[UninstallDelete]
Type: dirifempty; Name: "{app}\data\obs-plugins\obs-stable-stream-source"

[Code]
const
  UninstKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6F3A2C1E-8B4D-4E7A-9C25-5D1F0B8E3A47}_is1';

function ProcessRunning(Name: String): Boolean;
var
  Wmi, Found: Variant;
begin
  Result := False;
  try
    Wmi := CreateOleObject('WbemScripting.SWbemLocator').ConnectServer('.', 'root\CIMV2');
    Found := Wmi.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + Name + '''');
    Result := Found.Count > 0;
  except
  end;
end;

function ObsRunning(): Boolean;
begin
  Result := ProcessRunning('obs64.exe') or ProcessRunning('obs-browser-page.exe');
end;

{ Waits until OBS Studio is closed. Returns False if the user cancels. }
function WaitForObs(): Boolean;
begin
  Result := True;
  while ObsRunning() do
    if SuppressibleMsgBox(CustomMessage('ObsRunning'), mbError, MB_RETRYCANCEL, IDCANCEL) = IDCANCEL then
    begin
      Result := False;
      exit;
    end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not WaitForObs() then
    Result := CustomMessage('ObsRunning');
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObs();
end;

function ObsDir(Param: String): String;
begin
  if not RegQueryStringValue(HKLM64, 'SOFTWARE\OBS Studio', '', Result) then
    Result := ExpandConstant('{commonpf64}\obs-studio');
end;

function InitializeSetup(): Boolean;
var
  Installed, Uninstaller: String;
  Code: Integer;
begin
  Result := True;
  if not RegQueryStringValue(HKLM64, UninstKey, 'DisplayVersion', Installed) then
    exit;
  case SuppressibleMsgBox(FmtMessage(CustomMessage('AlreadyInstalled'), [Installed, '{#AppVersion}']),
                          mbConfirmation, MB_YESNOCANCEL, IDYES) of
    IDYES:
      Result := True;
    IDNO:
      begin
        Result := False;
        if RegQueryStringValue(HKLM64, UninstKey, 'UninstallString', Uninstaller) then
          Exec(RemoveQuotes(Uninstaller), '', '', SW_SHOWNORMAL, ewWaitUntilTerminated, Code);
      end;
  else
    Result := False;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if (CurPageID = wpSelectDir) and not FileExists(AddBackslash(WizardDirValue) + 'bin\64bit\obs64.exe') then
  begin
    MsgBox(CustomMessage('NoObs'), mbError, MB_OK);
    Result := False;
  end;
end;
