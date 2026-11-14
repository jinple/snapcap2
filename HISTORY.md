# SnapCap: design history

This file replaces the earlier planning and results notes (PLAN, PLAN2, PLAN3, PLAN4, STEP1, STEP2, the recorder findings summary and the step 0 results). It keeps what was decided, why, what was measured, and what is still open. The full originals are in git history (last present in commit `a612c4e`). For using the program, see `README.txt`.

## 1. How the design got here

**PLAN (first plan).** Ambitious: an in-process graphics hook (DX11/DX12, exclusive fullscreen), a GPU-side copy into a shared texture, an out-of-process encoder, an in-game overlay drawn into the backbuffer, tiered fallbacks (WGC, Desktop Duplication, ETW FPS), instant replay, per-game profiles, Vulkan/OpenGL/DX9, an installer. Requirements: lightweight, wide compatibility, exclusive fullscreen, an FPS overlay that is always available, and video plus game-only audio.

**Findings from an earlier prototype** (a gameplay recorder for QA use) contradicted parts of that plan:
- Copying the backbuffer on every Present stalled the game's GPU pipeline. Copying only on demand fixed it.
- A window overlay cost about 9% fps on a GPU-bound fullscreen game (about 4% windowed). An overlay drawn by a hook in the game cost nothing measurable.
- Recording with CPU readback used about 130% of one core at 1080p60. GPU conversion into the hardware encoder used about 23%.
- Some DX11 games load `d3d12.dll` (DLSS), which fooled detection by loaded module. Unreal-style games often default to DX12 and use 10-bit backbuffers.
- Windows event tracing (ETW) gives exact FPS for any API with no injection, but needs administrator rights or a specific group.
- Safety lessons: an installer step that changed the user's account group and asked for a sign-out ended in a temporary-profile sign-in problem; an automated test recorded a private window that happened to be focused.

**PLAN2** reacted to those findings: on-demand copy, API detection by device creation, ETW opt-in, a much smaller scope, and hard safety rules. **PLAN3** objected that PLAN2 gave up requirements to get there, and argued for a larger plan (a compositor-side overlay in recordings, paced non-blocking copies, tiered capture, Vulkan layer, about 20 to 23 weeks).

**PLAN4 (the one that was built).** Started again from six simple requirements:
1. Small (50 MB limit).
2. Works with any window or game.
3. Shows the framerate over the window.
4. Shows the application name over the window.
5. No elevated permissions without asking first.
6. A record toggle producing MP4, with audio from the recorded application only.

The idea: **no hook, no injection, no vendor SDKs.** Everything is done from outside the target with APIs that ship in Windows, so there is no per-graphics-API code at all:

| Job | Windows API |
|---|---|
| Exact frame count | ETW present events (kernel graphics provider, event 184), read by a small elevated helper |
| See the window's frames | Windows Graphics Capture (window mode) |
| Draw FPS and name | A click-through topmost window that follows the target |
| Encode and write MP4 | Media Foundation sink writer, hardware encoder, frames stay on the GPU |
| Application-only audio | WASAPI process loopback (target process and its children) |

Two programs: `snapcap.exe` (everything visible, never elevated) and `snapcap-fps.exe` (the frame counter, the only part that may run as administrator). The same `Name | FPS` text is drawn onto each video frame just before encoding, independent of the on-screen overlay ("Show on screen" and "Show in video" are separate settings). A recording is taken from the window, not the screen, so the on-screen overlay can never leak into a video that has it switched off.

## 2. What was measured

All on one NVIDIA machine (RTX-class card, Windows 11, 3840x2160 at 60 Hz) against test windows that present at a known rate. AMD and Intel hardware were never tested.

**Exact FPS from the helper** (ETW event 184): equal to the true rate at 30, 60, 144 and 240 fps on DX11, DX12, OpenGL and Vulkan, windowed and fullscreen, and not capped at the refresh rate. The helper reads that one event directly (about 150 lines, no third-party code), instead of using PresentMon's library as PLAN4 first said.

**FPS without the helper** (capture-frame arrivals): accurate to about 1.5% below the refresh rate, capped at it above, and marked "(capped)" in the overlay. It wandered by a frame or two and was once 14% low, so it is a fallback, not a measurement. It gets nothing from fullscreen OpenGL or Vulkan windows.

**Cost to the target** (GPU-bound DX11 scene):

| Condition | 2560x1440 windowed | 3840x2160 fullscreen |
|---|---|---|
| Overlay window | -0.2% (noise) | -0.2% (noise) |
| Capture session only | -2.5% | -3.4% |
| Capture and record to MP4 | -6.4% | -7.0% |

The earlier prototype's 4% and 9% overlay costs were not reproduced. Recording used about 10% of one CPU core, audio included. Nothing was tuned.

**Hardware encoder:** the NVIDIA encoder took capture textures directly on the GPU. 600 of 600 frames at 1924x1126, 60 fps.

**Recording tests** (checked by decoding the MP4 with Windows): windowed 467 of 467 frames; resize during recording gives black bars and a constant video size; fullscreen DX11 at 4K 344 of 344 frames; SnapCap exiting mid-recording still leaves a playable file.

