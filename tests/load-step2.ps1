# Records a fullscreen DX11 test window that keeps the graphics card fully busy
# (as a real game does), and reports how many of the 60 frames a second made it
# into the file. No administrator prompt. The recordings are deleted afterwards.
param([int[]]$Loads = @(4000, 12000, 30000), [int]$Seconds = 14, [switch]$Windowed, [int]$W = 2560, [int]$H = 1440)
$root = Split-Path -Parent $PSScriptRoot
$res = "$PSScriptRoot\results"; $log = "$res\s2-load-log.jsonl"; $truth = "$res\s2-load-truth.jsonl"
Remove-Item $log, $truth -ErrorAction SilentlyContinue
if (Get-Process snapcap, testwin -ErrorAction SilentlyContinue) { 'snapcap or testwin already running'; exit 1 }
$before = @(Get-ChildItem "$env:USERPROFILE\Videos\SnapCap\*.mp4" -ErrorAction SilentlyContinue | ForEach-Object FullName)
$rows = foreach ($load in $Loads) {
    $sc = Start-Process "$root\bin\snapcap.exe" -ArgumentList '--no-admin', '--attach-title', 'snapcap-testwin', '--record-after', '2', '--log', "`"$log`"" -PassThru
    Start-Sleep -Milliseconds 700
    Start-Process "$root\spike\bin\testwin.exe" -ArgumentList (@('--api', 'dx11', '--fps', 0, '--load', $load, '--seconds', $Seconds, '--out', $truth, '--tag', "load-$load") + $(if ($Windowed) { @('--w', $W, '--h', $H) } else { @('--borderless') })) -Wait -WindowStyle Hidden
    Start-Sleep -Seconds 4
    if (-not $sc.HasExited) { $sc.Kill() }
    Start-Sleep -Seconds 1
    $r = Get-Content $log | Where-Object { $_ -match '"record":1' } | Select-Object -Last 1 | ConvertFrom-Json
    $t = Get-Content $truth | ConvertFrom-Json | Where-Object tag -eq "load-$load"
    [pscustomobject]@{ load = $load; game_fps = [math]::Round($t.fps, 1); ok = $r.ok; seconds = $r.seconds; frames = $r.frames
        video_fps = [math]::Round($r.frames / [math]::Max($r.seconds, 0.001), 1); no_buffer_or_draw = $r.dropped; loop_late = $r.late; hw = $r.hw_encoder }
}
$rows | Format-Table -AutoSize
Get-ChildItem "$env:USERPROFILE\Videos\SnapCap\*.mp4" | Where-Object { $before -notcontains $_.FullName } | Remove-Item
