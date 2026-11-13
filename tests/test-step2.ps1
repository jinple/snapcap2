# Step 2 test. Records the step 0 test window (and nothing else) and checks the MP4.
#   -Part window     : windowed recording, text in video, application-only audio
#   -Part notext     : text off, window resized during the recording
#   -Part fullscreen : fullscreen DX11 at the screen's full size (window capture)
#   -Part screen     : fullscreen OpenGL, which needs whole-screen capture
# Plays quiet test tones. The picture is steady; nothing flashes. No administrator prompt.
param([ValidateSet('window', 'notext', 'fullscreen', 'screen')][string]$Part = 'window', [ValidateSet('gl', 'vk')][string]$ScreenApi = 'gl')
$root = Split-Path -Parent $PSScriptRoot
$bin = "$root\bin"; $tw = "$root\spike\bin\testwin.exe"; $tb = "$PSScriptRoot\bin"
$res = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force $res | Out-Null
$log = "$res\s2-$Part-log.jsonl"; $truth = "$res\s2-$Part-truth.jsonl"
Remove-Item $log, $truth -ErrorAction SilentlyContinue
if (Get-Process snapcap, snapcap-fps, testwin -ErrorAction SilentlyContinue) { 'snapcap or testwin already running; stop them first'; exit 1 }

Add-Type -TypeDefinition @'
using System; using System.Runtime.InteropServices;
public static class U {
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
'@

function Record([string[]]$snapArgs, [string[]]$winArgs, [scriptblock]$During) {
    $sc = Start-Process "$bin\snapcap.exe" -ArgumentList (@('--no-admin', '--attach-title', 'snapcap-testwin', '--log', "`"$log`"") + $snapArgs) -PassThru
    Start-Sleep -Milliseconds 700
    $w = Start-Process $tw -ArgumentList ($winArgs + @('--out', $truth)) -PassThru -WindowStyle Hidden
    if ($During) { & $During }
    $w.WaitForExit()
    Start-Sleep -Seconds 3   # the recording stops with its window, or earlier; let the file finish
    $m = [U]::FindWindowW('SnapCapMain', 'SnapCap')
    if ($m -ne [IntPtr]::Zero) { [void][U]::PostMessageW($m, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
    if (-not $sc.WaitForExit(20000)) { 'SNAPCAP DID NOT EXIT'; $sc.Kill() }
    $r = Get-Content $log | Where-Object { $_ -match '"record":1' } | Select-Object -Last 1
    if (-not $r) { 'NO RECORDING RESULT IN LOG'; return $null }
    $r | ConvertFrom-Json
}
function Check($r, [string[]]$checkArgs) {
    Write-Host "recorder: ok=$($r.ok) error='$($r.error)' $($r.w)x$($r.h) frames=$($r.frames) dropped=$($r.dropped) seconds=$($r.seconds) audio=$($r.audio) hw_encoder=$($r.hw_encoder) screen_method=$($r.screen_method)"
    if (-not $r.ok) { return $null }
    Write-Host "file: $($r.file)  ($([math]::Round((Get-Item $r.file).Length / 1MB, 2)) MB)"
    $c = & "$tb\mp4check.exe" $r.file @checkArgs | ConvertFrom-Json
    Write-Host "mp4:  $($c.w)x$($c.h) h264=$($c.h264) length=$($c.duration)s video_frames=$($c.video_frames) audio=$($c.audio) audio_seconds=$($c.audio_seconds) first_sound=$($c.first_sound)s"
    $c
}
$bg = '25,28,33 expected for the test picture'

if ($Part -eq 'window') {
    # Test window plays 1000 Hz from 7 s. A different program plays 3000 Hz throughout.
    $r = Record @('--record-after', '2', '--record-seconds', '8', '--video-text', '1') @('--api', 'dx11', '--fps', '60', '--seconds', '16', '--tone', '1000', '--tone-at', '5', '--tone-stop', '8') {
        Start-Sleep -Seconds 1
        Start-Process "$tb\tone.exe" -ArgumentList '3000', '13' -WindowStyle Hidden
    }
    if ($r) {
        $c = Check $r @('--at', '3', '--px', '2', '100', '--px', '1277', '717', '--px', '640', '360', '--px', '14', '14', '--freq', '1000', '--freq', '3000')
        if ($c) {
            "pixels at 3 s: left edge $($c.pixels[0]), bottom-right $($c.pixels[1]), centre $($c.pixels[2])   ($bg)"
            "pixel inside the name/FPS box: $($c.pixels[3])   (darker than the picture = text box is in the video)"
            "tone strength: window's own 1000 Hz = $($c.tones.'1000'), other program's 3000 Hz = $($c.tones.'3000')   (about 0.08 = present, 0 = absent)"
            $ev = Get-Content $truth | ConvertFrom-Json
            $on = ($ev | Where-Object kind -eq 'tone').t - $r.start; $off = ($ev | Where-Object kind -eq 'tone_stop').t - $r.start
            "sound in the file: $($c.first_sound) s to $($c.last_sound) s; the window asked Windows to start its tone at $([math]::Round($on, 3)) s and stopped it at $([math]::Round($off, 3)) s"
        }
    }
}

if ($Part -eq 'notext') {
    $r = Record @('--record-after', '2', '--record-seconds', '7', '--video-text', '0') @('--api', 'dx11', '--fps', '60', '--seconds', '13') {
        Start-Sleep -Seconds 6
        $t = [U]::FindWindowW('SnapCapTestWin', 'snapcap-testwin-dx11')
        [void][U]::SetWindowPos($t, [IntPtr]::Zero, 0, 0, 916, 639, 0x16)   # resize: client becomes about 900x600
    }
    if ($r) {
        $c = Check $r @('--at', '1.5', '--px', '14', '14', '--px', '640', '360')
        if ($c) { "before resize: text-box position $($c.pixels[0]), centre $($c.pixels[1])   ($bg; text is off)" }
        $c = & "$tb\mp4check.exe" $r.file --at 6 --px 20 360 --px 640 360 --px 1260 360 | ConvertFrom-Json
        "after resize:  left bar $($c.pixels[0]), centre $($c.pixels[1]), right bar $($c.pixels[2])   (bars 0,0,0; centre as before)"
    }
}

if ($Part -eq 'fullscreen' -or $Part -eq 'screen') {
    $api = if ($Part -eq 'screen') { $ScreenApi } else { 'dx11' }
    $r = Record @('--record-after', '2', '--record-seconds', '6', '--video-text', '1') @('--api', $api, '--fps', '60', '--seconds', '13', '--borderless', '--tone', '1000', '--tone-at', '0')
    if ($r) {
        $c = Check $r @('--at', '3', '--px', '1920', '1080', '--px', '100', '2000', '--px', '50', '50', '--freq', '1000')
        if ($c) { "pixels at 3 s: centre $($c.pixels[0]), lower left $($c.pixels[1]), inside text box $($c.pixels[2])   ($bg); 1000 Hz strength $($c.tones.'1000')" }
    }
}
