$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$prerequisites = Join-Path $root 'artifacts\prerequisites'
New-Item -ItemType Directory -Force $prerequisites | Out-Null
$msi = Join-Path $prerequisites 'winfsp.msi'
Invoke-WebRequest -Uri 'https://github.com/winfsp/winfsp/releases/download/v2.2/winfsp-2.2.26323.msi' -OutFile $msi
if ((Get-FileHash $msi -Algorithm SHA256).Hash.ToLowerInvariant() -ne '36ce01f9eedbbe6842b7fb4bedc79d7d4f71983e8aceb6877b46a07b331c7e19') {
    throw 'WinFsp installer checksum mismatch'
}
$compiler = Get-Command iscc.exe -ErrorAction SilentlyContinue
if ($compiler) { $iscc = $compiler.Source }
else { $iscc = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'Inno Setup 6\ISCC.exe' }
if (!(Test-Path $iscc)) { throw 'Inno Setup 6 is required' }
& $iscc (Join-Path $root 'installer\WinZipFS.iss')
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
