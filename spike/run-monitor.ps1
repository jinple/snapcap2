# Step 0 follow-up: does monitor capture see a fullscreen window that window
# capture cannot? Monitor capture is only started while the test window covers
# the whole screen, drops every frame if anything else shows, and stores no
# picture: it counts frames and reads single pixels.
$bin = "$PSScriptRoot\bin"
$out = "$PSScriptRoot\results\monitor.jsonl"
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
    & "$bin\capprobe.exe" --title "snapcap-testwin-$api" --seconds 4 --monitor --sample $sample[0] $sample[1] --out $out --tag $tag | Out-Null
    $p.WaitForExit()
    if ($ov) { if (-not $ov.WaitForExit(5000)) { $ov.Kill() } }
    Start-Sleep -Seconds 1
}

$centre = @(1920, 1080)
foreach ($api in 'gl', 'dx11', 'dx12') {
    foreach ($fps in 30, 45, 60, 120) { Run $api $fps "count-$api-$fps" 'none' $centre }
}
$underOverlay = @(20, 46)
foreach ($api in 'gl', 'dx11') {
    Run $api 60 "overlay-none-$api" 'none' $underOverlay
    Run $api 60 "overlay-shown-$api" 'shown' $underOverlay
    Run $api 60 "overlay-excluded-$api" 'excluded' $underOverlay
}

Get-Content $out | ConvertFrom-Json | Group-Object tag | ForEach-Object {
    $t = $_.Group | Where-Object kind -eq 'truth'; $c = $_.Group | Where-Object kind -eq 'capture'
    [pscustomobject]@{ run = $_.Name; true_fps = $t.fps; counted = $c.arrival_fps; pixel_rgb = $c.sample_rgb; sampled = $c.sampled; guard = $c.guard_tripped }
} | Format-Table -AutoSize
