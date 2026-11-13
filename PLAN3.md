# SnapCap: Plan 3

Keeps the **requirements of [PLAN.md](PLAN.md) section 1 (R1 to R5) unchanged**. Keeps the measured corrections from [PLAN2.md](PLAN2.md) and [recorder-findings-summary.md](recorder-findings-summary.md). Replaces both plans as the implementation plan.

## 1. Why a third plan

**PLAN.md** has the right requirements and the right core architecture (in-process hook, zero-copy GPU path, out-of-process encoder). Four of its mechanisms are contradicted by the prototype: per-Present copy, the window overlay as a free fallback, ETW as a no-strings fallback, and API detection by loaded module. It also has gaps that would show up as bugs (section 3).

**PLAN2.md** fixes those mechanisms correctly, but it does so partly by giving up requirements:

| Requirement | What PLAN2 did | Status under PLAN2 |
|---|---|---|
| R1 "< 2% CPU" | Replaced with "under 25% of one core" | Rewritten, but see 2.1: the two numbers are mostly the same thing in different units |
| R1 "Installer < 10 MB" | Portable zip only | Dropped |
| R2 DX9/DX10/OpenGL/Vulkan, 32-bit, UWP | "Deferred, pulled in by QA need" | No longer scheduled |
| R4 "always available" | "Do not promise always on-screen"; window fallback off by default in fullscreen | Weakened |
| R5 optional mic | Deferred | No longer scheduled |

So neither plan delivers R1 to R5 as written. Plan 3 does, with one residual case in R4 that no user-mode software can close (section 4.3), stated precisely instead of softened globally.

## 2. Three findings that need re-reading

### 2.1 The CPU budget was a units mismatch, not a miss

R1 says "< 2% CPU" (whole machine, as Task Manager shows it). The prototype measured "about 23% of **one core**". Converting:

| Logical processors | 23% of one core as total CPU | 2% total expressed per core |
|---|---|---|
| 8 | 2.9% | 16% of a core |
| 12 | 1.9% | 24% of a core |
| 16 | 1.4% | 32% of a core |

The prototype already meets R1 on a 12-thread machine and is about 1.4x over on an 8-thread one. R1 stays. **Gate: recording at 1080p60 uses at most 2% total CPU on an 8-logical-processor reference machine, i.e. at most 16% of one core**, summed across the encoder process and any helper. Section 5.3 lists where the remaining third is expected to come from. The findings do not say what CPU the 23% was measured on or how it breaks down, so M0 profiles it before any code is designed around it.

### 2.2 "Exclusive fullscreen" is a much smaller set than both plans assume

The findings note that many "exclusive" games are really flip-model borderless. Taking that further:

- **DX12** swap chains are always flip-model. "Exclusive" there is emulated, and the compositor can still draw over it.
- **DX9/10/11** fullscreen gets Fullscreen Optimizations by default on Windows 10/11, which also turns it into a flip-model borderless presentation.
- **True exclusive** remains only where the user or a compatibility setting disabled Fullscreen Optimizations, plus some OpenGL and Vulkan (`VK_EXT_full_screen_exclusive`) titles.

This matters for R4: the case where nothing can draw on screen is "true exclusive **and** hook refused", which is far narrower than "fullscreen and hook refused".

### 2.3 Screen APIs are not all blind to exclusive fullscreen

PLAN.md states screen-based APIs cannot capture exclusive fullscreen. That is true for WGC window capture and GDI. DXGI Desktop Duplication is expected to deliver frames from a true exclusive fullscreen output (it reports `ACCESS_LOST` on the mode switch and must be re-created). I have not verified this on the target matrix, and hybrid-GPU laptops are a known weak spot, so it is an M0 gate (G6), not an assumption. If it holds, recording works in every display mode even with no hook.

## 3. Gaps in both earlier plans

