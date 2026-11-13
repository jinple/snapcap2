# SnapCap: Implementation Plan

Super-lightweight video and audio capture for games and applications on Windows 10/11.

## 1. Requirements

| # | Requirement | Meaning in practice |
|---|---|---|
| R1 | Lightweight | Installer < 10 MB. Idle < 0.5% CPU and < 40 MB RAM. Recording costs < 3% FPS and < 2% CPU. No browser engine, no .NET runtime. |
| R2 | Wide compatibility | DX11 and DX12 first-class. Also DX10, DX9, OpenGL, Vulkan (later phases). 64-bit and 32-bit games. UWP/Store titles. |
| R3 | Exclusive fullscreen | Must capture and overlay true exclusive fullscreen, not only borderless windows. |
| R4 | FPS overlay | Always available. On-screen display toggle and in-recording toggle are **independent**. |
| R5 | Video + audio | Hardware-encoded video, game-only audio via per-process loopback, plus optional mic. |

## 2. Core technical decisions

**Language and tooling:** C++20, CMake, vcpkg, MSVC. Win32 and COM directly. UI is a tray icon plus a small Dear ImGui settings window.

**Capture model: tiered backends with automatic fallback.** Exclusive fullscreen cannot be captured by screen-based APIs, so in-process hooking is the primary path.

| Tier | Backend | Handles | Used when |
|---|---|---|---|
| 1 | Graphics hook (DLL in the game process) | DX11, DX12, exclusive FS, windowed, borderless | Default |
| 2 | Windows Graphics Capture (WGC) | Windowed, borderless, independent-flip | Game blocks injection (anti-cheat) |
| 3 | DXGI Desktop Duplication | Whole monitor | Last resort, desktop apps |

**Exclusive fullscreen:** the hook intercepts `IDXGISwapChain::Present`, `Present1` and `ResizeBuffers` (DX11/DX12) and takes a GPU-side copy of the backbuffer just before present. This works in exclusive FS, flip-model and bitblt-model swap chains.

**DX12:** hook `ID3D12CommandQueue::ExecuteCommandLists` to learn the direct queue that owns the swap chain. Copy the backbuffer with a private command list and fence-sync it.

**Zero-copy pipeline:** hook copies the frame into a shared NT-handle texture (keyed mutex for DX11, shared fence for DX12). The encoder reads that texture on the GPU. No CPU readback in the normal path.

**Process split (keeps the in-game footprint minimal and crashes isolated):**
```
snapcap.exe (tray, UI, hotkeys, settings)   - one instance
 ├─ snapcap-encoder.exe   encode + mux + audio, out of process
 ├─ snapcap-hook64.dll    injected into 64-bit games: capture + overlay only
 └─ snapcap-hook32.dll    + snapcap-helper32.exe for 32-bit games
```
The hook DLL does no encoding, no disk I/O and no allocations in the Present path beyond init. A crash in the encoder never takes the game down.

## 3. FPS overlay design (R4)

**Rendering:** overlay is drawn by the hook into the game's backbuffer immediately before Present, using a tiny DX11 renderer and a tiny DX12 renderer. Both use a pre-baked bitmap font atlas, a single draw call, and full state save/restore so the game's pipeline is untouched.

**Two independent toggles, via capture-point ordering:**
- *Show on screen:* draw overlay into the backbuffer, then Present.
- *Show in recording:* take the capture copy **after** the overlay draw. If off, copy **before** the overlay draw.
- Resulting 4 combinations (screen on/off × recording on/off) cost nothing extra.

**FPS data source:** frame times measured at Present in the hook (QPC). Shows FPS, frametime, 1% / 0.1% lows, optional CPU/GPU load. Configurable position, scale, opacity, colour, hotkey.

**"Always available" guarantee:** the counter must never silently disappear. Tiering:
1. Hook present: in-game overlay (works in exclusive FS).
2. Hook blocked: FPS from ETW (`Microsoft-Windows-DXGI` / `DxgKrnl` present events, same method as PresentMon, no injection). Drawn as a topmost click-through layered window (works for windowed, borderless and independent-flip). In the recording, a WGC/Duplication frame gets the overlay composited in software before encoding.
3. True exclusive FS **and** injection blocked: the counter is still computed and shown on a secondary monitor or the tray tooltip, and a one-time notice explains why in-game drawing is unavailable. We will not hide this limitation (see Risks).

## 4. Recording features

- **Video:** NVENC, AMD AMF, Intel QuickSync (via vendor SDKs or Media Foundation). Codecs: H.264, HEVC, AV1 where hardware supports it. Presets: 1080p60, 1440p60, 4K60, native. CBR/CQP/VBR. Constant frame rate via timestamp-driven frame duplication/dropping.
- **Audio:** WASAPI. Per-process loopback (game-only audio), full system loopback, microphone. Separate tracks. All timestamped on QPC for A/V sync.
- **Container:** Fragmented MP4 or MKV so a crash or power loss never corrupts the file.
- **Controls:** global hotkeys (start/stop, instant-replay save, screenshot, overlay toggles). Hotkeys use RegisterHotKey plus raw input so they work in exclusive FS.
- **Instant replay:** rolling N-minute ring buffer of encoded packets in RAM or on disk. Hotkey saves the last N seconds.
- **Screenshots:** PNG/JPG from the same capture path.
- **Auto-detect games:** per-game profiles by exe name. Capture only the game, not the desktop.

