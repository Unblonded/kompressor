; Kompressor installer (Inno Setup 6).
; Build with packaging/windows/build_installer.ps1, or:
;   ISCC.exe /DAppVersion=1.0.0 /DBuildDir=..\..\build\windows packaging\windows\kompressor.iss
;
; Installs per user (no admin): %LOCALAPPDATA%\Programs\Kompressor.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BuildDir
  #define BuildDir "..\..\build\windows"
#endif
#ifndef OutputDir
  #define OutputDir BuildDir + "\installer"
#endif

[Setup]
AppId={{6E2B6C1A-4F7D-4C1B-9E2A-6B1D2C7A9F41}
AppName=Kompressor
AppVersion={#AppVersion}
AppVerName=Kompressor {#AppVersion}
AppPublisher=Kompressor
AppPublisherURL=https://github.com/Unblonded/kompressor
AppSupportURL=https://github.com/Unblonded/kompressor/issues
DefaultDirName={autopf}\Kompressor
DefaultGroupName=Kompressor
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir={#OutputDir}
OutputBaseFilename=KompressorSetup-{#AppVersion}
SetupIconFile=..\icons\kompressor.ico
UninstallDisplayIcon={app}\Kompressor.exe
UninstallDisplayName=Kompressor
WizardStyle=modern dynamic
WizardSmallImageFile=wizard_small_55.bmp,wizard_small_69.bmp,wizard_small_83.bmp,wizard_small_110.bmp,wizard_small_138.bmp
WizardImageAlphaFormat=defined
Compression=lzma2/ultra64
SolidCompression=yes
ChangesEnvironment=yes
CloseApplications=yes
RestartApplications=no
VersionInfoVersion={#AppVersion}
VersionInfoDescription=Kompressor Setup

[Tasks]
Name: "contextmenu"; Description: "Add Kompressor to the Explorer right-click menu"; GroupDescription: "Integration:"
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Shortcuts:"; Flags: unchecked
Name: "addtopath"; Description: "Add the command-line tool (kompressor-cli) to PATH"; GroupDescription: "Command line:"; Flags: unchecked

[Files]
Source: "{#BuildDir}\Kompressor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\kompressor-cli.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\Kompressor"; Filename: "{app}\Kompressor.exe"
Name: "{autodesktop}\Kompressor"; Filename: "{app}\Kompressor.exe"; Tasks: desktopicon

[Run]
; The app writes (or removes) its own menu entries so they point at the installed exe.
Filename: "{app}\Kompressor.exe"; Parameters: "--register-shell"; Tasks: contextmenu; Flags: runhidden waituntilterminated
Filename: "{app}\Kompressor.exe"; Parameters: "--unregister-shell"; Tasks: not contextmenu; Flags: runhidden waituntilterminated
Filename: "{app}\Kompressor.exe"; Description: "Launch Kompressor"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\Kompressor.exe"; Parameters: "--unregister-shell"; Flags: runhidden waituntilterminated; RunOnceId: "RemoveShellMenu"

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Registry]
; App preferences (e.g. "right-click menu turned off") go with the app.
Root: HKCU; Subkey: "Software\Kompressor"; Flags: uninsdeletekey

[Code]
const
  EnvKey = 'Environment';

function PathContains(Paths, Dir: string): Boolean;
begin
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';') > 0;
end;

procedure AddToPath(Dir: string);
var
  Paths: string;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then Paths := '';
  if PathContains(Paths, Dir) then Exit;
  if (Paths <> '') and (Copy(Paths, Length(Paths), 1) <> ';') then Paths := Paths + ';';
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Paths + Dir);
end;

procedure RemoveFromPath(Dir: string);
var
  Paths: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvKey, 'Path', Paths) then Exit;
  P := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';');
  if P = 0 then Exit;
  Delete(Paths, P, Length(Dir) + 1);  // the entry plus its trailing ';'
  if (Length(Paths) > 0) and (Copy(Paths, Length(Paths), 1) = ';') then Delete(Paths, Length(Paths), 1);
  RegWriteExpandStringValue(HKCU, EnvKey, 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    if WizardIsTaskSelected('addtopath') then AddToPath(ExpandConstant('{app}'))
    else RemoveFromPath(ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then RemoveFromPath(ExpandConstant('{app}'));
end;
