# Builds snapcap.exe and snapcap-fps.exe into .\bin with MSVC.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here 'bin'
$obj = Join-Path $here 'bin\obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$vcvars = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found' }

$common = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS'
$targets = @(
    @{ src = 'snapcap.cpp'; extra = 'recorder.cpp'; libs = 'user32.lib gdi32.lib shell32.lib shcore.lib dwmapi.lib version.lib comctl32.lib ole32.lib advapi32.lib winmm.lib d3d11.lib dxgi.lib d2d1.lib dwrite.lib mfplat.lib mfreadwrite.lib mfuuid.lib mmdevapi.lib windowsapp.lib' },
    @{ src = 'snapcap-fps.cpp'; libs = 'kernel32.lib advapi32.lib shell32.lib' }
)
$failed = $false
foreach ($t in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($t.src)
    $extra = if ($t.extra) { "`"$here\src\$($t.extra)`"" } else { '' }
    $cmd = "`"$vcvars`" >nul && cd /d `"$obj`" && cl $common `"$here\src\$($t.src)`" $extra /Fe:`"$bin\$name.exe`" /link /SUBSYSTEM:WINDOWS /MANIFEST:EMBED $($t.libs)"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: $name"; $failed = $true } else { Write-Host "built $name.exe" }
}
if ($failed) { exit 1 }
Get-ChildItem $bin -Filter *.exe | ForEach-Object { '{0}  {1:N0} KB' -f $_.Name, ($_.Length / 1KB) }
