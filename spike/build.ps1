# Builds the step 0 probes into spike\bin with MSVC.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$vcvars = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw 'vcvars64.bat not found' }

$common = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /IC:\vcpkg\installed\x64-windows\include'
$targets = @(
    @{ src = 'testwin.cpp';  libs = 'user32.lib gdi32.lib winmm.lib d3d11.lib d3d12.lib dxgi.lib d3dcompiler.lib opengl32.lib' },
    @{ src = 'fpsprobe.cpp'; libs = 'advapi32.lib' },
    @{ src = 'overlay.cpp';  libs = 'user32.lib gdi32.lib' },
    @{ src = 'capprobe.cpp'; libs = 'user32.lib winmm.lib d3d11.lib dxgi.lib mfplat.lib mfreadwrite.lib mfuuid.lib windowsapp.lib' }
)
$failed = $false
foreach ($t in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($t.src)
    $cmd = "`"$vcvars`" >nul && cd /d `"$bin`" && cl $common `"$here\$($t.src)`" /Fe:$name.exe /link $($t.libs)"
    cmd /c $cmd
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: $name"; $failed = $true } else { Write-Host "built $name.exe" }
}
if ($failed) { exit 1 }
