# Step 0 results

Machine: Ryzen 7 5800X (16 threads), GeForce RTX 3080, Windows 11 Pro build 26300, 3840x2160 at 60 Hz.
All measurements are against `testwin.exe`, a window we control that presents at a known rate and reports its own exact frame count. Nothing else was ever captured.

**Status: step 0 measurements done for DX11, DX12, OpenGL and Vulkan, windowed and borderless fullscreen. One problem found (section 5) and followed up (sections 6 and 7).**

The first (windowed) runs used a test picture that changed colour every frame. From the fullscreen runs onward the picture is a steady dark grey with one small dim square drifting along the top edge. No test flashes.

## 1. Exact FPS from the elevated helper (windowed)

`fpsprobe.exe`, run as administrator, counted Windows present events per second for the test window.

| API | 30 | 60 | 144 | 240 | Uncapped |
|---|---|---|---|---|---|
| DX11 | 30.0 | 60.0 | 144.0 | 240.0 | 2248 vs 2258 true (0.4% low) |
| DX12 | 30.0 | 60.0 | 144.0 | 240.0 | matches (see note) |
| OpenGL | 30.0 | 60.0 | 144.0 | 240.0 | 6110 vs 6030 true (1.3% high) |

- **One event works for all three APIs:** the kernel graphics provider's event 184. No per-API logic is needed.
- At 30 to 240 fps the count equals the true count exactly. Well above the monitor's 60 Hz refresh, so the helper is not capped.
- Uncapped runs were in the thousands of fps and the rate was unsteady, so whole-second windows do not line up perfectly with the test window's own timing. For the DX12 run the table check failed for that reason; summing the same time span by hand gives 18,900 events against 18,936 true frames (0.2%).
- The probe listened to every graphics event (21 million in about three minutes) to find the right one. The real helper will subscribe only to what it needs.

## 2. FPS without elevation (the fallback if the prompt is declined), windowed

`capprobe.exe` counted capture-frame arrivals.

| True fps | DX11 | DX12 | OpenGL |
|---|---|---|---|
| 30 | 30.0 | 30.0 | 30.0 |
| 45 | 44.7 | 44.5 | 44.3 |
| 57 | 57.0 | 57.0 | 57.0 |
| 60 | 59.8 | 59.7 | 60.0 |
| 120 | 59.7 | 58.7 | 58.8 |
| 240 | 60.0 | 59.3 | 59.0 |

Accurate to about 1.5% below the refresh rate; capped at the refresh rate above it, as expected. Windows 11 let the capture border be turned off.

## 3. Cost to the target (windowed, GPU-bound DX11 scene at 2560x1440, about 61 fps)

Discarded warm-up, then four conditions run forward and reversed, twice (4 runs each).

| Condition | Avg fps | Range | Change |
|---|---|---|---|
| Nothing attached | 61.24 | 60.96 to 61.66 | |
| Overlay window | 61.09 | 60.13 to 61.57 | -0.2% (inside the noise) |
| Capture session only | 59.70 | 59.37 to 60.00 | -2.5% |
| Capture + record to MP4 | 57.32 | 55.82 to 58.55 | -6.4% |

- The overlay window cost nothing measurable on a windowed target. The prototype's 4% was not reproduced here.
- Recording a 2564x1487 window at 60 fps cost about 6% on a GPU-bound scene. This is with no optimisation at all, and at a larger frame than 1080p.
- With the helper running, capture is only needed while recording, so showing FPS costs only the overlay.

## 4. Hardware encoding of capture frames

1924x1126 window, 60 fps, 10 seconds, H.264 at 12 Mbit/s through the Windows sink writer.

