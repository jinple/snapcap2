# Step 1 test. Runs snapcap.exe against the step 0 test window (and nothing else).
#   -Part A : no administrator prompt. Capped FPS, overlay position, follow, hide.
#   -Part B : one administrator prompt. Exact FPS on each graphics API.
# The test picture is steady dark grey; nothing flashes.
param([ValidateSet('A', 'B')][string]$Part = 'A')
$root = Split-Path -Parent $PSScriptRoot
$bin = "$root\bin"
$tw = "$root\spike\bin\testwin.exe"
$res = "$PSScriptRoot\results"
New-Item -ItemType Directory -Force $res | Out-Null

Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Runtime.InteropServices;
public static class W {
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr SetProcessDpiAwarenessContext(IntPtr c);
    // Picture of one window we own (the overlay), not of the screen.
    public static void Snapshot(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        using (var bmp = new Bitmap(Math.Max(1, r.R - r.L), Math.Max(1, r.B - r.T))) {
            using (var g = Graphics.FromImage(bmp)) { IntPtr dc = g.GetHdc(); PrintWindow(h, dc, 2); g.ReleaseHdc(dc); }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
    }
}
'@
[void][W]::SetProcessDpiAwarenessContext([IntPtr](-4))

function OverlayState($target) {
    $o = [W]::FindWindowW('SnapCapOverlay', $null)
    $or = New-Object W+RECT; [void][W]::GetWindowRect($o, [ref]$or)
    $p = New-Object W+POINT; [void][W]::ClientToScreen($target, [ref]$p)
    [pscustomobject]@{ visible = [W]::IsWindowVisible($o); dx = $or.L - $p.X; dy = $or.T - $p.Y; w = $or.R - $or.L; h = $or.B - $or.T }
}
function StopSnapCap {
    $m = [W]::FindWindowW('SnapCapMain', 'SnapCap')
    if ($m -ne [IntPtr]::Zero) { [void][W]::PostMessageW($m, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }  # WM_CLOSE
    Start-Sleep -Seconds 3
    $left = Get-Process snapcap, snapcap-fps -ErrorAction SilentlyContinue
    "after exit, still running: " + $(if ($left) { ($left.Name -join ', ') } else { 'nothing' })
}
function RunWin([string]$api, [int]$fps, [string]$tag, [string]$truth, [switch]$Borderless, [int]$seconds = 8) {
    $a = @('--api', $api, '--fps', $fps, '--seconds', $seconds, '--out', $truth, '--tag', $tag)
    if ($Borderless) { $a += '--borderless' }
    Start-Process $tw -ArgumentList $a -Wait -WindowStyle Hidden
    Start-Sleep -Milliseconds 1200
}
function Report([string]$truth, [string]$log) {
    $rows = Get-Content $log | ConvertFrom-Json
    Get-Content $truth | ConvertFrom-Json | ForEach-Object {
        $t = $_
        $in = $rows | Where-Object { $_.t -ge ($t.t0 + 1.5) -and $_.t -le $t.t1 -and $_.attached -eq 1 }
        $f = $in | Measure-Object fps -Average -Minimum -Maximum
        [pscustomobject]@{
            run = $t.tag; true_fps = [math]::Round($t.fps, 2); shown_avg = [math]::Round($f.Average, 2)
            shown_min = $f.Minimum; shown_max = $f.Maximum; source = ($in.source | Select-Object -Unique) -join '/'
            samples = $f.Count; overlay_visible = ($in.overlay_visible | Select-Object -Unique) -join '/'
            text = ($in | Select-Object -Last 1).text
        }
    } | Format-Table -AutoSize
}

if (Get-Process snapcap, snapcap-fps, testwin -ErrorAction SilentlyContinue) { 'snapcap or testwin already running; stop them first'; exit 1 }

if ($Part -eq 'A') {
    $truth = "$res\a-truth.jsonl"; $log = "$res\a-log.jsonl"
    Remove-Item $truth, $log, "$res\overlay.png" -ErrorAction SilentlyContinue
    Start-Process "$bin\snapcap.exe" -ArgumentList '--no-admin', '--attach-title', 'snapcap-testwin', '--log', "`"$log`""
    Start-Sleep -Seconds 1

    # overlay behaviour on one window we move, minimise and restore
    $p = Start-Process $tw -ArgumentList '--api', 'dx11', '--fps', 45, '--seconds', 14, '--out', $truth, '--tag', 'dx11-45-capped' -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 3
    $t = [W]::FindWindowW('SnapCapTestWin', 'snapcap-testwin-dx11')
    "=== overlay behaviour ==="
    $s = OverlayState $t; "attached:        visible=$($s.visible) offset=($($s.dx),$($s.dy)) size=$($s.w)x$($s.h)"
    [W]::Snapshot([W]::FindWindowW('SnapCapOverlay', $null), "$res\overlay.png")
    [void][W]::SetWindowPos($t, [IntPtr]::Zero, 500, 300, 0, 0, 0x15)   # move only
    Start-Sleep -Milliseconds 400
    $s = OverlayState $t; "after move:      visible=$($s.visible) offset=($($s.dx),$($s.dy))"
    [void][W]::SetWindowPos($t, [IntPtr]::Zero, 0, 0, 900, 600, 0x16)   # resize only
    Start-Sleep -Milliseconds 400
    $s = OverlayState $t; "after resize:    visible=$($s.visible) offset=($($s.dx),$($s.dy))"
    [void][W]::ShowWindow($t, 6)                                        # minimise
    Start-Sleep -Milliseconds 600
    $s = OverlayState $t; "minimised:       visible=$($s.visible)"
    [void][W]::ShowWindow($t, 9)                                        # restore
    Start-Sleep -Milliseconds 600
    $s = OverlayState $t; "restored:        visible=$($s.visible) offset=($($s.dx),$($s.dy))"
    $p.WaitForExit()
    Start-Sleep -Milliseconds 1200
    $s = [W]::IsWindowVisible([W]::FindWindowW('SnapCapOverlay', $null)); "target closed:   visible=$s"

    RunWin dx11 30 'dx11-30-capped' $truth
    RunWin dx11 120 'dx11-120-capped' $truth
    RunWin gl 45 'gl-45-capped' $truth
    RunWin vk 45 'vk-45-capped' $truth
    StopSnapCap
    "=== FPS without the helper (capped at 60 on this screen) ==="
    Report $truth $log
}

if ($Part -eq 'B') {
    $truth = "$res\b-truth.jsonl"; $log = "$res\b-log.jsonl"
    Remove-Item $truth, $log -ErrorAction SilentlyContinue
    Start-Process "$bin\snapcap.exe" -ArgumentList '--attach-title', 'snapcap-testwin', '--log', "`"$log`""
    # wait for the administrator prompt to be answered
    $state = 1; $w = 0
    while ($state -eq 1 -and $w -lt 90) {
        Start-Sleep -Seconds 1; $w++
        $last = Get-Content $log -Tail 1 -ErrorAction SilentlyContinue
        if ($last) { $state = ($last | ConvertFrom-Json).helper }
    }
    if ($state -ne 2) { "EXACT-FPS TEST NOT RUN: helper state $state (3 = prompt declined, 4 = failed, 1 = prompt not answered)"; StopSnapCap; exit 2 }
    "helper running: " + [bool](Get-Process snapcap-fps -ErrorAction SilentlyContinue)
    foreach ($fps in 30, 60, 144, 240) { RunWin dx11 $fps "dx11-$fps" $truth }
    foreach ($api in 'dx12', 'gl', 'vk') { foreach ($fps in 60, 144) { RunWin $api $fps "$api-$fps" $truth } }
    RunWin dx11 144 'dx11-144-fullscreen' $truth -Borderless
    RunWin gl 144 'gl-144-fullscreen' $truth -Borderless
    StopSnapCap
    "=== exact FPS through the helper ==="
    Report $truth $log
}
