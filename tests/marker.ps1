# Finds the test picture's small drifting square (it moves 40 px a second along
# the top edge) in two frames of a recording, to show the video is really moving.
param([string]$File, [double]$T1 = 2, [double]$T2 = 4)
Add-Type -AssemblyName System.Drawing
$tmp = "$PSScriptRoot\results\marker.bmp"
function MarkerX([double]$t) {
    & "$PSScriptRoot\bin\mp4check.exe" $File --at $t --bmp $tmp | Out-Null
    $b = [System.Drawing.Bitmap]::FromFile($tmp)
    $first = -1; $last = -1
    for ($x = 0; $x -lt $b.Width; $x++) { $p = $b.GetPixel($x, 10); if ($p.R -ge 34) { if ($first -lt 0) { $first = $x }; $last = $x } }
    $b.Dispose(); Remove-Item $tmp
    if ($first -lt 0) { return $null }
    [int](($first + $last) / 2)
}
$a = MarkerX $T1; $b = MarkerX $T2
if ($null -eq $a -or $null -eq $b) { "marker not found (t=$T1 -> $a, t=$T2 -> $b)"; return }
"marker at x=$a at $T1 s and x=$b at $T2 s: moved $($b - $a) px in $($T2 - $T1) s (expected about $(40 * ($T2 - $T1)), wrapping at the window's width)"
