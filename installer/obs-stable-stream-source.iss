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
en.FoundTitle=Stable Stream Source is already installed
en.FoundUpdate=Currently installed version: %1%nUpdate to version: %2
en.FoundSame=Version %1 is already installed. There is nothing to update.
en.FoundNewer=A newer version is installed: %1%nThis installer contains version %2.
en.BtnUpdate=Update
en.BtnReinstall=Reinstall
en.BtnDowngrade=Install older version
en.BtnUninstall=Uninstall
en.BtnCancel=Cancel
en.ObsTitle=OBS Studio is still running
en.ObsText=OBS Studio has to be closed before the plugin can be installed or removed. Running streams and recordings are not stopped automatically, OBS asks first.
en.BtnCloseObs=Close OBS Studio
en.BtnRetry=Check again
en.ObsRunning=OBS Studio is still running.
de.FoundTitle=Stable Stream Source ist bereits installiert
de.FoundUpdate=Aktuell installierte Version: %1%nUpdate auf Version: %2
de.FoundSame=Version %1 ist bereits installiert. Es gibt nichts zu aktualisieren.
de.FoundNewer=Es ist eine neuere Version installiert: %1%nDieser Installer enthält Version %2.
de.BtnUpdate=Aktualisieren
de.BtnReinstall=Neu installieren
de.BtnDowngrade=Ältere Version installieren
de.BtnUninstall=Deinstallieren
de.BtnCancel=Abbrechen
de.ObsTitle=OBS Studio läuft noch
de.ObsText=OBS Studio muss beendet werden, bevor das Plugin installiert oder entfernt werden kann. Laufende Sendungen und Aufnahmen werden nicht ohne Rückfrage beendet, OBS fragt vorher nach.
de.BtnCloseObs=OBS Studio beenden
de.BtnRetry=Erneut prüfen
de.ObsRunning=OBS Studio läuft noch.

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
  Locator, Wmi, Found: Variant;
begin
  Result := False;
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Wmi := Locator.ConnectServer('.', 'root\CIMV2');
    Found := Wmi.ExecQuery('SELECT ProcessId FROM Win32_Process WHERE Name = ''' + Name + '''');
    Result := Found.Count > 0;
  except
  end;
end;

function ObsRunning(): Boolean;
begin
  Result := ProcessRunning('obs64.exe') or ProcessRunning('obs-browser-page.exe');
end;

{ Asks to close OBS Studio until it is no longer running. Closing is a normal
  close request, so OBS can still ask about a running stream or recording.
  Returns False if the user cancels. }
function WaitForObs(): Boolean;
var
  Code, I: Integer;
begin
  Result := True;
  while ObsRunning() do
    case TaskDialogMsgBox(CustomMessage('ObsTitle'), CustomMessage('ObsText'), mbError, MB_YESNOCANCEL, [CustomMessage('BtnCloseObs'), CustomMessage('BtnRetry'), CustomMessage('BtnCancel')], 0) of
      IDYES:
        begin
          Exec(ExpandConstant('{sys}\taskkill.exe'), '/IM obs64.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
          for I := 1 to 30 do
          begin
            if not ObsRunning() then
              break;
            Sleep(500);
          end;
        end;
      IDNO:
        ;
    else
      begin
        Result := False;
        exit;
      end;
    end;
end;

{ Compares dotted version numbers: -1, 0 or 1. }
function CompareVersions(A, B: String): Integer;
var
  PA, PB, NA, NB: Integer;
begin
  Result := 0;
  while (Result = 0) and ((A <> '') or (B <> '')) do
  begin
    PA := Pos('.', A);
    PB := Pos('.', B);
    if PA = 0 then PA := Length(A) + 1;
    if PB = 0 then PB := Length(B) + 1;
    NA := StrToIntDef(Copy(A, 1, PA - 1), 0);
    NB := StrToIntDef(Copy(B, 1, PB - 1), 0);
    Delete(A, 1, PA);
    Delete(B, 1, PB);
    if NA < NB then
      Result := -1
    else if NA > NB then
      Result := 1;
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
  Installed, Uninstaller, Text, Action: String;
  Code: Integer;
begin
  Result := True;
  if not RegQueryStringValue(HKLM64, UninstKey, 'DisplayVersion', Installed) then
    exit;
  case CompareVersions(Installed, '{#AppVersion}') of
    -1:
      begin
        Text := FmtMessage(CustomMessage('FoundUpdate'), [Installed, '{#AppVersion}']);
        Action := CustomMessage('BtnUpdate');
      end;
    0:
      begin
        Text := FmtMessage(CustomMessage('FoundSame'), [Installed]);
        Action := CustomMessage('BtnReinstall');
      end;
  else
    begin
      Text := FmtMessage(CustomMessage('FoundNewer'), [Installed, '{#AppVersion}']);
      Action := CustomMessage('BtnDowngrade');
    end;
  end;
  case TaskDialogMsgBox(CustomMessage('FoundTitle'), Text, mbConfirmation, MB_YESNOCANCEL, [Action, CustomMessage('BtnUninstall'), CustomMessage('BtnCancel')], 0) of
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
