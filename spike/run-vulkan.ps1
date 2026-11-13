# Step 0 on Vulkan: exact FPS (one administrator prompt), no-elevation count,
# and what each capture method sees in fullscreen. Captures only the test
# window; whole-screen methods run only while it covers the screen.
$bin = "$PSScriptRoot\bin"
$res = "$PSScriptRoot\results"
$truth = "$res\vk-etw-truth.jsonl"; $csv = "$res\vk-etw.csv"; $ready = "$res\etw.ready"; $stop = "$res\etw.stop"
$out = "$res\vk-capture.jsonl"
Remove-Item $truth, $csv, $ready, $stop, $out -ErrorAction SilentlyContinue
$title = 'snapcap-testwin-vk'

# --- 1. exact FPS through the elevated helper
$etwOk = $true
try {
    Start-Process "$bin\fpsprobe.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList '--exe', 'testwin.exe', '--out', "`"$csv`"", '--readyfile', "`"$ready`"", '--stopfile', "`"$stop`"", '--seconds', 600
} catch { $etwOk = $false; Write-Host "EXACT-FPS PART NOT RUN: administrator prompt declined or failed." }
if ($etwOk) {
    $w = 0; while (-not (Test-Path $ready) -and $w -lt 30) { Start-Sleep -Milliseconds 500; $w += 0.5 }
    if (-not (Test-Path $ready)) { $etwOk = $false; Write-Host "EXACT-FPS PART NOT RUN: helper did not start a trace." }
}
if ($etwOk) {
    foreach ($mode in 'windowed', 'borderless') {
        foreach ($fps in 30, 60, 144, 240) {
            $a = @('--api', 'vk', '--fps', $fps, '--seconds', 9, '--out', $truth, '--tag', "vk-$fps-$mode")
            if ($mode -eq 'borderless') { $a += '--borderless' }
            Start-Process "$bin\testwin.exe" -ArgumentList $a -Wait -WindowStyle Hidden
            Start-Sleep -Milliseconds 800
        }
    }
    Set-Content $stop 'stop'
    $w = 0; while (-not (Select-String -Path $csv -Pattern '^total,' -Quiet -ErrorAction SilentlyContinue) -and $w -lt 20) { Start-Sleep -Milliseconds 500; $w += 0.5 }
    Remove-Item $ready, $stop -ErrorAction SilentlyContinue
    "=== exact FPS (helper) ==="
    & "$PSScriptRoot\analyze-etw.ps1" -Truth $truth -Csv $csv
}

# --- 2 and 3. capture methods
function Run([int]$fps, [string]$tag, [string]$mode, [string[]]$capArgs, [string]$overlayMode = 'none') {
    $a = @('--api', 'vk', '--fps', $fps, '--seconds', 10, '--out', $out, '--tag', $tag)
    if ($mode -eq 'borderless') { $a += '--borderless' }
    $p = Start-Process "$bin\testwin.exe" -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    $ov = $null
    if ($overlayMode -ne 'none') {
        $ov = Start-Process "$bin\overlay.exe" -ArgumentList '--title', $title, '--seconds', 30 -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 1000
    }
    & "$bin\capprobe.exe" --title $title --seconds 4 --out $out --tag $tag @capArgs | Out-Null
    $p.WaitForExit()
    if ($ov) { if (-not $ov.WaitForExit(5000)) { $ov.Kill() } }
    Start-Sleep -Seconds 1
}
foreach ($fps in 30, 45, 60, 120) { Run $fps "window-windowed-$fps" 'windowed' @() }
foreach ($fps in 30, 45, 60, 120) { Run $fps "window-borderless-$fps" 'borderless' @() }
foreach ($fps in 45, 120) { Run $fps "monitor-borderless-$fps" 'borderless' @('--monitor', '--sample', '1920', '1080') }
foreach ($fps in 45, 120) { Run $fps "dupl-borderless-$fps" 'borderless' @('--dupl', '--sample', '1920', '1080') }
Run 60 'overlaypx-none-dupl' 'borderless' @('--dupl', '--sample', '20', '46') 'none'
Run 60 'overlaypx-shown-dupl' 'borderless' @('--dupl', '--sample', '20', '46') 'shown'
Run 60 'overlaypx-none-monitor' 'borderless' @('--monitor', '--sample', '20', '46') 'none'
Run 60 'overlaypx-shown-monitor' 'borderless' @('--monitor', '--sample', '20', '46') 'shown'

"=== capture methods ==="
Get-Content $out | ConvertFrom-Json | Group-Object tag | ForEach-Object {
    $t = $_.Group | Where-Object kind -eq 'truth'; $c = $_.Group | Where-Object kind -eq 'capture'
    [pscustomobject]@{ run = $_.Name; true_fps = $t.fps; counted = $c.arrival_fps; pixel_rgb = $c.sample_rgb; sampled = $c.sampled; guard = $c.guard_tripped }
} | Format-Table -AutoSize
