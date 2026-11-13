# SnapCap: Plan 4 (simple)

Starts again from six requirements. Replaces PLAN.md, PLAN2.md and PLAN3.md.

## 1. Requirements

| # | Requirement |
|---|---|
| 1 | Small. 50 MB is the limit. |
| 2 | Works with any window or game. |
| 3 | Shows the framerate over the game/window. |
| 4 | Shows the application/game name over the game/window. |
| 5 | No elevated permissions. If something would need them, you are asked before it is built. |
| 6 | A record on/off toggle. Output is MP4. Audio is the recorded application only. |

## 2. The idea

**No hook, no injection, no second process, no vendor SDKs.** Everything is done from outside the target with four APIs that ship in Windows and work on any window regardless of what it renders with:

| Job | Windows API | Elevation |
|---|---|---|
| Count the window's frames exactly | Windows event tracing of present events, read by a small helper | **Admin, approved by you (section 4)** |
| See the window's frames for recording | Windows Graphics Capture (window capture) | None |
| Draw FPS and name over it | A small click-through, always-on-top window that follows the target | None |
| Encode video and write MP4 | Media Foundation Sink Writer, hardware encoder, frames stay on the GPU | None |
| Application-only audio | WASAPI process loopback (target process and its children) | None |

Two executables written in C++ against Win32 and C++/WinRT: `snapcap.exe` (everything the user sees, never elevated) and `snapcap-fps.exe` (the frame-counting helper, the only part that runs as administrator). Expected size is a few MB in total. No runtime to install.

This is what makes requirement 2 cheap: there is no per-graphics-API code at all. DX11, DX12, Vulkan, OpenGL, DX9, a browser and Notepad are all the same case.

## 3. How each requirement is met

**Choosing the target.** Press a hotkey while the window you want is in front, or pick it from the tray menu. SnapCap stays attached to that window until it closes or you choose another. It never follows focus on its own, so it never captures a window you did not pick.

**Framerate (3).** Windows logs an event every time a program presents a frame, for every graphics API. The helper reads those events and sends per-program counts to SnapCap, which shows the rate for the attached window, refreshed twice a second. This is exact, including above the monitor's refresh rate. See section 4 for how the helper is started.

**Name (4).** Taken from the program's product name (what the developer set in the executable), falling back to the window title, then the file name. Shown next to the FPS.

**Overlay.** A small borderless window in a corner of the target, transparent to mouse clicks, repositioned when the target moves or resizes, hidden when the target is minimised. Shows `Name  |  FPS` and a red dot while recording.

**Overlay in the video.** The same `Name  |  FPS` text is drawn onto each frame just before it is encoded, in the step that already scales the frame, so it costs almost nothing extra. The red recording dot is never put in the video.

**Two independent settings**, both in the tray menu and each with a hotkey: *Show on screen* and *Show in video*. Any of the four combinations works, and either can be changed while recording. Both default to on. Because the recording is taken from the window itself and not from the screen, the on-screen overlay can never leak into a video that has *Show in video* off.

