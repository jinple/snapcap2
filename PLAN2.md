# SnapCap: Plan 2 (revised against recorder findings)

Supersedes [PLAN.md](PLAN.md). Informed by [recorder-findings-summary.md](recorder-findings-summary.md), which came from a real prototype (a QA-oriented gameplay recorder).

## 1. What changed and why

| PLAN.md assumption | Finding | Change |
|---|---|---|
| Hook copies the backbuffer on **every Present** | Per-present copy stalled the game's GPU pipeline | **Copy on demand only**: the recorder requests a frame at the target cadence; the hook copies only then |
| Overlay fallback = topmost click-through window | Window overlay cost about 9% fps in fullscreen GPU-bound (about 4% windowed); DirectComposition not shown better; impossible in true exclusive FS | In-game hook overlay is the only "free" overlay. Window overlay is a **clearly labelled, costed fallback**, off by default in fullscreen |
| ETW FPS is a no-injection fallback | Accurate (within a few percent of hook count) but needs admin or a specific group. Vulkan/OpenGL unverified. | ETW is **opt-in per launch** and optional. Never alter groups. Not a guarantee |
| Hook DX12 detected by loaded modules | DX11 games load `d3d12.dll` (DLSS); detection was fooled | Detect API by **actual device / swapchain creation**, never by module presence |
| 8/10-bit RGBA backbuffers assumed | Unreal-style games use 10-bit back buffers | 10-bit (R10G10B10A2) is MVP; FP16/HDR later |
| CPU target < 2% | Measured: CPU readback 130% of a core; GPU convert + HW encode **about 23% of a core** at 1080p60 | Realistic budget: **under 25% of one core at 1080p60**, no frames over budget |
| Broad feature scope | Requirements settled on: very simple, minimal game impact, max compatibility, QA use | **Cut scope hard** (section 5) |
| Installer, elevation, test automation not considered | Group change plus sign-out broke a user's profile; automated screen capture recorded a private window | New **safety rules** (section 4) |

## 2. Verdict on the original plan

**Holds:**
- Hook-based capture is required for exclusive fullscreen (confirmed: only a hook or driver overlay works there).
- Zero-copy GPU pipeline: GPU colour conversion into hardware encoder, colours within 1-2 levels of the old path.
- In-game overlay is effectively free (39.6 hidden vs 39.7 drawn fps).
- Out-of-process encoder, FPS from frame timing at Present.

**Does not hold (fixed above):** per-present copy, window-overlay fallback as a safe default, ETW as a dependable fallback, module-based API detection, over-broad scope, aggressive CPU number.

## 3. Revised architecture

```
snapcap.exe        tray + minimal settings, hotkeys, launches game or attaches
snapcap-encoder    out-of-process: GPU convert -> HW encode -> MP4 (fragmented)
snapcap-hook64.dll in-game: Present hook, on-demand copy, overlay draw
```

**Capture (hook, DX11 and DX12):**
1. Hook `Present` / `Present1` / `ResizeBuffers`. DX12 adds the command-queue hook.
2. Hook only measures frame time each Present (cheap, drives FPS overlay).
3. Encoder signals "need frame" at the target fps (shared event). On the next Present the hook copies the backbuffer into a shared texture (fence-synced), then returns. No signal, no copy.
4. Encoder converts RGB(10-bit or 8-bit) to NV12/P010 **on the GPU** and feeds the hardware encoder. CPU readback exists only as an explicit GPU/CPU setting for odd hardware.

**Overlay:**
- Drawn by the hook into the backbuffer pre-Present (DX11 and DX12 renderers), save/restore all state.
- On-screen vs in-recording independence: capture copy taken before the overlay draw (clean) or after (burned in).
- Overlay coverage is the honest weak spot: the prototype dropped "in-game overlay for every DX11 game". Plan for **per-title hook failures** with a visible state (overlay status shown in the tray: *in-game / window fallback / unavailable*).

