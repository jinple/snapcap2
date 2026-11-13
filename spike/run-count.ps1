# Step 0, no elevation: does counting capture-frame arrivals match the true
# frame rate? Only ever captures the spike's own test window.
param([string]$Out = "$PSScriptRoot\results\count.jsonl", [switch]$Borderless)
$bin = "$PSScriptRoot\bin"
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
foreach ($api in 'dx11', 'dx12', 'gl') {
    foreach ($fps in $(if ($Borderless) { 30, 45, 60, 120 } else { 30, 45, 57, 60, 120, 240 })) {
        $tag = "$api-$fps" + $(if ($Borderless) { '-borderless' } else { '-windowed' })
        $a = @('--api', $api, '--fps', $fps, '--seconds', 11, '--out', $Out, '--tag', $tag)
        if ($Borderless) { $a += '--borderless' }
        $p = Start-Process "$bin\testwin.exe" -ArgumentList $a -PassThru -WindowStyle Hidden
        Start-Sleep -Milliseconds 2500
        & "$bin\capprobe.exe" --title "snapcap-testwin-$api" --seconds 6 --out $Out --tag $tag | Out-Null
        $p.WaitForExit()
    }
}
Get-Content $Out