1. **Mouse cursor.** A backbuffer copy never contains the hardware cursor. Menus, strategy games and desktop apps would record with no pointer. Needs compositing in the encoder.
2. **Frame pacing beat.** PLAN2's "encoder signals, hook copies on the next Present" has the recorder clock and the game clock out of phase. When game fps is near target fps this gives periodic duplicate plus dropped frames (visible judder). Section 5.1 fixes it.
3. **Late attach.** "Detect the API by device creation" only works if the hook is loaded before the game creates its device. Most real sessions attach to a running game.
4. **The hook must never wait.** A keyed-mutex acquire with a timeout inside Present is the likely cause of the per-Present stall. On-demand copy reduces how often it happens but not whether it can.
5. **In-recording overlay on fallback tiers.** Capture-point ordering only exists inside the hook. With WGC or Desktop Duplication there is no "before/after overlay" point, and a monitor capture would pick up the fallback overlay window whether wanted or not.
6. **Multi-adapter machines.** The shared texture lives on the adapter the game renders on. The encoder must open it on that adapter and pick that adapter's hardware encoder.
7. **Player compatibility of fragmented MP4.** Crash-safe, but some editors and players seek poorly in it.
8. **ETW provider choice.** `Microsoft-Windows-DXGI` only sees DirectX. The kernel `DxgKrnl` present events cover every API, which is what PresentMon relies on for OpenGL and Vulkan.

## 4. Design

### 4.1 Processes

```
snapcap.exe           tray, native Win32 settings dialog, hotkeys, game list.   Always running, unelevated.
snapcap-encoder.exe   compositor + hardware encode + audio + mux.                Only while recording or replay is armed.
snapcap-hook64.dll    in-game: frame timing, overlay draw, paced copy.
snapcap-hook32.dll    same for 32-bit games, loaded by snapcap-inject32.exe.
snapcap-vklayer       Vulkan implicit layer (64 and 32), dormant unless snapcap.exe is running.
snapcap-etw.exe       optional elevated helper: present events to a named pipe.  Only when the user approves a UAC prompt.
```

Changes from PLAN.md: native Win32 settings instead of Dear ImGui (no D3D device or render loop in the idle process, which is what makes < 40 MB and < 0.5% achievable); a separate, minimal elevated helper so the main app never runs elevated; the Vulkan layer as its own artefact.

### 4.2 One rule for the two overlay toggles

> **Captured frames are always clean. The on-screen overlay is drawn by whichever on-screen tier is active. The in-recording overlay is always composited by the encoder.**

The hook copies the backbuffer **before** it draws the overlay, every time. Frame statistics (fps, frametime, lows) travel with each frame in shared memory. The encoder's GPU pass, which already exists for scaling and colour conversion, blends the FPS text and the cursor when their toggles are on.

This replaces capture-point ordering. It costs one small textured quad in a pass that runs anyway, and it buys:

- R4 independence that works identically on every capture tier, including the no-hook ones.
- The in-recording toggle works **in every case**, including the residual case in 4.3.
- Recorded overlay text is rendered at output resolution, not scaled with the game image.
- The fallback overlay window is marked `WDA_EXCLUDEFROMCAPTURE`, so monitor capture stays clean too.
- Cursor support with its own toggle, for free.

### 4.3 R4: where the FPS overlay comes from

| Situation | FPS source | On screen | In recording |
|---|---|---|---|
| A. Hook active (any display mode, incl. true exclusive) | Hook, exact | Drawn in-game, no measurable cost | Encoder composite |
| B. No hook, presentation is windowed / borderless / flip-model fullscreen | ETW helper, exact, if approved. Otherwise capture-arrival count, labelled approximate and capped at refresh | Click-through topmost window. Costed: about 4% windowed, about 9% fullscreen GPU-bound in the prototype | Encoder composite |
| C. No hook **and** true exclusive fullscreen | Same as B | **Not possible on that screen.** Shown on a second monitor if present and in the tray. One-time notice explains why and that switching the game to borderless restores it | Encoder composite |

Position on "always available": the counter is always computed, always available in the recording, and always on screen except in case C. Case C is a property of Windows, not of this design; only a hook or a display driver can draw there. Section 2.2 is why C is small, and the route to shrinking it further is getting the hook accepted (code signing plus anti-cheat vendor allow-listing, started in M1 because the lead time is long).

