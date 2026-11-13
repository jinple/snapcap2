# Step 0, no elevation: what do the overlay window, a capture session and
# recording cost a GPU-bound test window in fps? Warm-up run discarded,
# conditions run in forward then reversed order, repeated.
param([string]$Out = "$PSScriptRoot\results\cost.jsonl", [switch]$Borderless, [int]$Load = 4000, [int]$Rounds = 2)
$bin = "$PSScriptRoot\bin"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$mode = $(if ($Borderless) { 'borderless' } else { 'windowed' })
$title = 'snapcap-testwin-dx11'

function Run([string]$cond, [string]$outFile) {
    $a = @('--api', 'dx11', '--fps', 0, '--load', $Load, '--seconds', 12, '--tag', "$cond-$mode")
    if ($outFile) { $a += @('--out', $outFile) }
    if ($Borderless) { $a += '--borderless' } else { $a += @('--w', 2560, '--h', 1440) }
    $p = Start-Process "$bin\testwin.exe" -ArgumentList $a -PassThru -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200
    $side = $null
    switch ($cond) {
        'overlay' { $side = Start-Process "$bin\overlay.exe" -ArgumentList '--title', $title, '--seconds', 30 -PassThru -WindowStyle Hidden }
        'capture' { $side = Start-Process "$bin\capprobe.exe" -ArgumentList '--title', $title, '--seconds', 9 -PassThru -WindowStyle Hidden }
        'record'  { $side = Start-Process "$bin\capprobe.exe" -ArgumentList '--title', $title, '--seconds', 9, '--record', "$PSScriptRoot\results\cost-tmp.mp4" -PassThru -WindowStyle Hidden }
    }
    $p.WaitForExit()
    if ($side) { if (-not $side.WaitForExit(8000)) { $side.Kill() } }
    Start-Sleep -Seconds 2
}

Run 'base' $null   # discarded warm-up
$order = 'base', 'overlay', 'capture', 'record'
for ($r = 0; $r -lt $Rounds; $r++) {
    foreach ($c in $order) { Run $c $Out }
    foreach ($c in ($order[($order.Count - 1)..0])) { Run $c $Out }
}
Get-Content $Out | ConvertFrom-Json | Where-Object { $_.tag -like "*-$mode" } | Group-Object tag | ForEach-Object {
    $m = ($_.Group | Measure-Object fps -Average -Minimum -Maximum)
    [pscustomobject]@{ condition = $_.Name; runs = $m.Count; avg = [math]::Round($m.Average, 2); min = [math]::Round($m.Minimum, 2); max = [math]::Round($m.Maximum, 2) }
} | Format-Table -AutoSize
