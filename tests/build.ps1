# Builds the test helpers (mp4check.exe, tone.exe) into tests\bin, and rebuilds the test window.
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$bin = Join-Path $here 'bin'
New-Item -ItemType Directory -Force $bin | Out-Null
$vcvars = @(
    'C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
$common = '/nologo /std:c++20 /EHsc /O2 /MT /W3 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS'
$targets = @(
    @{ src = 'mp4check.cpp'; libs = 'ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib propsys.lib' },
    @{ src = 'tone.cpp';     libs = 'winmm.lib' }
)
foreach ($t in $targets) {
    $name = [IO.Path]::GetFileNameWithoutExtension($t.src)
    cmd /c "`"$vcvars`" >nul && cd /d `"$bin`" && cl $common `"$here\$($t.src)`" /Fe:$name.exe /link $($t.libs)"
    if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FAILED: $name" } else { Write-Host "built $name.exe" }
}
& "$here\..\spike\build.ps1"