**FPS availability tiers (revised guarantee):**
1. Hook active: exact FPS drawn in-game, any display mode incl. exclusive FS.
2. Hook not possible, game windowed/borderless: FPS from ETW if the user launched elevated (opt-in), drawn in a window overlay (costed, about 4-9%).
3. Exclusive FS + no hook: FPS shown in the tray / second display only. The tray status says why.

**DX12 / mixed titles:** many Unreal-style games default to DX12. Support the DX12 hook; also offer **restart in DX11** (`-dx11`) where the engine supports it, as the prototype did, when the DX12 path fails.

## 4. Safety and process rules (new, non-negotiable)

1. **Never change account or system state.** No group membership edits, no registry hacks, no sign-out requests. Ship as a portable zip first; installer is optional and does nothing privileged.
2. **Elevation is opt-in per launch**, off by default, explained in the UI.
3. **Automated tests never capture "the focused window".** They capture only a window or process the test itself created (our own test renderer). No desktop or foreground-window capture in tests.
4. Hook injects only into the process the user selected. Protected / anti-cheat processes are blocklisted, never injected.
5. Everything a user could find surprising (overlay fallback mode, elevation, restart offer) is shown plainly before it happens.

## 5. Scope (cut for "very simple")

**MVP (in):** DX11 + DX12 hook capture including exclusive FS and 10-bit; on-demand copy; GPU convert + one hardware encoder family at a time (NVENC first, then AMF/QuickSync); H.264 (+HEVC later) to fragmented MP4; WASAPI per-process (game) audio; hotkey start/stop; FPS overlay with independent on-screen/in-recording toggles; tray status; portable zip.

**Deferred:** instant-replay buffer, per-game profiles, mic track, AV1, FP16/HDR, DX9/GL/Vulkan, UWP, 32-bit, auto-update, ImGui polish (use a minimal Win32 settings dialog).

**Dropped (as the prototype did):** DirectComposition overlay, installer group step, "overlay in every DX11 game" as a promise.

## 6. Milestones

- **M0 Spike (1-2 wks):** DX11 + DX12 test renderers (windowed, borderless, exclusive FS). Prove on-demand copy has no measurable stall and the overlay renders. Prove DX12 detection by device creation. Exit criterion: fps delta within noise.
- **M1 MVP (4-5 wks):** scope above, NVENC, safety rules enforced.
- **M2 Compatibility (3-4 wks):** AMF + QuickSync, DX11 restart offer, blocklist, ETW opt-in, per-title failure reporting, 10-bit/format coverage across real Unreal/Unity titles.
- **M3 Extras:** pulled from the Deferred list by actual QA need.

## 7. Measurement method (from the findings)

- Use a **GPU-bound, uncapped, fullscreen test scene** we control. Avoid start-menu scenes (capped, noisy, shader compile).
- Protocol: discarded warm-up launch, settling period, many samples, **reversed run orders** (A/B/B/A). Single-order comparisons are not accepted.
- Cross-checks: count **unique frames in the recorded file** vs target; compare overlay fps against the hook's exact count and against ETW where available.
- When a real game dips, confirm with two independent measures before blaming the recorder.
- **Budgets (gates):** overlay cost not measurable vs hidden; recording under 25% of one core at 1080p60; zero frames over budget; colour within 2 levels of reference.

## 8. Risks

| Risk | Mitigation |
|---|---|
| Overlay not drawable in some titles (no hook, exclusive FS) | Tray status + tiered fallback; do not promise "always on-screen". |
| DX12 hook instability | M0 spike first; per-title disable; isolated encoder |
| ETW needs elevation; Vulkan/GL unverified | Opt-in only; verify before relying on it |
| Anti-cheat blocks injection | Blocklist, WGC fallback for windowed/borderless |
| Format surprises (10-bit, HDR) | Detect swapchain format; convert on GPU; test across engines |

## 9. Open questions

1. R4 says the FPS overlay must *always* be available. The findings show that is only truly achievable with a working hook. Is "always, with a visible status when degraded" acceptable?
2. Do we need Windows 10, or Windows 11 only?
3. Is "portable zip, no installer" acceptable for the QA audience?
