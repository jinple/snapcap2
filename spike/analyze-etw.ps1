# Joins the event-trace counts with the test window's own frame counts and
# shows, for every run, which event ids ran at the true frame rate.
param([string]$Truth, [string]$Csv)
$lines = Get-Content $Csv
$lines | Where-Object { $_ -like 'error,*' -or $_ -like 'total,*' } | ForEach-Object { Write-Host $_ }
$rows = $lines | Where-Object { $_ -match '^\d+,\d+,' } | ForEach-Object {
    $f = $_.Split(',')
    [pscustomobject]@{ sec = [long]$f[0]; pid = [int]$f[1]; key = "$($f[2]):$($f[3])"; count = [int]$f[4] }
}
$summary = foreach ($t in (Get-Content $Truth | ConvertFrom-Json)) {
    $s0 = [math]::Ceiling($t.t0); $s1 = [math]::Floor($t.t1)
    $n = $s1 - $s0
    if ($n -lt 1) { continue }
    $mine = $rows | Where-Object { $_.pid -eq $t.pid -and $_.sec -ge $s0 -and $_.sec -lt $s1 }
    $rates = $mine | Group-Object key | ForEach-Object {
        [pscustomobject]@{ key = $_.Name; rate = ($_.Group | Measure-Object count -Sum).Sum / $n }
    }
    $match = $rates | Where-Object { $t.fps -gt 0 -and [math]::Abs($_.rate - $t.fps) / $t.fps -le 0.02 } | Sort-Object key
    [pscustomobject]@{
        run      = $t.tag
        true_fps = [math]::Round($t.fps, 1)
        matching = (($match | ForEach-Object { "$($_.key)=$([math]::Round($_.rate,1))" }) -join '  ')
    }
}
$summary | Format-Table -AutoSize -Wrap