In case B the window overlay is **on by default** because R4 is a requirement and the cost only applies to games that refused the hook. The tray shows the active tier (*in-game / window / second screen*) and the measured cost, and the user can switch it off. M0 gate G4 checks with PresentMon whether the game keeps independent flip under the window on MPO-capable hardware; if it does, the cost on those machines is near zero and the default is uncontroversial.

### 4.4 Capture tiers

| Tier | Backend | Covers | Used when |
|---|---|---|---|
| 1 | Graphics hook / Vulkan layer | Every display mode | Default |
| 2 | Windows Graphics Capture, window mode | Windowed, borderless, flip-model fullscreen, UWP | Hook refused or failed |
| 3 | Desktop Duplication, cropped to the game's monitor | True exclusive (pending G6), anything else | Tier 2 delivers no frames |

All three feed the same encoder input: a GPU texture plus a QPC timestamp plus frame statistics.

## 5. Mechanisms

### 5.1 Hook: paced, non-blocking copy

- **Pacing is done in the hook, by timestamp.** The encoder publishes "recording on, interval T" in shared memory. At each Present the hook compares QPC time against the next due time and copies only the Present nearest to it (copy if `now >= due - T/2`, then `due += T`, with a clamp after stalls). This is one integer compare per Present, no cross-process wait, and it keeps the findings' on-demand behaviour. At game fps equal to target fps every frame is taken with no beat; above it, evenly spaced frames; below it, the encoder duplicates.
- **Never wait.** A ring of three shared textures. The hook try-acquires the next one with a zero timeout; if the encoder still holds it, the frame is skipped and counted. DX12 uses a shared fence signalled on the game's queue; nothing in Present blocks on it.
- **No explicit Flush.** The copy is queued immediately before the real Present, which submits it.
- **MSAA backbuffers** are resolved, not copied. **8-bit and 10-bit** formats in the first milestone, as in PLAN2.
- **Idle cost.** Not recording: QPC read, ring-buffer write for frame time, overlay draw if shown. Nothing else.

### 5.2 Hook: attach and API detection

- **Find Present without touching the game.** `snapcap.exe` (and the 32-bit injector) create a throwaway device and swap chain in their own process, read the offsets of `Present`, `Present1` and `ResizeBuffers` inside `dxgi.dll`, and pass them to the hook. The hook patches those functions. Works for early and late attach.
- **Detect the API at the first hooked Present** by asking the swap chain for its device: `ID3D12Device`, else `ID3D11Device`, else `ID3D10Device`. Loaded modules are never consulted. This is immune to the DLSS `d3d12.dll` case and to late attach.
- **DX12 queue.** Launched by SnapCap: taken from the `CreateSwapChain*` call, where the queue is an argument. Attached late: learned from `ExecuteCommandLists`, accepted only if it is a direct queue on the swap chain's device.
- **Injection.** Into a running game: remote `LoadLibrary`, with `SetWindowsHookEx` as the alternative method. Launch-through-SnapCap for early attach. The restart-in-DX11 offer from the prototype stays as a per-title escape hatch.
- **Which processes.** A game list that grows the first time the user presses a SnapCap hotkey over a foreground app, plus optional auto-detect (foreground window covering a monitor, graphics API loaded, not on the deny list). Anti-cheat-protected titles are detected and never injected; they go straight to tier 2.

### 5.3 Encoder: getting from 23% to 16% of a core

- Vendor SDKs called directly (NVENC, AMF, Intel VPL), loaded from the driver. No Media Foundation pipeline in the video path.
- When output size equals game size and no overlay or cursor is being composited, hand the RGB texture straight to the encoder (NVENC and AMF convert internally). The compositor pass runs only when it has something to do.
- No pixel data ever touches the CPU. No per-frame allocation. All waits are event-driven.
- Audio captured at the device mix format, encoded with the Windows AAC encoder, no resampling stage unless required.
- If M0 profiling shows the prototype's 23% was dominated by something else, this list is revised then.

### 5.4 Audio and container