**Audio:** a 1000 Hz tone from the target was present in the recording (0.05 to 0.09) and a 3000 Hz tone from another program was absent (0.0000). The tone ended 12 ms apart in file and window. A recording of a silent window has a full-length silent track.

## 3. Problems found and what was done

1. **Fullscreen OpenGL and Vulkan.** Window capture and monitor capture receive no frames from them. Desktop Duplication does, at the true rate and with correct pixels. So the recorder counts frames from both methods for 0.7 s when the window covers its whole monitor, and uses Desktop Duplication only when window capture is clearly starved. Desktop Duplication copies frames only while the target covers the screen and holds the last good frame otherwise. The pointer is not in those recordings.
2. **The overlay over true exclusive fullscreen.** A topmost window cannot draw there. Fullscreen OpenGL and Vulkan behave this way on the test machine (read from pixels, not seen by eye); DX11 and DX12 fullscreen were fine. This is why the "force borderless" setting exists (section 5).
3. **A freeze when the attached window closed** (once in about 45 closes, no helper). The likely cause, shutting down the capture session, now runs on a separate thread. 40 of 40 closes then completed. A mitigation, not a proven cure.
4. **22 fps on a real game.** A 4K fullscreen Unreal game recorded at 22.5 fps instead of 60. When the game keeps the GPU saturated, Windows shares it out in turns, and the first recorder needed about three turns per video frame. The recorder was reworked to stop copying captured frames and to convert to the encoder's format in the same batch as the drawing. Windowed recording then held the game's rate up to 60. Fullscreen on the test window reached only about half. The real game then recorded at 60.00 fps (48.5 Mbit/s, about 365 MB a minute at 4K), which the saturated test window did not predict.
5. **A 0.89 fps build.** Backing out an attempt to switch capture methods mid-recording left both methods running for fullscreen games and stalled the recorder. Restoring the removed lines fixed it.

## 4. Known limits and open items

- Fullscreen Vulkan sometimes picks the wrong capture method and records a frozen picture (twice in about a dozen runs). The choice is made once, at the start, and Windows seems to change how it presents a fullscreen Vulkan window from moment to moment. An attempt to switch during recording made DX11 worse and was backed out.
- Under a saturated GPU, fullscreen recording can fall short of the game's rate. Untried remedies: a frame limit or V-sync in the game, or a higher GPU priority for the recorder (Windows only allows that from an administrator process).
- Programs that draw from another process (web browsers) show 0 fps with the helper. FPS is counted for the process that owns the window.
- A recording is lost if SnapCap is killed (not exited) mid-recording, because a standard MP4 is completed on close. A crash-safe variant exists but seeks badly in some players.
- A game switched between windowed and fullscreen during a recording is not handled. A minimised window freezes the picture until restored.
- Text burned into a video cannot be removed afterwards. Windows 10 shows a yellow border around a captured window.
- Untested: real key presses and tray clicks (tests used switches running the same code), AMD and Intel encoders, two-GPU machines, long recordings (longest test 11 s), the whole-screen guard being tripped, the NVIDIA driver setting that presents OpenGL and Vulkan through DXGI (it should make fullscreen OpenGL behave like DX11).

## 5. Decisions made along the way

- Exact FPS through an elevated helper; if declined, SnapCap still works with the capped count. Never change accounts, groups, services or system settings; elevation limited to the FPS helper; automated tests only touch windows the test itself created.
- If the window changes shape during a recording, the picture is scaled to fit with black bars and the video size does not change.
- **Follow focus** (added at the user's request): SnapCap attaches to whichever window is in focus by default, which reverses the original "never follows focus". Picking a window by hotkey or menu turns following off, and a recording keeps its window.
- **Settings window** inside `snapcap.exe` (standard Windows controls, changes apply on Save), chosen over a separate program to keep the package small. Settings are kept in `snapcap.ini` (hotkeys, corner, 30 or 60 fps, video height, bitrate, output folder).
- **Force borderless** (setting `ForceBorderless`, off by default): when the attached game is in true exclusive fullscreen, SnapCap presses Alt+Enter once to make it a window, then strips the title bar and borders and fits it to its monitor. Unity re-applies its windowed size after the switch, which pushed the bottom edge off the screen (3862x2216 on a 4K monitor), so the size is re-checked for 6 seconds. Only Unity windows and windows seen in exclusive fullscreen are changed.
- **Helper without the prompt:** the helper is now started as the normal user first, and the administrator prompt is shown only if that fails. It works when the account is in the "Performance Log Users" group, which the user adds once themselves (README has the command). This is a deliberate, user-performed change, not something SnapCap does.
- **Installer:** a per-user Inno Setup installer (`installer\snapcap.iss`, built by `package.ps1`) alongside the portable zip. No administrator prompt, optional "Start SnapCap with Windows", settings kept on uninstall.

## 6. Method lessons

- Benchmark with a GPU-bound, uncapped, fullscreen scene you control, not menu scenes (capped, noisy, shader compilation).
- Use a discarded warm-up, a settling period, many samples, and reversed run orders (A/B/B/A). A single-order comparison gave a misleading result.
- Cross-check: count unique frames in the recorded file, and compare overlay fps with an independent count. Confirm a dip with two independent measures before blaming the recorder.
- A test window that never saturates the GPU hides real-game problems. The 22 fps case only appeared with a load test.
