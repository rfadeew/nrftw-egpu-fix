<!-- Tag: v0.1.0-beta | Title: v0.1.0-beta: first public beta | Mark as pre-release -->
<!-- Attach: dist/No-Rest-for-the-Wicked-eGPU-Fix-v0.1.0-beta.zip. SHA-256 below match this zip; rerun package.bat and update them if the code changes. -->

**Public beta for NVIDIA eGPUs without Resizable BAR** (Thunderbolt / USB4). It has been tested on one system
only. Please read the limitations below and report your results.

## What it does

*No Rest for the Wicked* reads its per-frame vertex, shader-resource and constant data from D3D12 `UPLOAD`
heaps in system memory. Over Thunderbolt without ReBAR every one of those GPU reads crosses a slow link, and the
GPU spends most of the frame waiting. This `version.dll` proxy creates those heaps in the GPU's CPU-visible VRAM
window (BAR1) through NVAPI, so the GPU reads them from its own VRAM. Game files are not modified.

## Results (test system)

- Laptop with an i7-12700H and a Thunderbolt 4 port.
- eGPU: RTX 5070 Ti, 16 GB, BAR1 256 MiB, no ReBAR.
- NVIDIA driver 617.42, Windows 11 25H2, game Steam build 22928553.

| | Fix off | Fix on |
|---|---|---|
| Worst spot | 13.7 FPS, GPU busy 73 ms/frame | 42 FPS, GPU busy 7 ms/frame |
| Lighter spot | 29 FPS | 50–59 FPS (60 FPS cap) |

## Install

1. Download `No-Rest-for-the-Wicked-eGPU-Fix-v0.1.0-beta.zip` below.
2. Copy `version.dll` and `egpu_fix.ini` into the game folder (Steam → right-click the game → *Manage* →
   *Browse local files*).
3. Start the game. `egpu_proxy.log` in the game folder should contain `vidmem: stage 2c ON` and `-> VRAM` lines.

MelonLoader also installs a `version.dll`. Do not overwrite it: rename MelonLoader's `version.dll` to
`winhttp.dll` first, then copy this fix's files. This combination is not tested yet.

## Uninstall

Delete `version.dll` and `egpu_fix.ini` (and `egpu_proxy*.log`) from the game folder.

## Requirements

- An NVIDIA GPU without ReBAR. On GPUs with ReBAR, and on AMD or Intel GPUs, the fix switches itself off.
- Windows 10/11 x64 with a current NVIDIA driver. Only Windows 11 25H2 with driver 617.42 has been tested.

## Known limitations

- **CPU writes into the VRAM window are slow** (about 0.8 GB/s over TB4, versus about 16 GB/s to system memory).
  Heavy scenes can become CPU-limited. At the worst spot on the test system, CPU time per game job thread grew
  from about 3 to about 14 ms per frame.
- **CPU reads from the window are extremely slow** (about 3.5 MB/s). The game was not seen reading these buffers.
  If it ever did, it would stall.
- **Only 160–192 MB of the 256 MB window is usable** (160 MB in a game session on the test system). When it is
  full, the driver silently uses system memory. The fix detects this, logs a `SYSMEM` line and creates normal
  heaps from then on, exactly as without the fix. Rendering stays correct; only those heaps miss the speed-up.
- **The game's heap pool only grows** (32 → 224 MB in a 1-hour session). Longer sessions are not tested yet.
- **Online co-op has not been tested with the fix.** The game ships no anti-cheat as of Steam build 22928553, and
  the fix does not touch game logic, saves or network traffic. Still, use it online at your own risk.
- If the game crashes, remove the fix and retry before you report the crash to Moon Studios.

## Reporting problems

Open an issue with:

- `egpu_proxy.log` from the game folder (the previous run is `egpu_proxy.prev.log`; the log has no user names
  or folder paths);
- your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU, driver version and graphics settings.

For performance problems, set `Measure=1` in `egpu_fix.ini`, play 1–2 minutes at the problem spot, then attach
that log.

## Checksums (SHA-256)

```
09def442d91163248e2cd888f0880947b103c987f3fef4f3441475458cf0e4aa  No-Rest-for-the-Wicked-eGPU-Fix-v0.1.0-beta.zip
44774573fa37455108efd21caf7cc72513a529e33b0a6d3a752376bb2d9064b6  version.dll
```

MIT License. Uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause). Not affiliated with Moon
Studios, Private Division or NVIDIA.
