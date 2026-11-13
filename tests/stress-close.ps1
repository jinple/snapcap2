# Opens and closes the test window many times while snapcap.exe is attached with
# the no-administrator fallback, and checks snapcap.exe keeps running each time.
param([int]$Runs = 20, [string[]]$Apis = @('vk', 'dx11', 'gl', 'dx12'))
$root = Split-Path -Parent $PSScriptRoot
$log = "$PSScriptRoot\results\stress-log.jsonl"
New-Item -ItemType Directory -Force "$PSScriptRoot\results" | Out-Null
Remove-Item $log -ErrorAction SilentlyContinue
if (Get-Process snapcap, testwin -ErrorAction SilentlyContinue) { 'snapcap or testwin already running'; exit 1 }
$sc = Start-Process "$root\bin\snapcap.exe" -ArgumentList '--no-admin', '--attach-title', 'snapcap-testwin', '--log', "`"$log`"" -PassThru
Start-Sleep -Seconds 1
$hung = $false
for ($i = 0; $i -lt $Runs -and -not $hung; $i++) {
    $api = $Apis[$i % $Apis.Count]
    Start-Process "$root\spike\bin\testwin.exe" -ArgumentList '--api', $api, '--fps', 45, '--seconds', 3 -Wait -WindowStyle Hidden
    Start-Sleep -Milliseconds 1500
    $a = (Get-Content $log).Count; Start-Sleep -Milliseconds 1200; $b = (Get-Content $log).Count
    if ($b -le $a) { $hung = $true; "HUNG after run $($i + 1) ($api). Last lines:"; Get-Content $log -Tail 4 }
}
if (-not $hung) { "$Runs runs, no hang" }
$attached = (Get-Content $log | Select-String '"attached":1').Count
"log lines with a window attached: $attached"
$sc.Kill()
