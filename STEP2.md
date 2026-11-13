# Step 2 results: recording

> **Update: fullscreen confirmed on the real game.** After the fix described at the end ("The 0.89 fps build"), a 61 s recording of the same 4K fullscreen Unreal game came out at 60.00 frames a second, 48.5 Mbit/s video, 193 kbit/s stereo audio (read from the file's properties by the user). How it looks and sounds in playback has not been reported yet. The note below is the earlier status.
>
> **Status after the first real-game recording (12 Nov, late): not finished.** A 4K fullscreen Unreal game recorded correctly in picture, text and sound but at 22.5 frames a second instead of 60. The recorder was reworked for that (section "Real game" at the end). Windowed recording is now confirmed good under a busy graphics card. **Fullscreen recording with the reworked recorder is unconfirmed**: the last test runs gave very low frame counts, but other programs were running on the PC by then, so those runs prove nothing either way. The next step is one short recording of the real game. Sections below this note describe the tests before the rework unless they say otherwise.

Built and tested on the step 0 machine (RTX 3080, Windows 11, 3840x2160 at 60 Hz). Tested only against the step 0 test window. The test picture is steady; the tests play quiet tones.

## What was added

- **Record on/off:** Ctrl+Shift+F11, or *Start recording* / *Stop recording* in the tray menu. A red dot shows on the on-screen overlay while recording. The dot is never in the video.
- **Output:** `Videos\SnapCap\<Name> <date time>.mp4`. H.264 video at 60 fps, AAC audio at 192 kbit/s. A notification says where the file was saved.
- **Audio:** only the attached window's program and the programs it started. Nothing else playing on the PC.
- **Show in video:** Ctrl+Shift+F12 or the tray menu. Draws the same `Name | FPS` text onto the video. Independent of *Show on screen*. Can be changed during a recording.
- **Picture:** the window's contents only, without title bar or border. The video keeps the size the window had when recording started; if the window changes shape later, the picture is scaled to fit with black bars.
- A recording stops by itself when its window closes, when you detach, or when SnapCap exits, and the file is finished properly in each case.

`snapcap.exe` is now 307 KB. Total 0.45 MB.

## Test results

All from `tests\test-step2.ps1`, checked by opening each MP4 with Windows' own decoder (`tests\mp4check.exe`).

| Test | Result |
|---|---|
| Windowed 1280x720, 7.8 s | 467 of 467 frames, none dropped, hardware encoder. File is H.264 + AAC, video and audio the same length. |
| Picture | Edges and centre are the test picture's colour; no title bar or border in the frame. |
| Text in video on | Name and FPS box present in the frame (viewed: `tests\results\frame-topleft.png`). |
| Text in video off | Box absent. |
| Window resized during recording | Video stays 1280x720, picture scaled with black bars left and right, recording continues. |
| Fullscreen DX11, 3840x2160, 5.7 s | 344 of 344 frames, none dropped, hardware encoder, picture and text correct. |
| SnapCap exited during a recording | File finished and plays: 305 frames, 5.1 s. |
| Fullscreen OpenGL, 3840x2160, 4.7 s | Whole-screen capture chosen. 281 of 281 frames, hardware encoder, picture, text and sound correct. |
| Fullscreen Vulkan, 3840x2160, 4.7 s | Whole-screen capture chosen. 279 of 279 frames, same result. |
| Is the video really moving? | The test picture's small square moves 40 px a second. In the DX11, OpenGL and Vulkan fullscreen videos it moved 80 px between 2 s and 4 s, as it should. |

**Fullscreen OpenGL and Vulkan: a fault found and fixed.** The first version chose the capture method by whether window capture delivered a first frame. It does deliver one from a fullscreen OpenGL window, then almost nothing, so the first recording was a valid file showing a frozen picture. The recorder now counts what both methods deliver for 0.7 s when the window covers the whole screen, and uses whole-screen capture only when window capture is clearly starved:

| Fullscreen window at 60 fps | Window capture | Whole-screen capture | Chosen |
|---|---|---|---|
| DX11 | 26 frames | 42 frames | Window capture |
| OpenGL | 7 frames | 21 frames | Whole-screen |
| Vulkan | 6 frames | 24 frames | Whole-screen |

The rule is "window capture under about 30 a second and whole-screen well ahead of it". DX11's 26 against a cut-off of 20 is a thin margin; if it ever went the other way the recording would still be correct but without the mouse pointer. The choice is made once, when recording starts: a game switched between windowed and fullscreen during a recording is not handled.

Whole-screen capture ran only while the test window covered the entire screen, as agreed.

**Application-only audio.** The test window played a 1000 Hz tone while a different program played a 3000 Hz tone at the same loudness.

| Tone | Strength in the recording |
|---|---|
| Test window's 1000 Hz | 0.05 to 0.09 (present) |
| Other program's 3000 Hz | 0.0000 (absent) |

**Sound timing.** The tone ended 5.860 s into the file; the window stopped it 5.872 s after recording began. That is 12 ms apart. The tone's start was 0.31 s later in the file than the moment the window asked Windows to play it, in both runs; the matching end time says that delay is Windows starting the sound, not the recorder. Sync has not been judged by eye and ear on real content.

**Silence.** The tone began 3 s into the recording, so the file starts with 3 s of filled silence and the sound still lands in the right place. A recording of a silent window has a full-length silent audio track.

**Cost to the recorded program** (`tests\cost-step2.ps1`: GPU-bound DX11 test window at 2560x1440, discarded warm-up, base/record/record/base twice):

| Condition | Avg fps | Range | Change |
|---|---|---|---|
| Nothing attached | 60.29 | 59.79 to 60.54 | |
| SnapCap recording, overlay shown | 56.37 | 55.52 to 58.53 | -6.5% |

- No frames dropped in any of the four 11-second recordings.
- `snapcap.exe` used about 10% of one CPU core while recording, audio included.
- This matches step 0's unoptimised figure. Nothing has been tuned yet.

## Not tested

| Item | Why |
|---|---|
| The whole-screen guard being tripped | Nothing was put over the test window during a recording, so "holds the last good frame when something else shows" is built but not exercised. |
| Fps cost of whole-screen capture | Not measured. |
| The recording hotkeys and tray items | Need real key presses and clicks. Tests use test-only switches that run the same code. |
| Any real game | Tests only touch the test window. |
| Sound from a program the target started | The test window plays its own sound. |
| Sync by eye and ear | Only the tone timing above. |
| AMD and Intel encoders, PCs with two graphics cards | No such hardware here. |
| Long recordings | Longest test was 11 s. |
| FPS shown while recording without the helper | It read 55 at a true 60 in the frame that was looked at. With the helper the number comes from the helper as before. |

## Known limits of this step

- The mouse pointer is in the video for normal recordings, but not when whole-screen capture is used (fullscreen OpenGL / Vulkan).
- Whole-screen capture holds the last good frame whenever anything other than the game is showing, so nothing else on screen is recorded.
- A window minimised during recording freezes the picture until it is restored. Sound continues.
- A recording is lost if SnapCap is killed (not exited) mid-recording, as PLAN4 says.
- If the program's audio cannot be opened, the video is recorded without sound and the notification says so.
- The text in the video is small at low resolutions (14 px high at 720p). It scales with the video height.
- Test recordings are in `Videos\SnapCap`. They are a few hundred KB each and can be deleted.

## Real game: the 22 fps problem and the rework

**What happened.** A 6 min 49 s recording of a fullscreen 3840x2160 Unreal game (69 fps in game, exact FPS helper running) came out at an average of 22.5 video frames a second, 913 MB. Picture, name/FPS text and file structure were correct.

**Cause, as far as it is established.** When a game keeps the graphics card fully busy, Windows shares the card out in turns. The first recorder needed about three turns per video frame (copy the captured frame, draw and scale it, convert it for the encoder), so it got roughly a third of the game's frame rate. The test window had never kept the card that busy at fullscreen, so the tests missed it. Reproduced with `tests\load-step2.ps1`.

**Rework.** The recorder no longer copies captured frames, and it converts to the encoder's format itself in the same batch as the drawing, so the Windows encoder pipeline adds no conversion stage.

| Test window, graphics card saturated | Game fps | Video fps before | Video fps after |
|---|---|---|---|
| Windowed 2560x1440 | 62.6 | not measured | 60.0 |
| Windowed 2560x1440 | 28.4 | not measured | 29.8 |
| Windowed 2560x1440 | 13.0 | not measured | 15.8 |
| Windowed 3800x2000 | 72.3 | not measured | 53.1 |
| Fullscreen 3840x2160 | about 68 | about a third of game fps | 37 to 39 |
| Fullscreen 3840x2160 | 29.4 | 15.6 | 17 to 18 |

- Windowed: the video now keeps the game's frame rate up to 60. A video cannot usefully have more frames than the game produced.
- Fullscreen: improved from about a third to about half of the game's rate, still short of 60. Making the video smaller (1080p, 1440p) did not help, and neither did skipping the capture-method check, so the remaining loss is tied to fullscreen itself, not to size.
- After the rework the standard tests passed again (windowed, text off + resize, fullscreen DX11 4K 283 of 283 frames, OpenGL, colours unchanged), in runs before other programs were started on the PC.

**Fullscreen Vulkan is unreliable.** Over about a dozen runs, fullscreen Vulkan picked the wrong capture method twice and recorded a frozen picture. Windows appears to switch how it shows a fullscreen Vulkan window from moment to moment, so a choice made once at the start cannot be fully reliable. An attempt to keep both capture methods open and switch during the recording made fullscreen DX11 worse and was backed out.

**Open, in order of importance.**
1. Confirm fullscreen recording on the real game with the reworked recorder (one short recording).
2. The remaining fullscreen shortfall under a saturated graphics card. Two known remedies, neither tried: limiting the game's frame rate or turning on V-sync so the card has headroom; or raising the recorder's graphics priority, which Windows only allows from an administrator process, so it would be a new use of the elevated helper and needs approval first.
3. Fullscreen Vulkan choosing the wrong capture method some of the time.
4. File size: the 4K recording was about 134 MB a minute. A lower-resolution or lower-bitrate setting belongs in step 3.

## The 0.89 fps build

The user's second real recording came out at 0.89 frames a second. Cause: when the "switch capture method during the recording" attempt was backed out, the part that closes whole-screen capture after window capture is chosen was not restored, so both ran together for fullscreen games and the recorder stalled. Restoring those lines fixed it: the fullscreen test window recorded 302 frames in 5.0 s, and the real game then recorded at 60.00 fps.

The real game did better than the saturated test window predicted (60 against 37 to 39). Why is not known; the test window's load is a single very heavy draw, which may share the graphics card less well than a real game does.

Still open: fullscreen Vulkan sometimes choosing the wrong capture method, and file size (48.5 Mbit/s is about 365 MB a minute at 4K).