**Recording (6).** Hotkey or tray menu toggles it. Frames from the same capture session are scaled to the size the recording started at, encoded as H.264 at 60 fps by whatever hardware encoder the PC has (NVIDIA, AMD or Intel, with Windows' software encoder as the fallback), and written with AAC audio to `Videos\SnapCap\<Name> <date time>.mp4`. The mouse cursor is included. Audio comes only from the target's process tree; silence is filled so sound stays in sync.

**Elevation (5).** Only the FPS helper needs it, as you approved. Nothing else does. No installer is needed: unzip and run.

**Size (1).** No bundled libraries beyond the C++ runtime, statically linked.

## 4. The admin helper (decided: exact FPS via an elevated helper)

- When SnapCap starts it launches `snapcap-fps.exe`, and Windows shows its standard administrator prompt. This happens once per SnapCap start.
- The helper does one thing: reads present events and sends frame counts per program to SnapCap. It has no window, opens no files of yours, touches no other program, and exits when SnapCap exits. It reads one Windows event directly (the one step 0 found exact on every API), so it contains no third-party code.
- `snapcap.exe` itself never runs elevated.
- **If you decline the prompt,** SnapCap still works. FPS is then counted from frames reaching the screen, which cannot read above the monitor's refresh rate, and the overlay marks the number as capped.
- A setting turns the prompt off for people who are happy with the capped number.
- Nothing is installed and no account, group or service is changed. The alternative that avoids a prompt per start (joining a Windows group, or installing a service) is exactly what the earlier findings ruled out, so it is not used.

## 5. What this design cannot do

These are the costs of dropping the hook. Each is fixable only by adding back the complexity that was just removed.

1. **No on-screen overlay where a game owns the display directly.** That means true exclusive fullscreen, and, as step 0 found on an NVIDIA card, **fullscreen OpenGL and fullscreen Vulkan**. Such a game can still be recorded (see 3a) and its FPS measured and put in the video, but the overlay very probably cannot be drawn over it. DX11 and DX12 fullscreen were fine. Running the game windowed fixes it; so should the NVIDIA driver setting that presents OpenGL/Vulkan through DXGI (untested).
2. **The overlay window has a cost.** The prototype measured about 4% fps on a windowed game and about 9% on a GPU-bound fullscreen one. With the helper running, capture runs only while recording, so showing FPS adds nothing more. If the prompt was declined, capture stays on to produce the capped count, at a cost step 0 measures.
3. **Exact FPS is confirmed** on DX11, DX12, OpenGL and Vulkan, windowed and fullscreen, on one NVIDIA machine.
3a. **Recording needs a second capture method.** Step 0 found that window capture gets no frames from a fullscreen OpenGL or Vulkan window, and that Desktop Duplication does. So: window capture normally; Desktop Duplication when window capture is starved **and** the target exactly covers its monitor. Because Desktop Duplication sees the whole screen, it copies frames only while the target covers it and holds the last good frame the moment anything else shows. Recording from it works (step 2), without the mouse pointer. See [spike/RESULTS.md](spike/RESULTS.md) sections 5 and 6.
3b. **FPS is counted for the process that owns the window.** Programs that draw from another process, such as web browsers, show 0 fps. Games are not affected.
4. **Text burned into a video cannot be removed afterwards.** Turn *Show in video* off before recording if a clean video is wanted.
5. **Windows 10 shows a yellow border** around a window while it is being captured. Windows 11 lets an app turn that off. Process-only audio needs Windows 10 version 2004 or later.
6. **A recording is lost if SnapCap is killed mid-recording**, because a standard MP4 is completed when it is closed. A crash-safe variant exists but seeks badly in some players.

## 6. Steps

**Step 0: three measurements (about a week).** Against test windows we control that draw at a known rate.
- Does the helper's FPS match the real rate below, at and well above refresh, on DX11, DX12, Vulkan and OpenGL, windowed and borderless fullscreen? Does the capped fallback match below refresh?
- What does the overlay window cost in fps, and what does capture cost while recording?
- Does the hardware encoder accept capture frames directly on NVIDIA, AMD and Intel?

**Step 1: overlay. Done, see [STEP1.md](STEP1.md).** Target selection, FPS helper and capped fallback, name, tray icon, hotkeys (Ctrl+Shift+F9 attach, Ctrl+Shift+F10 show/hide).

**Step 2: recording. Working: a real 4K fullscreen game recorded at 60 fps with sound, see [STEP2.md](STEP2.md). Open: fullscreen Vulkan sometimes picks the wrong capture method.** Video, application audio, MP4, record indicator, overlay in the video, the two show/hide settings (Ctrl+Shift+F11 record, Ctrl+Shift+F12 show in video).

**Step 3: tidy. Done.** Settings file `snapcap.ini` (hotkeys, corner, 30/60 fps, video height, bitrate, output folder), [README.txt](README.txt), and `package.ps1`, which makes `dist\SnapCap.zip` (226 KB). One test recording with non-default settings passed: 852x480 at 30 fps, text bottom-right, custom folder, sound. Custom hotkeys were read and written back correctly but not pressed.

About four to five weeks in total.

## 7. Rules kept from the earlier findings

- No changes to accounts, groups, services or system settings.
- Elevation is per launch, visible, and limited to the FPS helper.
- Automated tests capture only windows the test itself created.
- Measurements use warm-up, settling, many samples and reversed run orders.

## 8. Decisions made

- Exact FPS through the elevated helper, one administrator prompt per SnapCap start.
- FPS and name shown on screen and in the video, each with its own on/off setting.

- The helper reads the Windows present event directly instead of using PresentMon's library.

- If the window changes shape during a recording, the picture is scaled to fit with black bars; the video size does not change.

- For a fullscreen window the recorder picks the capture method by counting frames from both for 0.7 s, not by "window capture delivers nothing": step 2 found it delivers a trickle from fullscreen OpenGL and Vulkan.

- **Changed after step 3, at the user's request:** SnapCap now follows the window in focus by default (section 3 said it never would). Picking a window by hotkey or menu turns following off; a recording keeps its window. Without the helper this means the capped FPS count runs on each window that takes focus.

- **Added after step 3:** a settings window inside `snapcap.exe` (tray menu, *Settings...*), built from standard Windows controls in code. Chosen over a separate Rust program to keep the package small. Changes apply on Save without a restart.

All four steps are done. Open items: fullscreen Vulkan sometimes records a frozen picture (STEP2.md); AMD and Intel graphics are untested; the rare step 1 freeze is mitigated, not proven fixed.