- The NVIDIA hardware encoder was selected and took the capture textures directly on the GPU. No separate conversion stage was inserted.
- 600 of 600 frames written, no errors, 9.97 MB file that Windows reads as an MP4 of the right length. The picture was not inspected by eye.
- **CPU: 9.9% of one core** (0.6% of this machine's total). The prototype's GPU path was about 23% of a core. Audio is not in this figure.
- Only NVIDIA could be tested on this machine. AMD and Intel are unverified.

## 5. Borderless fullscreen (3840x2160)

**Exact FPS from the elevated helper.** Event 184 equalled the true count exactly at 30, 60, 144 and 240 fps on DX11, DX12 and OpenGL. Same result as windowed.

**FPS without elevation.**

| True fps | DX11 | DX12 | OpenGL |
|---|---|---|---|
| 30 | 30.0 | 30.0 | **0** |
| 45 | 45.0 | 45.0 | **0** |
| 60 | 60.0 | 59.2 | **0** |
| 120 | 59.8 | 59.8 | **0** |

**Problem: window capture receives no frames at all from a fullscreen OpenGL window.** Repeated and confirmed: the same OpenGL window delivers frames when windowed (45.0 counted at 45 true) and none when it covers the screen. DX11 and DX12 fullscreen are fine. The likely cause is that the graphics driver presents fullscreen OpenGL straight to the display, bypassing the part of Windows that window capture reads from; that is a hypothesis, not something measured here. Consequences for PLAN4:
- The helper still reports exact FPS for it, so the overlay number is unaffected when the admin prompt is approved.
- The no-elevation FPS fallback does not work for it.
- **Recording it with window capture would produce nothing.** PLAN4 needs a second capture method for this case; section 6 tests two. Vulkan may behave the same way and is untested.

**Cost to the target** (GPU-bound DX11 scene at 3840x2160, about 63 fps, same protocol, 4 runs each):

| Condition | Avg fps | Range | Change |
|---|---|---|---|
| Nothing attached | 62.91 | 62.67 to 63.22 | |
| Overlay window | 62.78 | 62.59 to 63.01 | -0.2% (inside the noise) |
| Capture session only | 60.77 | 60.57 to 60.98 | -3.4% |
| Capture + record to MP4 | 58.49 | 58.24 to 58.99 | -7.0% |

- The overlay window cost nothing measurable in fullscreen either. The prototype's 9% was not reproduced on this machine with this overlay. Why they differ is not known.
- Recording here means encoding 4K at 60 fps. A file was produced; its contents and frame count were not checked in this run.

## 6. Follow-up: capturing fullscreen OpenGL

Two whole-screen capture methods were tested. Both ran only while the test window covered the entire screen (checked before starting and on every frame; the check never failed), stored no picture, and only counted frames and read single pixels. The expected background pixel is 25,28,33.

**Windows Graphics Capture, monitor mode** (`run-monitor.ps1`)

| Fullscreen window | 30 | 45 | 60 | 120 | Pixel read |
|---|---|---|---|---|---|
| OpenGL | 0 | 0 | 0 | 0 | none |
| DX11 | 30.0 | 45.0 | 59.5 | 60.0 | 25,28,33 (correct) |
| DX12 | 30.0 | 44.7 | 58.7 | 59.7 | 25,28,33 (correct) |

It does not see fullscreen OpenGL either. Not a solution.

**Desktop Duplication** (`run-dupl.ps1`)

| Fullscreen window | True fps | Frames per second received | Pixel read |
|---|---|---|---|
| OpenGL | 30 | 30.1 | 25,28,33 (correct) |
| OpenGL | 45 | 45.0 | correct |
| OpenGL | 60 | 60.2 | correct |
| OpenGL | 120 | 120.0 | correct |
| DX11 | 45 | 45.2 | correct |

**Desktop Duplication does capture fullscreen OpenGL**, with the right picture and at the true rate. It was not capped at the 60 Hz refresh rate in this test (120 received at 120 true), which also makes it a no-elevation FPS source for this case. Encoding from it and its cost to the game were not measured.

**The overlay over fullscreen OpenGL.** A pixel under the overlay's position was read in three setups:

| Fullscreen window, capture method | No overlay | Overlay shown | Overlay shown, marked "exclude from capture" |
|---|---|---|---|
| DX11, monitor capture | 25,28,33 | 17,17,18 (overlay visible) | 25,28,33 (overlay hidden from capture) |
| OpenGL, Desktop Duplication | 25,28,33 | 25,28,33 | 25,28,33 |

- On DX11 the overlay is on screen, and the "exclude from capture" flag keeps it out of a whole-screen capture. That is what keeps *Show in video* independent of *Show on screen* if whole-screen capture is ever used.
- On fullscreen OpenGL the overlay did not appear in the captured screen image at all. The most likely reading is that **the overlay is not visible on screen over a fullscreen OpenGL window** on this machine: the driver gives that window the display directly, which is the "true exclusive fullscreen" case PLAN4 already lists as unsupported. This was read from pixels, not seen by eye.

## 7. Vulkan

Vulkan headers were installed through vcpkg and a Vulkan mode added to `testwin` (`run-vulkan.ps1`). **Vulkan behaves exactly like OpenGL in every test.**

**Exact FPS from the elevated helper:** event 184 equalled the true count exactly at 30, 60, 144 and 240 fps, windowed and fullscreen.

**What each capture method receives (frames per second):**

| True fps | Window capture, windowed | Window capture, fullscreen | Monitor capture, fullscreen | Desktop Duplication, fullscreen |
|---|---|---|---|---|
| 30 | 30.2 | 3.7 | | |
| 45 | 45.0 | 0 | 0 | 45.0, correct pixel |
| 60 | 60.0 | 0 | | |
| 120 | 58.0 (capped) | 0 | 0 | 120.0, correct pixel |

The 3.7 in one fullscreen run is a handful of frames at the start, not a working capture.

**Overlay over fullscreen Vulkan:** the pixel under the overlay read 25,28,33 with and without the overlay shown (Desktop Duplication), the same as OpenGL. Very probably not visible on screen.

## 8. Not done

| Item | Why | What it needs |
|---|---|---|
| Seeing the overlay over fullscreen OpenGL by eye | I cannot see the screen | Someone watching one test run |
| The NVIDIA setting "Vulkan/OpenGL present method: prefer layered on DXGI swapchain" | It is a driver setting, not ours to change | Trying it by hand; it should make fullscreen OpenGL behave like DX11 |
| Encoding from Desktop Duplication, and its fps cost | Out of time in this step | Step 2 |
| AMD and Intel encoders | No such hardware here | Another machine |
| Application-only audio | Not part of step 0 | Step 2 |

## 9. What this means for PLAN4

- The elevated-helper design works: one event, exact, API-independent on DX11, DX12, OpenGL and Vulkan, windowed and fullscreen.
- The overlay window is effectively free on this machine, windowed and fullscreen, for DX11.
- The recording path works on the GPU with no bundled libraries, at under half the prototype's CPU cost. Its fps cost is 6 to 7% unoptimised, at 1440p and 4K.
- The no-elevation FPS fallback works for DX11 and DX12, and for windowed OpenGL.
- **Recording needs two capture methods, not one:** window capture normally, and Desktop Duplication when window capture delivers no frames and the target covers its whole monitor.
- **Fullscreen OpenGL and Vulkan are the plan's "exclusive fullscreen" limitation in practice:** they can be recorded and their FPS measured, but the on-screen overlay very probably cannot be shown over them. Windowed OpenGL and Vulkan are unaffected. All of this is from one NVIDIA machine.