## 5. Compatibility plan

| Target | Approach | Phase |
|---|---|---|
| DX11 | Present hook + shared texture | 1 |
| DX12 | Queue + Present hook, fence-synced copy | 1 |
| Exclusive FS (DX11/12) | Same hooks, handle mode-switch ResizeBuffers | 1 |
| Borderless / windowed | Hook or WGC | 1 |
| DX9 / DX10 | Present/EndScene hooks | 3 |
| OpenGL | `wglSwapBuffers` hook, DX interop | 3 |
| Vulkan | Implicit Vulkan layer (no injection needed) | 3 |
| UWP / Store | Package-aware injection or WGC | 3 |
| Anti-cheat titles | Per-game blocklist, auto-fallback to Tier 2/3 and ETW, never inject into protected processes | 2 |

**Injection method:** launcher-assisted (SnapCap starts a process suspended-free via a watcher, loads the DLL early) or `SetWindowsHookEx` for already-running games. Signed binaries to reduce AV and anti-cheat false positives.

## 6. Repository layout

```
snapcap2/
├─ src/
│  ├─ app/        tray, settings UI (ImGui), hotkeys, profiles
│  ├─ hook/       swapchain hooks, DX11/DX12 capture, overlay renderers
│  ├─ encoder/    encode, mux, replay buffer, audio mix
│  ├─ audio/      WASAPI capture, per-process loopback
│  ├─ fallback/   WGC, Desktop Duplication, ETW FPS (PresentMon-style)
│  ├─ ipc/        shared memory, shared textures, fences, command pipe
│  └─ common/     logging, config, timing (QPC), utilities
├─ tests/         unit, integration, test-game harness
├─ tools/         DX11/DX12/exclusive-FS test renderers
├─ third_party/   MinHook, Dear ImGui, vendor encoder headers
└─ CMakeLists.txt, vcpkg.json
```

## 7. Milestones

**M0: Spike (1–2 wks).** Minimal DX11 and DX12 test apps (windowed + exclusive FS). Prove Present hook, shared-texture copy and an FPS number on screen. De-risks everything else.

**M1: MVP (4–6 wks).** DX11/DX12 hook capture incl. exclusive FS, NVENC H.264, game-audio loopback, MP4 output, hotkey start/stop, FPS overlay with both independent toggles, tray app.

**M2: Compatibility and resilience (3–4 wks).** AMF + QuickSync, HEVC/AV1, WGC and Desktop Duplication fallback, ETW FPS fallback, anti-cheat blocklist, 32-bit hook, crash-safe fragmented files, mic track.

**M3: Features (3–4 wks).** Instant replay, screenshots, per-game profiles, overlay customisation, DX9/OpenGL/Vulkan, UWP.

**M4: Polish and release (2–3 wks).** Installer, code signing, auto-update, telemetry-free diagnostics log, docs.

## 8. Testing and verification

- **Test renderers** (`tools/`): DX11, DX12, DX9, GL, Vulkan apps in windowed, borderless and exclusive FS, including mode-switches and ALT-TAB.
- **Matrix:** NVIDIA / AMD / Intel GPUs × Win10 / Win11 × FSO on/off × HDR on/off × multi-monitor.
- **Real-game soak:** a list of ~30 popular titles across engines (Unreal, Unity, Source, id Tech, custom).
- **Performance gates in CI/nightly:** FPS impact, CPU, RAM, frame-pacing stutter (frametime p99) with and without recording. Fail the build on regression.
- **Correctness:** A/V sync drift check over 2 h, file integrity after killing the encoder mid-record, overlay-in-recording on/off pixel diff tests.

## 9. Risks and open questions

| Risk | Impact | Mitigation |
|---|---|---|
| Anti-cheat blocks injection | No Tier 1 capture or in-game overlay | Fallback tiers, ETW FPS, honest per-game notice, per-game allow/blocklist, signing, outreach to vendors |
| "Always display overlay" in exclusive FS with injection blocked | Cannot draw into the game without a hook | Defined guarantee: counter always *computed*; drawing falls back as in section 3. Needs your sign-off on this wording |
| DX12 hook fragility across drivers/engines | Crashes, black frames | Defensive init, per-title disable, large test matrix, isolated encoder |
| HDR / wide gamut | Washed-out captures | Detect format, tonemap or record HDR10 |
| Variable refresh / frame pacing | Judder in recording | Timestamp-based CFR, GPU copy kept off the critical path |
| AV false positives | Install friction | Code signing, minimal API surface, reputation building |

**Open questions for you:**
1. Windows 10 support required, or Windows 11 only? (Per-process audio and WGC are simpler on 11.)
2. Is the hook-based "overlay unavailable in exclusive FS under anti-cheat" limitation acceptable, with fallback on a second display/tray?
3. Distribution: free/open-source, or commercial (affects signing and anti-cheat relationships)?
4. Priority order for post-MVP APIs: Vulkan, OpenGL or DX9 first?
