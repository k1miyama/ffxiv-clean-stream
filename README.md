# FFXIV Clean Stream

FFXIV Clean Stream creates a separate game preview for Discord. Your normal FFXIV window keeps
showing the MMOMinion GUI, while the preview keeps FFXIV and its native HUD but leaves the
MMOMinion overlay out.

The app does not stream or record anything by itself. Discord captures the preview window named
**FFXIV Clean Stream**.

> Share **FFXIV Clean Stream** in Discord. Do not share the FFXIV window or your entire screen,
> because those still contain the overlay.

## Requirements

- 64-bit Windows 10 or newer.
- FFXIV running with the DirectX 11 client (`ffxiv_dx11.exe`).
- MMOMinion running with its GUI visible before capture begins.
- `FfxivCleanStream.exe` and `FfxivCleanStreamHook64.dll` kept together in the same folder.
- FFXIV Clean Stream running at the same permission level as FFXIV. If FFXIV runs as
  administrator, run FFXIV Clean Stream as administrator too.

Only one copy of FFXIV Clean Stream can run at a time.

## Quick start

1. Extract the complete release into one folder.
2. Start FFXIV in DirectX 11 mode.
3. Start MMOMinion and wait until its GUI is visible in the game.
4. Run `FfxivCleanStream.exe` at the same permission level as FFXIV.
5. Leave **1280 × 720 (recommended)** and **30 FPS (recommended)** selected, then click
   **Start / Resume**.
6. Wait for the status to say **Ready** and for the separate preview window to appear.
7. In Discord, share the application window named **FFXIV Clean Stream**. Do not select FFXIV or
   the controls window named **FFXIV Clean Stream — Standalone Controls**.

