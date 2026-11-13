# Makes dist\SnapCap.zip: the two programs and the readme. Run build.ps1 first.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$stage = Join-Path $here 'dist\SnapCap'
Remove-Item (Join-Path $here 'dist') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item "$here\bin\snapcap.exe", "$here\bin\snapcap-fps.exe", "$here\README.txt" $stage
Compress-Archive -Path $stage -DestinationPath "$here\dist\SnapCap.zip"
Get-ChildItem $stage, "$here\dist\SnapCap.zip" | ForEach-Object { '{0}  {1:N0} KB' -f $_.Name, ($_.Length / 1KB) }
