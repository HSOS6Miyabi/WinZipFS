param([Parameter(Mandatory=$true)][string] $Destination)
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Microsoft Visual Studio\Installer\vswhere.exe'
$visualStudio = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$visualStudio) { throw 'Visual Studio C++ redistributables not found' }
$redist = Join-Path $visualStudio 'VC\Redist\MSVC'
$directory = Get-ChildItem $redist -Directory |
    Where-Object { $_.Name -match '^\d+\.\d+(\.\d+)?(\.\d+)?$' -and (Test-Path (Join-Path $_.FullName 'x64\Microsoft.VC143.CRT')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$directory) { throw 'Visual C++ x64 runtime files not found' }
Copy-Item (Join-Path $directory.FullName 'x64\Microsoft.VC143.CRT\*.dll') $Destination -Force
