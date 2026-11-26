$ErrorActionPreference = 'Stop'
function Get-VerifiedFile($Url, $Sha, $File) {
    Invoke-WebRequest -Uri $Url -OutFile $File
    if ((Get-FileHash $File -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Sha) {
        throw "SHA256 mismatch: $File"
    }
}
$msi = Join-Path $env:RUNNER_TEMP 'winfsp.msi'
Get-VerifiedFile 'https://github.com/winfsp/winfsp/releases/download/v2.2/winfsp-2.2.26323.msi' '36ce01f9eedbbe6842b7fb4bedc79d7d4f71983e8aceb6877b46a07b331c7e19' $msi
$install = Start-Process msiexec.exe -ArgumentList @('/i', ('"{0}"' -f $msi), 'ADDLOCAL=ALL','/qn','/norestart') -Wait -PassThru
if ($install.ExitCode -notin 0,3010) { throw "WinFsp installer returned $($install.ExitCode)" }
$winfsp = Join-Path ([Environment]::GetFolderPath('ProgramFilesX86')) 'WinFsp'
if (!(Test-Path (Join-Path $winfsp 'inc\winfsp\winfsp.h'))) { throw 'WinFsp SDK missing' }
$installer = Join-Path $env:RUNNER_TEMP '7zip.exe'
Get-VerifiedFile 'https://github.com/ip7z/7zip/releases/download/26.06/7z2606-x64.exe' '9b697f65ecc2d9b0152f5ee96d72dde522a7827690349d9307935dacf40f42ec' $installer
$install = Start-Process $installer -ArgumentList '/S' -Wait -PassThru
if ($install.ExitCode -ne 0) { throw "7-Zip installer returned $($install.ExitCode)" }
if (!(Test-Path 'C:\Program Files\7-Zip\7z.dll')) { throw '7-Zip engine missing' }
