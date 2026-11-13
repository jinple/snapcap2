SnapCap
=======

Shows the name and frame rate of a game or window, and records it to MP4 with
only that program's sound.

Needs Windows 10 version 2004 or later, or Windows 11. Nothing to install:
unzip anywhere and run snapcap.exe.


Starting
--------
1. Run snapcap.exe. It lives in the tray (the ^ arrow next to the clock); it
   has no window of its own.
2. Windows asks for administrator permission for snapcap-fps.exe. This small
   helper reads the exact frame rate. It says "unknown publisher" because the
   program is not signed.
   - Yes: exact FPS.
   - No: SnapCap still works. FPS is then counted from frames reaching the
     screen, cannot read above the screen's refresh rate, and is marked
     "(capped)". You can stop being asked from the tray menu.
   snapcap.exe itself never runs as administrator. Nothing is installed and no
   Windows setting is changed.


Using it
--------
SnapCap follows the window in focus: click a game or window and the name and
FPS appear on it. Things that take focus briefly (Start menu, Alt+Tab, the
taskbar) are ignored. A recording keeps its window until you stop it.

To stay on one window instead, press Ctrl+Shift+F9 on it or pick it in the
tray menu. That turns following off; "Follow the window in focus" in the tray
menu turns it back on.

Ctrl+Shift+F9    Stay on the window in front (again on the same window: detach)
Ctrl+Shift+F10   Show or hide the name and FPS on screen
Ctrl+Shift+F11   Start or stop recording
Ctrl+Shift+F12   Show or hide the name and FPS in the video

Everything is also in the tray menu, including a list of windows to attach to.

A red dot on the overlay means recording. The dot is never in the video.
Recordings go to Videos\SnapCap as "<Name> <date time>.mp4".
The video is the window's contents only, with only that program's sound.

"Show on screen" and "Show in video" are independent. Text drawn into a video
cannot be removed afterwards, so turn "Show in video" off first if you want a
clean recording.


Settings
--------
Tray menu > "Settings...". Press Save and the changes apply straight away;
recording settings are used by the next recording. While the settings window
is open the hotkeys are switched off, so you can press them into the boxes.

The same settings are kept in snapcap.ini, next to snapcap.exe. If you edit
that file by hand, restart SnapCap afterwards. Its entries:

HotkeyAttach, HotkeyShowOnScreen, HotkeyRecord, HotkeyShowInVideo
    For example Ctrl+Shift+F9 or Ctrl+Alt+R. Keys: F1-F24, A-Z, 0-9, Insert,
    Home, End, PageUp, PageDown, Pause, ScrollLock. None turns a hotkey off.
Corner
    TopLeft, TopRight, BottomLeft or BottomRight. Used on screen and in the video.
VideoFps
    60 or 30.
VideoHeight
    0 records at the window's own size. 1440, 1080, 720 and so on scale the
    video down to that height. A 4K recording at the default quality is about
    365 MB a minute.
VideoBitrateMbps
    0 picks a quality from the video's size (about 12 for 1080p, 50 for 4K).
    Lower numbers make smaller files.
OutputFolder
    Empty means Videos\SnapCap.
ShowOnScreen, ShowInVideo, AskForExactFps, FollowFocus
    1 or 0. These are also changed from the tray menu.
ForceBorderless
    1 or 0, off by default. The overlay can't show over exclusive fullscreen.
    With this on, when the attached game is in exclusive fullscreen SnapCap
    presses Alt+Enter once, which makes most games a window. It then removes
    the window's title bar and borders and stretches it over the monitor.
    If the game ignores Alt+Enter you get a notice to change it in the game's
    own settings. Only Unity windows and windows seen in exclusive fullscreen
    are changed. Don't press Alt+Enter yourself afterwards: that returns the
    game to exclusive fullscreen.


Good to know
------------
- The video keeps the size the window had when recording started. If the
  window changes shape, the picture is scaled to fit with black bars.
- A recording stops by itself when its window closes or SnapCap exits, and the
  file is finished properly. If SnapCap is killed from Task Manager while
  recording, that recording is lost.
- Web browsers show 0 fps: they draw from a different program than the one
  that owns the window. Games are not affected.
- A window that is not redrawing shows 0 fps. That is correct.
- Fullscreen OpenGL and Vulkan games: the on-screen overlay may not appear
  (name and FPS can still go in the video), the mouse pointer is not recorded,
  and occasionally the recording shows a frozen picture. If that happens,
  record again or run the game in a window.
- Windows 10 draws a yellow border around a window while it is being recorded.


Tested on
---------
One PC: Windows 11, NVIDIA RTX 3080, 3840x2160 screen. AMD and Intel graphics
are untested.
