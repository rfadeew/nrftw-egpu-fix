<!-- Tag: v0.2.0 | Title: No Rest for the Wicked eGPU Fix v0.2.0 | Set as the latest release -->
<!-- Attach: dist/No-Rest-for-the-Wicked-eGPU-Fix-v0.2.0.zip. SHA-256 below match this zip; rerun package.bat and update them if it is rebuilt. -->

**Performance fix for NVIDIA eGPUs without Resizable BAR** (Thunderbolt / USB4). Please read the limitations
below and report your results.

## What it does

*No Rest for the Wicked* reads its per-frame vertex, shader-resource and constant data from D3D12 `UPLOAD`
heaps in system memory. Over Thunderbolt without ReBAR every one of those GPU reads crosses a slow link, and the
GPU spends most of the frame waiting. This `version.dll` proxy creates those heaps in VRAM, keeps a copy in
system memory for the CPU to write to, and copies only the changed pages to VRAM before the GPU uses them. The GPU
then reads everything from its own VRAM. Game files are not modified.

## What is new in v0.2.0

- New default fix (`ShadowHeaps=1`): VRAM heaps with a write-watched system-memory shadow. It no longer depends on
  the small BAR1 window, so it keeps working in long sessions when the game's heap pool grows.
- The v0.1.0 fix (BAR1 window) stays as the fallback for heaps the new fix cannot take. `ShadowHeaps=0` gives the
  v0.1.0 behaviour.
- Costs: about 300-350 MB more system RAM, and a known aliasing risk (see below).

## Results (test system)

- Laptop with an i7-12700H and a Thunderbolt 4 port.
- eGPU: RTX 5070 Ti, 16 GB, BAR1 256 MiB, no ReBAR.
- NVIDIA driver 617.42, Windows 11 25H2, game Steam build 22928553.

Heaviest spot, same session:

| No fix | v0.1.0 | v0.2.0 |
|---|---|---|
| about 11–13 FPS | 31–34 FPS (16–26 FPS once the BAR1 window is full) | about 40–46 FPS |

Per frame at that spot with v0.2.0: about 12 ms of GPU work plus about 7.6 ms of the GPU waiting for the copy of
changed data over Thunderbolt (bandwidth-bound, about 2.2–2.8 GB/s).

## Install

1. Download `No-Rest-for-the-Wicked-eGPU-Fix-v0.2.0.zip` below.
2. Copy `version.dll` and `egpu_fix.ini` into the game folder (Steam → right-click the game → *Manage* →
   *Browse local files*).
3. Start the game. `egpu_proxy.log` in the game folder should contain `shadow: stage 2e ON` and `shadow: UPLOAD heap` lines.

MelonLoader also installs a `version.dll`. Do not overwrite it: rename MelonLoader's `version.dll` to
`winhttp.dll` first, then copy this fix's files. This combination is not tested yet.

## Uninstall

Delete `version.dll` and `egpu_fix.ini` (and `egpu_proxy*.log`) from the game folder.

## Requirements

- An NVIDIA GPU without ReBAR. On GPUs with ReBAR, and on AMD or Intel GPUs, the fix switches itself off.
- Windows 10/11 x64 with a current NVIDIA driver. Only Windows 11 25H2 with driver 617.42 has been tested.

## Known limitations

- **More system RAM:** about 300–350 MB (copies of the game's upload heaps). They are not freed while the game
  runs; the game does not release these heaps today.
- **Copy bandwidth** over Thunderbolt limits the frame rate at the heaviest spot (about 21 MB per frame).
- **Known risk, aliasing:** the copy target is one buffer placed over a whole VRAM heap, so it overlaps the game's
  own buffers. It is written on the mod's COPY queue while the game may read other parts of the heap on another
  queue (only an aliasing barrier is used). No problem observed; the smoke tests pass.
- **Fallback (BAR1 window, the v0.1.0 fix), only for heaps the main fix did not take:** CPU writes are slow
  (about 0.8 GB/s over TB4), CPU reads are extremely slow (about 3.5 MB/s), and only 160–192 MB of the 256 MB
  window is usable. When it is full the fix logs a `SYSMEM` line and creates normal heaps, as without the fix.
- **The game's heap pool only grows.** Sessions longer than about an hour are not tested with v0.2.0.
- **Online co-op has not been tested with the fix.** The game ships no anti-cheat as of Steam build 22928553, and
  the fix does not touch game logic, saves or network traffic. Still, use it online at your own risk.
- If the game crashes, remove the fix and retry before you report the crash to Moon Studios.

## Reporting problems

Open an issue with:

- `egpu_proxy.log` from the game folder (the previous run is `egpu_proxy.prev.log`; the log has no user names
  or folder paths);
- your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU, driver version and graphics settings.

If something looks wrong, try `ShadowHeaps=0` (v0.1.0 behaviour) and say in the report whether it changes anything.

For performance problems, set `Measure=1` in `egpu_fix.ini`, play 1–2 minutes at the problem spot, then attach
that log.

## Checksums (SHA-256)

```
154cadf2de20306c36d9f31e233830733f504de02d8ef399f8200473a85b111a  No-Rest-for-the-Wicked-eGPU-Fix-v0.2.0.zip
97a72d887b3e8300bcbb07388f839dc545bf51ed9e4e6c9e806b4746b7c70ce2  version.dll
```

MIT License. Uses [MinHook](https://github.com/TsudaKageyu/minhook) (BSD 2-clause). Not affiliated with Moon
Studios, Private Division or NVIDIA.
