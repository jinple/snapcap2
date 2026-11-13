# Step 1 results: overlay with FPS and name

Built and tested on the step 0 machine (RTX 3080, Windows 11, 3840x2160 at 60 Hz). Tested only against the step 0 test window. The test picture is steady; nothing flashes.

## What exists

| File | Size | What it is |
|---|---|---|
| `bin\snapcap.exe` | 248 KB | Tray program, overlay, hotkeys. Never elevated. |
| `bin\snapcap-fps.exe` | 137 KB | Exact-FPS helper. The only part that runs as administrator. |

Source is in `src\`, built by `build.ps1`. No installer, no other files. Total 0.4 MB against the 50 MB limit.

## How to use it

1. Run `bin\snapcap.exe`. Windows shows one administrator prompt for `snapcap-fps.exe`. It says "unknown publisher" because the program is not signed.
2. Bring a game or window to the front and press **Ctrl+Shift+F9**, or right-click the SnapCap tray icon and pick it under *Attach to window*.
3. The overlay shows `Name | FPS` in the window's top-left corner. **Ctrl+Shift+F10** or the tray menu hides and shows it.
4. Ctrl+Shift+F9 on the attached window detaches. *Exit* is in the tray menu.

Declining the prompt still works: the overlay then shows a number marked `(capped)`. The tray menu can turn exact FPS on later, and can turn the prompt off for future starts. The two settings are kept in `snapcap.ini` next to the program.

## Test results

**Exact FPS through the helper** (`tests\test-step1.ps1 -Part B`, one prompt). "Shown" is what the overlay displayed, sampled twice a second.

| Test window | True fps | Shown, average | Shown, range |
|---|---|---|---|
| DX11 | 30.00 | 29.99 | 29.99 to 30.00 |
| DX11 | 60.00 | 60.00 | 59.99 to 60.01 |
| DX11 | 144.00 | 144.01 | 143.97 to 144.05 |
| DX11 | 240.00 | 239.99 | 239.93 to 240.08 |
| DX12 | 60.00 / 144.00 | 60.00 / 143.97 | within 0.11 |
| OpenGL | 60.00 / 144.00 | 60.00 / 143.99 | within 0.06 |
| Vulkan | 59.99 / 143.98 | 60.00 / 144.00 | within 0.01 |
| DX11 fullscreen | 144.00 | 144.00 | 143.99 to 144.01 |
| OpenGL fullscreen | 144.00 | 143.99 | 143.96 to 144.01 |

The helper exited by itself when SnapCap exited.

**FPS without the helper** (`-Part A`, no prompt), four runs:

| Test window | True fps | Shown, average over the runs |
|---|---|---|
| DX11 | 30 | 29.9 to 30.0 |
| DX11 | 120 | 59.6 to 59.9 (capped at the 60 Hz screen, as designed) |
| OpenGL | 45 | 45.1, 45.2, 44.4, and **38.8 in one run** |
| Vulkan | 45 | 44.5 to 45.2 |

The capped number wanders by a frame or two and was once 14% low. It is a fallback, not a measurement.

**Overlay behaviour** (all four runs the same): appears 18 px inside the window's top-left at this screen's scaling, stays there after the window is moved and resized, hides when the window is minimised, returns when restored, hides when the window closes. Picture of the overlay: `tests\results\overlay.png`.

## Problem found and handled

In one run `snapcap.exe` froze at the moment its attached window closed, while using the no-helper fallback. The frozen thread was waiting on another Windows process. It happened once in about 45 window closes and did not reproduce in 24 further tries, so the exact call was not proven. The likely one, shutting down the capture session of a closing window, now runs on a separate thread so the tray and overlay cannot freeze on it. After that change: 40 of 40 shutdowns completed (`tests\stress-close.ps1`). This is a mitigation, not a confirmed cure. It does not affect the helper path, which does not use capture.

## Not tested

| Item | Why |
|---|---|
| The two hotkeys and the tray menu | They need real key presses and clicks. The tests attach through a test-only switch that runs the same attach code. |
| Declining the administrator prompt | Needs a person to click No. The no-helper path itself was tested with a switch that skips the prompt. |
| Any real game or program | Tests only touch the test window. |
| The product-name lookup | The test window has no product name, so the tests only show the window-title fallback. |
| Overlay over fullscreen OpenGL/Vulkan | Step 0 found it very probably cannot show there. Unchanged. |

## Known limits of this step

- **FPS is counted for the process that owns the window.** A program that draws from a different process (web browsers do) will show 0 fps with the helper. Games draw from their own process.
- A window that is not redrawing shows 0 fps. That is correct, not a fault.
- The overlay sits in the top-left corner. Choosing the corner is in step 3.
- If the helper is killed from Task Manager, SnapCap drops to the capped number, and Windows keeps the helper's small event session open until the next start or a restart.

## Change from the plan

PLAN4 said the helper would be built on PresentMon's library. Step 0 showed one Windows event gives the exact count on all four APIs, so the helper reads that event directly: about 150 lines, no third-party code.
