#define AppVersion "0.1.0"
[Setup]
AppId={{F5E59D55-7EE5-4B9B-8A2D-FD859D404FAA}
AppName=WinZipFS
AppVersion={#AppVersion}
AppPublisher=WinZipFS
AppPublisherURL=https://github.com/HSOS6Miyabi/WinZipFS
AppSupportURL=https://github.com/HSOS6Miyabi/WinZipFS/issues
DefaultDirName={localappdata}\Programs\WinZipFS
DefaultGroupName=WinZipFS
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.17763
AppMutex=Local\WinZipFS.Instance
CloseApplications=no
RestartApplications=no
UninstallDisplayIcon={app}\WinZipFS.exe
OutputDir=..\artifacts\installer
OutputBaseFilename=WinZipFS-Setup-x64
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
LicenseFile=..\LICENSE
ChangesAssociations=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "chinesesimplified"; MessagesFile: "compiler:Languages\ChineseSimplified.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; Flags: unchecked
Name: "registertypes"; Description: "Register WinZipFS for archive file types"

[Files]
Source: "..\artifacts\app\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\artifacts\prerequisites\winfsp.msi"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\WinZipFS"; Filename: "{app}\WinZipFS.exe"
Name: "{autodesktop}\WinZipFS"; Filename: "{app}\WinZipFS.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\WinZipFS.exe"; Parameters: "--register"; Tasks: registertypes; Flags: runhidden waituntilterminated
Filename: "{app}\WinZipFS.exe"; Description: "{cm:LaunchProgram,WinZipFS}"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\WinZipFS.exe"; Parameters: "--unregister"; Flags: runhidden waituntilterminated; RunOnceId: "UnregisterArchiveTypes"

[Code]
function HasWinFsp(): Boolean;
var
  Dir: String;
  MajorMinor, BuildRevision: Cardinal;
begin
  Result := False;
  if RegQueryStringValue(HKLM32, 'SOFTWARE\WinFsp', 'InstallDir', Dir) then
    if GetVersionNumbers(AddBackslash(Dir) + 'bin\winfsp-x64.dll', MajorMinor, BuildRevision) then
      Result := MajorMinor >= $00020002;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ExitCode: Integer;
  Parameters: String;
begin
  Result := '';
  if HasWinFsp() then exit;
  ExtractTemporaryFile('winfsp.msi');
  Parameters := '/i "' + ExpandConstant('{tmp}\winfsp.msi') + '" /qn /norestart';
  if not ShellExec('runas', ExpandConstant('{sys}\msiexec.exe'), Parameters, '',
                    SW_SHOW, ewWaitUntilTerminated, ExitCode) then
  begin
    Result := 'WinFsp driver installation requires administrator approval.';
    exit;
  end;
  if ExitCode = 3010 then
  begin
    NeedsRestart := True;
    Result := 'Restart Windows to finish installing the WinFsp driver, then run WinZipFS Setup again.';
    exit;
  end;
  if (ExitCode <> 0) or (not HasWinFsp()) then
    Result := 'WinFsp driver installation failed (Windows Installer code ' + IntToStr(ExitCode) + ').';
end;
