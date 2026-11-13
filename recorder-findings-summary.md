# Summary of findings (anonymised)

**What the project is:** a lightweight Windows gameplay recorder with an FPS overlay, aimed at QA use. The requirements settled on: very simple, minimal impact on the game, and as compatible as possible.

## Recording cost

- The original recorder was CPU-heavy: it read each frame back from the GPU and converted it on the CPU. That used about **130% of one core** at 1080p60.
- Converting on the GPU and handing frames straight to the hardware encoder used about **23%** of a core, with no frames over budget and colours matching the old path to within 1-2 levels. The tester called it "vastly better".
- A hook that copied the game's back buffer on every present stalled the game's GPU pipeline. Copying only when the recorder asks fixed that.

## Overlay cost (game GPU-bound, fullscreen, warmed up and run in reversed orders)

| Overlay | Game fps |
| --- | --- |
| Hidden | about 39.6 |
| Drawn inside the game by a hook | about 39.7 (no measurable cost) |
| A window over the game | about 36.2 (about -9%) |

In a plain windowed game, the window cost only about 4%. A DirectComposition window was not shown to be better, and the first comparison of it was confounded.

## Compatibility

- Unreal-style games often default to Direct3D 12, which a Direct3D 11 hook can't use.
- Some Direct3D 11 games also load `d3d12.dll` (through DLSS support), which fooled the detection. That was fixed.
- Such games often use 10-bit back buffers. Support was added.
- A `-dx11` launch switch works for some builds, and a restart-in-DX11 offer was built.
- True exclusive fullscreen allows no window overlay on the same screen. Only a hook (or a driver-level overlay) works there, and many "exclusive" games are really flip-model borderless.
- Windows' own event tracing gives an **exact FPS for any graphics API with no injection**. In testing on DX11, its count matched the hook's exact count within a few percent. It needs administrator rights or membership of a specific group. Support for Vulkan and OpenGL was never verified.

## Method lessons

- Start-menu scenes are capped, noisy and affected by shader compilation, so they are weak benchmarks.
- Good practice: a discarded warm-up launch, a settling period, many samples, and reversed run orders. The first comparison, run in a single order, produced a misleading result.
- Independent checks help: counting unique frames in a recording, and comparing against the hook's exact count.
- One game's frame rate collapsed in a heavy scene. It showed up in two independent measures, so it was the game's own lag and not the recorder.

## Safety and process lessons

- An installer step that changed the user's account group, followed by asking them to sign out, was too risky. The tester then hit a temporary-profile sign-in problem. The evidence suggests Windows hive locks held by OEM and system services, and that the group change never applied, but it damaged trust either way. **Rule: never change account or system state, and keep elevation opt-in per launch.**
- An automated screen-capture test recorded a private window the tester had open. Automated tests should never capture whichever window happens to be focused.

## State of the code

- **Published (v0.2.0):** GPU recording, 10-bit support, the Direct3D 12 misdetection fix, the DX11 restart offer, lighter overlay and UI work, and a GPU/CPU setting.
- **Dropped:** event-tracing FPS, in-game overlay for every DX11 game, the DirectComposition overlay, and the installer group step.