- Game audio: WASAPI process loopback (target process tree). Works on Windows 10 2004 and later in practice. Mic: separate track by default, optional mix into track 1 because many players only play the first track. System loopback as the fallback.
- **Hybrid MP4:** written fragmented while recording (crash-safe), then finalised with a full index on a clean stop so every player and editor seeks normally. An interrupted file is still playable.
- Constant frame rate from frame timestamps. All clocks are QPC.

### 5.5 FPS without the hook

- `snapcap-etw.exe` is started only when a no-hook game is detected and the user accepts a UAC prompt, once per session. It reads kernel present events using the PresentMon analysis library (MIT) and publishes numbers over a pipe. The main app stays unelevated.
- Declined or unavailable: capture-arrival count, shown with an "approx." marker because it cannot exceed the refresh rate.
- No group membership changes, ever. A permanent service is not planned.

### 5.6 API breadth (R2)

| Target | Mechanism | Milestone |
|---|---|---|
| DX11, DX12, 64-bit | DXGI Present hook | M1 |
| Any API, windowed / borderless / UWP | Tier 2 + encoder composite + window overlay | M2 |
| 32-bit games | `hook32` + `inject32`, same source | M2 |
| Vulkan | Implicit layer on `vkQueuePresentKHR`. The Khronos-sanctioned route; no injection | M3 |
| OpenGL | `wglSwapBuffers` hook, GL/DX interop share, PBO readback fallback | M3 |
| DX9 | `Present` / `PresentEx` / `Reset` hooks | M3 |
| DX10 | Same DXGI hook, small D3D10 copy and overlay path | M3 |
| UWP in-game overlay | Hook where the package permits loading it, else tier 2 | M3 |
| HDR (FP16 / HDR10) | Tonemap to SDR in the compositor; HDR10 recording later | M3 |

Tier 2 landing in M2 means every API is recordable with an overlay in non-exclusive modes from M2 on. M3 then upgrades each API to tier 1 (exclusive fullscreen, free overlay, lowest cost).

## 6. Safety rules

Carried over from PLAN2 section 4, with one clarification.

1. No account or machine-wide state changes: no group edits, no services, no drivers, no sign-out requests, no compatibility-flag edits.
2. Elevation is per launch, opt-in, and confined to `snapcap-etw.exe`.
3. Automated tests capture only windows and processes the test created. Never the foreground window, never the desktop.
4. Never inject into protected or anti-cheat processes.
5. Anything surprising is shown before it happens.
6. **Clarification:** the installer is per-user (no admin, installs under the user profile) and a portable zip ships alongside it. The only registration it makes is the per-user Vulkan implicit-layer entry, which is the documented Khronos mechanism, is listed on the install screen with a checkbox, and is removed on uninstall. The zip makes no registrations and offers the Vulkan entry from settings.

## 7. Milestones

**M0: Feasibility gates (2 wks).** Test renderers we control (DX11, DX12; windowed, borderless, FSO fullscreen, true exclusive). Each gate is a measurement with the PLAN2 section 7 protocol (warm-up, settle, many samples, reversed orders).

| Gate | Question | Pass | If it fails |
|---|---|---|---|
| G1 | Paced non-blocking copy cost, GPU-bound, 1080p and 4K | fps delta within noise | Investigate the stall before anything else; R1 depends on it |
| G2 | In-game overlay cost, DX11 and DX12 | Within noise | Simplify the renderer |
| G3 | Recording CPU at 1080p60 on the 8-thread reference | At most 16% of a core; profile of the prototype's 23% | Revise 5.3; report the achievable number |
| G4 | Window overlay: does the game keep independent flip, and what does it cost | Measured per presentation mode | Cost goes in the tray text as measured |
| G5 | ETW helper fps vs hook fps on DX11, DX12, Vulkan, OpenGL | Within 2% | Per-API "approx." label |
| G6 | Desktop Duplication in true exclusive fullscreen, incl. a hybrid-GPU laptop | Frames delivered after mode switch | Case C has no recording either; say so in the notice |
| G7 | Late attach and API detection on a DX11 game that loads `d3d12.dll` | Correct API, no crash | Fix before M1 |

