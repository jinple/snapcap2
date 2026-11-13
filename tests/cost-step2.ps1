# What recording with snapcap.exe costs the recorded program, on a GPU-bound
# DX11 test window at 2560x1440. Discarded warm-up, then base/record/record/base, twice.
# No administrator prompt. Recordings made here are deleted afterwards.
param([int]$Load = 4000, [int]$Rounds = 2)
$root = Split-Path -Parent $PSScriptRoot
$tw = "$root\spike\bin\testwin.exe"
$out = "$PSScriptRoot\results\s2-cost.jsonl"; $log = "$PSScriptRoot\results\s2-cost-log.jsonl"
Remove-Item $out, $log -ErrorAction SilentlyContinue
if (Get-Process snapcap, testwin -ErrorAction SilentlyContinue) { 'snapcap or testwin already running'; exit 1 }
$before = @(Get-ChildItem "$env:USERPROFILE\Videos\SnapCap\*.mp4" -ErrorAction SilentlyContinue | ForEach-Object FullName)

function One([string]$cond) {
    $sc = $null
    if ($cond -eq 'record') {
        # recording starts 1 s after the window appears, before the measured part (2 s to 11 s)
        $sc = Start-Process "$root\bin\snapcap.exe" -ArgumentList '--no-admin', '--attach-title', 'snapcap-testwin', '--record-after', '1', '--show-on-screen', '1', '--log', "`"$log`"" -PassThru
        Start-Sleep -Milliseconds 500
    }
    Start-Process $tw -ArgumentList '--api', 'dx11', '--fps', 0, '--load', $Load, '--seconds', 12, '--w', 2560, '--h', 1440, '--out', $out, '--tag', $cond -Wait -WindowStyle Hidden
    if ($sc) {
        $cpu0 = $sc.TotalProcessorTime.TotalSeconds
        Start-Sleep -Seconds 3
        if (-not $sc.HasExited) { $script:cpu += $sc.TotalProcessorTime.TotalSeconds; $sc.Kill() }
    }
    Start-Sleep -Seconds 2
}
$script:cpu = 0
One 'warmup'
for ($i = 0; $i -lt $Rounds; $i++) { One 'base'; One 'record'; One 'record'; One 'base' }

$rows = Get-Content $out | ConvertFrom-Json | Where-Object { $_.kind -eq 'truth' -and $_.tag -ne 'warmup' }
$base = ($rows | Where-Object tag -eq 'base' | Measure-Object fps -Average).Average
$rows | Group-Object tag | ForEach-Object {
    $m = $_.Group | Measure-Object fps -Average -Minimum -Maximum
    [pscustomobject]@{ condition = $_.Name; runs = $m.Count; avg_fps = [math]::Round($m.Average, 2); min = [math]::Round($m.Minimum, 2); max = [math]::Round($m.Maximum, 2); change_pct = [math]::Round(100 * ($m.Average - $base) / $base, 1) }
} | Format-Table -AutoSize
Get-Content $log | Where-Object { $_ -match '"record":1' } | ConvertFrom-Json | ForEach-Object { "recording: ok=$($_.ok) $($_.w)x$($_.h) frames=$($_.frames) dropped=$($_.dropped) seconds=$($_.seconds) hw=$($_.hw_encoder)" }
"snapcap.exe CPU: $([math]::Round(100 * $script:cpu / (4 * $Rounds / 2 * 11), 1))% of one core, averaged over the recording runs (about 11 s each)"
Get-ChildItem "$env:USERPROFILE\Videos\SnapCap\*.mp4" | Where-Object { $before -notcontains $_.FullName } | Remove-Item
