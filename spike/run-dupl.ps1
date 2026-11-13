# Step 0 follow-up: does Desktop Duplication see a fullscreen OpenGL window
# that Windows Graphics Capture cannot? Same guard as run-monitor.ps1: only
# while the test window covers the whole screen, nothing stored, frames
# counted and single pixels read.
$bin = "$PSScriptRoot\bin"
$out = "$PSScriptRoot\results\dupl.jsonl"
Remove-Item $out -ErrorAction SilentlyContinue

function Run([string]$api, [int]$fps, [string]$tag, [string]$overlayMode, [int[]]$sample) {
    $p = Start-Process "$bin\testwin.exe" -ArgumentList '--api', $api, '--fps', $fps, '--seconds', 10, '--borderless', '--out', $out, '--tag', $tag -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    $ov = $null
    if ($overlayMode -ne 'none') {
        $oa = @('--title', "snapcap-testwin-$api", '--seconds', 30)
        if ($overlayMode -eq 'excluded') { $oa += '--exclude' }
        $ov = Start-Process "$bin\overlay.exe" -ArgumentList $oa -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 1000
    }
    & "$bin\capprobe.exe" --title "snapcap-testwin-$api" --seconds 4 --dupl --sample $sample[0] $sample[1] --out $out --tag $tag | Out-Null
    $p.WaitForExit()
    if ($ov) { if (-not $ov.WaitForExit(5000)) { $ov.Kill() } }
    Start-Sleep -Seconds 1
}

$centre = @(1920, 1080)
foreach ($fps in 30, 45, 60, 120) { Run 'gl' $fps "count-gl-$fps" 'none' $centre }
Run 'dx11' 45 'count-dx11-45' 'none' $centre
$underOverlay = @(20, 46)
Run 'gl' 60 'overlay-none-gl' 'none' $underOverlay
Run 'gl' 60 'overlay-shown-gl' 'shown' $underOverlay
Run 'gl' 60 'overlay-excluded-gl' 'excluded' $underOverlay

Get-Content $out | ConvertFrom-Json | Group-Object tag | ForEach-Object {
    $t = $_.Group | Where-Object kind -eq 'truth'; $c = $_.Group | Where-Object kind -eq 'capture'
    [pscustomobject]@{ run = $_.Name; true_fps = $t.fps; counted = $c.arrival_fps; accum = $c.accumulated_fps; pixel_rgb = $c.sample_rgb; sampled = $c.sampled; fmt = $c.format; opens = $c.opens; err = $c.first_error; guard = $c.guard_tripped }
} | Format-Table -AutoSize
