# Step 0, needs one administrator prompt: does Windows event tracing give the
# exact frame rate of the test window on each graphics API?
# Starts fpsprobe.exe elevated once, runs the test window at known rates,
# then compares event counts per second against the test window's own count.
param([switch]$Borderless)
$bin = "$PSScriptRoot\bin"
$res = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force $res | Out-Null
$mode = $(if ($Borderless) { 'borderless' } else { 'windowed' })
$truth = "$res\etw-truth-$mode.jsonl"
$csv = "$res\etw-$mode.csv"
$ready = "$res\etw.ready"
$stop = "$res\etw.stop"
Remove-Item $truth, $csv, $ready, $stop -ErrorAction SilentlyContinue

try {
    Start-Process "$bin\fpsprobe.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList '--exe', 'testwin.exe', '--out', "`"$csv`"", '--readyfile', "`"$ready`"", '--stopfile', "`"$stop`"", '--seconds', 900
} catch {
    Write-Host "NOT RUN: administrator prompt was declined or failed: $($_.Exception.Message)"
    exit 1
}
$waited = 0
while (-not (Test-Path $ready) -and $waited -lt 30) { Start-Sleep -Milliseconds 500; $waited += 0.5 }
if (-not (Test-Path $ready)) {
    Write-Host "NOT RUN: helper did not start a trace."
    if (Test-Path $csv) { Get-Content $csv }
    exit 1
}

foreach ($api in 'dx11', 'dx12', 'gl') {
    foreach ($fps in $(if ($Borderless) { 30, 60, 144, 240 } else { 30, 60, 144, 240, 0 })) {
        $a = @('--api', $api, '--fps', $fps, '--seconds', 9, '--out', $truth, '--tag', "$api-$fps-$mode")
        if ($Borderless) { $a += '--borderless' }
        if ($fps -eq 0) { $a += @('--load', 300) }   # keeps "uncapped" in the hundreds, not thousands, on DX11
        Start-Process "$bin\testwin.exe" -ArgumentList $a -Wait -WindowStyle Hidden
        Start-Sleep -Milliseconds 800
    }
}
Set-Content $stop 'stop'
$waited = 0
while (-not (Select-String -Path $csv -Pattern '^total,' -Quiet -ErrorAction SilentlyContinue) -and $waited -lt 20) { Start-Sleep -Milliseconds 500; $waited += 0.5 }
Remove-Item $ready, $stop -ErrorAction SilentlyContinue
& "$PSScriptRoot\analyze-etw.ps1" -Truth $truth -Csv $csv
