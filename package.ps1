# Makes dist\SnapCap.zip (the two programs and the readme) and, when Inno Setup is
# installed, dist\SnapCap-Setup.exe. Run build.ps1 first.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stage = Join-Path $here 'dist\SnapCap'
Remove-Item (Join-Path $here 'dist') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$here\bin\snapcap.exe", "$here\bin\snapcap-fps.exe", "$here\README.txt" $stage
Compress-Archive -Path $stage -DestinationPath "$here\dist\SnapCap.zip"

$iscc = @(
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
    "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
    "$env:ProgramFiles\Inno Setup 6\ISCC.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($iscc) {
    & $iscc /Q "$here\installer\snapcap.iss"
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup failed' }
} else {
    Write-Host 'Inno Setup 6 not found: skipped the installer (zip only).'
}
Get-ChildItem $stage, "$here\dist\SnapCap.zip", "$here\dist\SnapCap-Setup.exe" -ErrorAction SilentlyContinue |
    ForEach-Object { '{0}  {1:N0} KB' -f $_.Name, ($_.Length / 1KB) }