If the app reports an error, click **Copy error** and paste the complete message into the
[GitHub bug-report form](https://github.com/k1miyama/ffxiv-clean-stream/issues/new/choose). The
button stays disabled when the current status is not an error.

Keep the preview window open while it is being shared. The MMOMinion GUI should remain visible in
your normal game window but absent from the Discord preview.

When you finish streaming, click **End stream** or close the preview with its **X** button. To stream
again, choose the resolution and frame rate you want and click **Start / Resume**. Discord may keep
the old, closed preview selected, so choose the newly created **FFXIV Clean Stream** window before
going live again.

## Controls, resolution, and frame rate

- **Start / Resume** starts capture, resumes after a pause, or creates a new preview after an end. It
  applies the currently selected resolution and frame rate.
- **Pause capture** stops new GPU frame copies. Click **Start / Resume** to continue.
- **End stream** stops capture, releases the controller's preview resources, and closes the
  shareable preview window. Closing the preview with its **X** button does the same thing.
- **Copy error** copies the complete current error message to the Windows clipboard. It becomes
  available only while an error is displayed and changes to **Copied!** on success.
- Closing the controls window exits the controller. The small capture helper stays loaded until
  FFXIV exits, even when capture is paused or the stream is ended.

The resolution selector supports exactly two 16:9 preview-window sizes:

| Preview size | When to use it |
| --- | --- |
| **1280 × 720 (recommended)** | Default. A smaller Discord window-capture source and lighter Windows composition. |
| **1920 × 1080** | A larger Discord window-capture source with higher composition cost. |

The 720p setting changes the preview window and the source dimensions Discord sees when selecting
that window. Discord still controls the encoded stream resolution and may rescale it according to
your Discord quality settings. This selector does not shrink the native GPU capture ring inside
FFXIV; clean frames are still copied at the game's render resolution. Lower the frame rate as well
when the game-side copy cost is the limiting factor.

| Setting | When to use it |
| --- | --- |
| **30 FPS (recommended)** | Best starting point for normal streaming. |
| **60 FPS** | Smoother preview with higher GPU and memory-bandwidth use. |
| **15 FPS (lightest)** | Use when the game is already GPU-bound or the dropped-frame count rises quickly. |

While capture is ready, the controls show the number of clean frames, frames dropped because the
preview was busy, and the worst measured CPU submission time. An occasional busy-frame drop is
normal: the app drops work instead of making the game wait.

## Troubleshooting

| Message or problem | What to do |
| --- | --- |
| Discord still shows the overlay | Confirm Discord is sharing **FFXIV Clean Stream**, not FFXIV or the entire screen. |
| `FFXIV (DirectX 11) was not found` | Start the DirectX 11 game client, start MMOMinion, wait for its GUI, then click **Start / Resume**. |
| `Windows denied access to FFXIV` | Run FFXIV Clean Stream at the same permission level as FFXIV, usually as administrator. |
| The helper DLL is missing or could not load | Keep the EXE and DLL together. Check whether security software quarantined `FfxivCleanStreamHook64.dll`. |
| MMOMinion changed its graphics hook | Fully exit and restart FFXIV. Wait for the MMOMinion GUI to appear, then run FFXIV Clean Stream and start capture again. |
| The overlay is present from the first preview frame | Restart FFXIV and repeat the startup order exactly. If it persists, that MMOMinion build may draw too early for its pixels to be separated. |
| The preview is black, frozen, or reports a GPU error | Click **Copy error**, then click **End stream**, select 1280 × 720 and 30 FPS, and click **Start / Resume**. If it still fails, run `GpuShareSelfTest.exe` and include the copied message in a bug report. |
| Discord stays black after restarting the stream | Stop sharing the old preview in Discord and select the newly created **FFXIV Clean Stream** window. |
| Game performance drops | Click **End stream**, select 1280 × 720 and 15 FPS, then click **Start / Resume**. The 720p choice lightens the output window; 15 FPS also reduces game-side copy frequency. Disable other programs that inject into or capture FFXIV. |
| FFXIV Clean Stream says it is already running | Find and use the existing controls window, or close it before starting another copy. |
| Discord has no FFXIV audio | The preview supplies video only. Select the appropriate audio source in Discord or use your normal audio-routing setup. |

## How it keeps the game responsive

Frames stay on the GPU from capture to preview. The helper uses a three-frame sharing ring at
FFXIV's native render resolution, does not read frames back through the CPU, and never waits for
Discord or the preview to catch up. If a shared frame is still busy, that frame is skipped instead
of stalling FFXIV. The selected 720p or 1080p size applies to the separate preview window; Discord
may rescale that source for the encoded stream.

Several GPU-sharing methods are tried automatically for compatibility. No setting or manual choice
is required.

## Safety and limitations

- The app loads a native capture helper into FFXIV. Security software may warn about or block this
  behavior, and any injected graphics helper carries some crash risk.
- Overlay removal depends on render order. It works when MMOMinion draws through the compatible
  presentation path after capture attaches. An overlay already baked into the game frame cannot be
  removed generically.
- The preview carries video only; it does not forward FFXIV audio.
- Pausing, ending the stream, or closing the controller stops capture but does not remotely unload
  the helper. Fully exiting FFXIV unloads it.
- The project contains no stealth or anti-cheat bypass behavior.

## Optional GPU self-test

`GpuShareSelfTest.exe` checks the GPU-sharing methods without opening or modifying FFXIV,
MMOMinion, or Discord. A successful result verifies the frame transport on the current GPU and
driver; the real overlay order is tested only when running with FFXIV.

## Project layout

- `common/` contains the shared IPC protocol and pixel-format rules.
- `host/` contains the controller entry point, Win32 UI, FFXIV discovery, IPC session, and preview
  renderer. GPU resource setup and per-frame presentation are separate modules.
- `hook/` contains the DLL entry point, process-lifetime state, capture control, GPU transport,
  resource negotiation and lifetime, frame capture, swap-chain hooks, and worker lifecycle.
- `tests/` contains the GPU self-test and hidden synthetic end-to-end fixtures. Shared fixture code
  is divided into process, IPC, and GPU support modules.
- `.github/ISSUE_TEMPLATE/` contains guided forms for bug reports and feature requests.
- First-party implementations use `.cpp`; first-party headers use `.h` or `.hpp`.
- `scripts/source_manifest.ps1` assigns every production and test `.cpp` file to a build target. The
  build fails if a source is missing, unclassified, or uses an unsupported implementation suffix.

## Building from source

The release build uses Zig 0.16.0 and does not require a Visual Studio project. Put Zig at
`.tools\zig-0.16.0\zig.exe`, add `zig` to `PATH`, or pass its full path to any build or packaging script with
`-Zig`.

The checked-in clangd fallback uses the portable `.tools\zig-0.16.0` layout for Windows headers.
When Zig is installed elsewhere, use a compilation database generated for that toolchain or update
the Zig system-include paths in `compile_flags.txt`; this does not affect command-line builds.

Build the app and run the GPU self-test:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -RunSelfTest
```

Build and run the complete synthetic test suite:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\tests\build_tests.ps1 -Run
```

Create a verified release after a fresh build and complete test run:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\package_release.ps1
```

Compiled files are written to `build`; the packaged user-facing files are written to `release`.
Use `-OutputDirectory <path>` to create a side-by-side package when an existing release is running.

## License

The app-specific source is available under the [MIT License](LICENSE.txt). MinHook is included
under the BSD 2-Clause License; its license is `third_party/minhook/LICENSE.txt` in the source tree
and `MinHook-LICENSE.txt` in the release. See
[THIRD-PARTY-NOTICES.txt](THIRD-PARTY-NOTICES.txt) for attribution.