**M1: Core (5 to 6 wks).** `hook64` for DX11 and DX12 with late attach, exclusive fullscreen and mode switches, 8/10-bit. Encoder with NVENC H.264, compositor (scale, FPS overlay, cursor), game audio, hybrid MP4. In-game overlay with both toggles. Hotkeys (`RegisterHotKey`, plus a key check inside the hook for games that suppress hotkeys). Tray, settings, game list. Start code signing and anti-cheat vendor contact.

**M2: Universal fallback and vendors (4 to 5 wks).** WGC and Desktop Duplication tiers, ETW helper, window overlay with tier status, anti-cheat detection, AMF and Intel, HEVC and AV1, mic track, 32-bit hook, multi-adapter handling.

**M3: API breadth (4 wks).** Vulkan layer, OpenGL, DX9, DX10, UWP, HDR tonemap.

**M4: Features (3 wks).** Instant replay, screenshots, per-game profiles, overlay customisation (position, scale, colour, 1% / 0.1% lows).

**M5: Release (2 to 3 wks).** Per-user installer under 10 MB, portable zip, signing, update check, diagnostics log, docs.

Total about 20 to 23 weeks. This is longer than PLAN.md's 13 to 19 because M0 is a real measurement phase and tier 2 is built as a first-class path. R1 to R5 are all met at the end of M3; M1 already meets R1, R3, R4 and R5 (minus mic) for DX11/DX12.

## 8. Verification

PLAN.md section 8 and PLAN2 section 7 both apply. Additions:

- **R1 gates, run nightly on the reference machine:** installer size; idle CPU and private bytes of `snapcap.exe` over 10 minutes; fps delta and frametime p99 with recording on vs off; total CPU while recording.
- **R4 matrix test:** for each capture tier, all four toggle combinations, checked by pixel diff of the screen region and of the recorded file.
- **Pacing test:** renderer draws a frame counter; recorded file is checked for duplicates and gaps at game fps of 0.5x, 1x, 1.5x, 2x and 2.5x the target.
- **Never-wait test:** suspend the encoder mid-recording; game frametime p99 must not move.
- **Coexistence:** Steam overlay, Discord overlay, RTSS, ReShade, DLSS/FSR frame generation (which proxy or wrap the swap chain and present from other threads).

## 9. Risks

| Risk | Mitigation |
|---|---|
| The prototype "dropped in-game overlay for every DX11 game"; the summary does not say why | Find out from the prototype's history before M1. If it was state corruption, the DX11 renderer saves and restores the full pipeline state and is tested against deferred-context engines |
| G3 fails on 8 threads | R1 number is reported honestly per core count; direct-to-encoder path and compositor skipping are the main levers |
| Anti-cheat refuses the hook | Tier 2, case B overlay, vendor allow-listing. Case C stated plainly |
| Frame generation and third-party overlays hooking the same functions | Coexistence tests; chain to whatever was there; per-title disable |
| Hybrid-GPU laptops | Adapter LUID carried with every frame; encoder chosen per adapter; cross-adapter copy only as a last resort |
| Windows N editions lack the AAC encoder | Detect at start, fall back to Opus or PCM audio, tell the user |
| AV heuristics on remote `LoadLibrary` | Signing, `SetWindowsHookEx` alternative, launch-through-SnapCap |

## 10. Decisions taken, and three that are yours

Taken, to stop them blocking: Windows 10 22H2 and Windows 11 both supported. Post-MVP API order is Vulkan, OpenGL, DX9, DX10. Native Win32 UI.

Yours:

1. **R4 wording.** "Always computed, always in the recording, always on screen except true exclusive fullscreen with the hook refused." If that exception is not acceptable, the only remaining lever is anti-cheat allow-listing, which is a business effort and not an engineering one.
2. **Window-overlay default.** On by default when the hook is refused (plan as written, honours R4, costs a measured few percent in those games), or off by default (PLAN2's choice, honours "minimal impact").
3. **Safety rule 6.** Is a per-user installer with one opt-in, per-user Vulkan layer registration inside the "never change system state" rule as you mean it?
