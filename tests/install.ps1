$ErrorActionPreference = 'Stop'
$target = Join-Path $env:RUNNER_TEMP 'WinZipFS installed'
$setup = (Resolve-Path 'artifacts\installer\WinZipFS-Setup-x64.exe').Path
$log = Join-Path (Get-Location) 'artifacts\test-results'
New-Item -ItemType Directory -Force $log | Out-Null
function Run-Setup($File, $Arguments) {
    $p = Start-Process -FilePath $File -ArgumentList $Arguments -Wait -PassThru
    if ($p.ExitCode -ne 0) { throw "$File returned $($p.ExitCode)" }
}
Run-Setup $setup @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/DIR="{0}"' -f $target),('/LOG="{0}"' -f (Join-Path $log 'install.log')))
$exe = Join-Path $target 'WinZipFS.exe'
if (!(Test-Path $exe)) { throw 'Application was not installed' }
$registered = Get-ItemPropertyValue 'HKCU:\Software\RegisteredApplications' 'WinZipFS'
if ($registered -ne 'Software\WinZipFS\Capabilities') { throw 'Default-app registration missing' }
$command = (Get-Item 'HKCU:\Software\Classes\WinZipFS.Archive\shell\open\command').GetValue('')
if (!$command.Contains($exe)) { throw 'The registered command does not point to the installed executable' }
$menu = Join-Path ([Environment]::GetFolderPath('Programs')) 'WinZipFS.lnk'
if (!(Test-Path $menu)) { throw 'Start menu shortcut missing' }
New-Item -ItemType Directory -Force 'artifacts\fixtures' | Out-Null
@'
from zipfile import ZipFile
with ZipFile("artifacts/fixtures/ui.zip", "w") as archive:
    archive.writestr("sample.txt", "original")
'@ | python -
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
python -m pip install pywinauto==0.6.9 Pillow
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:WZ_EXE = $exe
python -u tests/ui_test.py picker
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Remove-Item Env:WZ_EXE
$uninstaller = Join-Path $target 'unins000.exe'
Run-Setup $uninstaller @('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',('/LOG="{0}"' -f (Join-Path $log 'uninstall.log')))
if (Test-Path $exe) { throw 'Application executable was not removed' }
if (Test-Path 'HKCU:\Software\Classes\WinZipFS.Archive') { throw 'Archive association was not removed' }
if (Test-Path $menu) { throw 'Start menu shortcut was not removed' }
if (!(Test-Path 'HKLM:\SOFTWARE\WOW6432Node\WinFsp')) { throw 'Shared WinFsp installation was removed' }
