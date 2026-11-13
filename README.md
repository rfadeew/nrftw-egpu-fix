# No Rest for the Wicked eGPU Fix

A small `version.dll` proxy that makes *No Rest for the Wicked* (NRFTW) run much faster on
**NVIDIA eGPUs without Resizable BAR** (Thunderbolt / USB4 enclosures). Game files are not modified.

On the test system the heaviest spot went from **about 11–13 to about 40–46 FPS**.

> Tested on one system so far. Read [Limitations](#limitations-and-trade-offs) before you install it.

## Who it is for

| Your setup | What the fix does |
|---|---|
| NVIDIA GPU in an eGPU enclosure (Thunderbolt 3/4/5, USB4), **no ReBAR** | The case it was made for. |
| NVIDIA GPU, no ReBAR, other link (OCuLink, internal PCIe) | Switches on. Untested; may help less. |
| NVIDIA GPU **with ReBAR** | Switches itself off. Nothing changes. |
| AMD or Intel GPU | Switches itself off. Nothing changes. |

How to check ReBAR: *NVIDIA Control Panel → System Information → Resizable BAR*. Over Thunderbolt it is
almost always "No".

## Results

Test system:

- Laptop with an Intel Core i7-12700H and a Thunderbolt 4 port.
- eGPU: RTX 5070 Ti, 16 GB. BAR1 is 256 MiB (no ReBAR).
- NVIDIA driver 617.42, Windows 11 25H2.
- Game: Steam build 22928553 (November 2026).

Same session, heaviest spot (FPS from `egpu_proxy.log`):

| | No fix | v0.1.0 | v0.2.0 |
|---|---|---|---|
| Heaviest spot | about 11–13 FPS | 31–34 FPS (16–26 FPS once the BAR1 window is full) | about 40–46 FPS |

Your numbers will differ. Remaining frame time at that spot with v0.2.0: about 12 ms of GPU work plus about
7.6 ms of the GPU waiting for the copy of changed data over Thunderbolt. That copy is bandwidth-bound
(about 2.2–2.8 GB/s). Lighter spots hit the 60 FPS cap. The v0.1.0 measurements are in
[docs/how-it-works.md](docs/how-it-works.md).

## Install

1. Download `No-Rest-for-the-Wicked-eGPU-Fix-v0.2.0.zip` from the
   [Releases](../../releases) page.
2. Open the game folder. In Steam: right-click the game → *Manage* → *Browse local files*. It is the folder that
   contains `NoRestForTheWicked.exe`.
3. Copy `version.dll` and `egpu_fix.ini` from the zip into that folder.
4. Start the game normally.

To check that it works, open `egpu_proxy.log` in the game folder (it appears after the first start). Look for:

```
shadow: stage 2e ON for NVIDIA GeForce RTX ...
shadow: UPLOAD heap #1 32.00 MB (...): shadow ..., VRAM heap VA ...
```

Every few seconds there is a stats line with FPS. If `shadow: stage 2e OFF` appears instead, the line says why;
the fallback (`vidmem: stage 2c ON ...`) may then take over.

## Uninstall

Delete `version.dll` and `egpu_fix.ini` from the game folder. You can also delete `egpu_proxy.log` and
`egpu_proxy.prev.log`. Nothing else is changed.

### MelonLoader and other `version.dll` mods

[MelonLoader](https://github.com/LavaGang/MelonLoader) also installs a `version.dll` in the game folder. Do not
overwrite it. Rename MelonLoader's `version.dll` to `winhttp.dll` first (MelonLoader supports that name), then
copy this fix's files. This combination has not been tested yet; please report how it goes.

If a game update ever ships its own `version.dll` in the game folder, do not overwrite it. Open an issue instead.

## Settings (`egpu_fix.ini`)

All keys are optional. If the file is missing, the defaults below are used.

| Key | Default | Meaning |
|---|---|---|
| `[Fix] ShadowHeaps` | `1` | `1` = the main fix (stage 2e). `0` = off. |
| `[Fix] VidmemHeaps` | `1` | `1` = fallback (the v0.1.0 fix) for heaps the main fix could not take. With `ShadowHeaps=0` it is the only fix. `0` = off. Both `0`: the DLL only logs FPS. |
| `[Fix] VidmemCapMB` | `192` | Upper limit in MB for the fallback's use of the 256 MB CPU-visible VRAM window. The fallback also stops by itself when the window is full. |
| `[General] Measure` | `0` | `1` = detailed measurement for bug reports. Costs FPS; set it back to `0` afterwards. |
| `[Stats] IntervalSec` | `5` | Seconds between stats lines in the log. |

## Reporting problems

Open an issue on GitHub (or post on the Nexus page) and include:

- `egpu_proxy.log` from the game folder, from the run with the problem. The previous run is in
  `egpu_proxy.prev.log`. The log contains the GPU name, driver-reported memory sizes, the Windows build and
  FPS. It contains no user names or folder paths.
- Your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU, driver version, resolution and quality settings.
- For performance problems: play 1–2 minutes at the problem spot with `Measure=1` in `egpu_fix.ini`, then attach
  that log.

## Limitations and trade-offs

- **More system RAM.** The main fix keeps a copy of the game's upload heaps in system memory: about 300–350 MB
  more. These copies are not freed while the game runs (the game does not release these heaps today).
- **Copy bandwidth limits the frame rate.** At the heaviest spot about 21 MB per frame cross Thunderbolt, and the
  GPU waits about 7.6 ms per frame for that copy.
- **Known risk: aliasing.** The copy target is one buffer placed over a whole VRAM heap, so it overlaps the
  game's own buffers in that heap. It is written on the mod's COPY queue while the game may read other parts of
  the heap on another queue. No problem has been observed and the smoke tests pass; see
  [docs/how-it-works.md](docs/how-it-works.md).
- **Switching back to v0.1.0 behaviour:** set `ShadowHeaps=0` in `egpu_fix.ini`. The limits below apply to that
  fallback (v0.1.0 fix), and in v0.2.0 only to the heaps the main fix did not take.
- **CPU writes get slower** (fallback only). The game's job threads write per-frame data straight into these buffers. Writes into
  the VRAM window cross Thunderbolt at about 0.8 GB/s instead of about 16 GB/s to system memory. At the worst spot
  on the test system, each job worker thread went from about 3 to about 14 ms of CPU time per frame. The game is
  then CPU-limited there instead of GPU-limited. A slower CPU may gain less.
- **CPU reads from the window are extremely slow** (fallback only) (about 3.5 MB/s). If the game ever reads these buffers back,
  it would stall badly. This has not been seen.
- **The window is small** (fallback only). Without ReBAR only 256 MB of VRAM is CPU-visible. On the test system 160 MB of it
  was usable during a game session (5 heaps of 32 MB), and 192 MB in a synthetic test.
  - When the window is full, the driver quietly puts a new heap in system memory, while still reporting it as VRAM.
    The fix measures CPU write speed on every new heap, so it notices this. It logs a `SYSMEM` line, creates a
    normal heap instead (exactly as without the fix), and puts no more heaps in the window after that.
  - A `SYSMEM` line is normal and needs no action. Rendering stays correct; only the heaps that did not fit miss
    the speed-up.
  - The driver does not report the window size here (`NvAPI_D3D12_QueryCpuVisibleVidmem` returns
    `NVAPI_NOT_SUPPORTED`). `VidmemCapMB` is only an upper limit.
- **The game's heap pool only grows.** In a 1-hour session (v0.1.0) it grew from 32 MB to 224 MB: 160 MB in the
  window, the rest in normal system memory. In a later session it reached 11 heaps while the window fitted 5; with
  the main fix this no longer matters, since heaps do not need the window. Sessions longer than about an hour
  have not been tested with v0.2.0.
- **NVIDIA only.** The fallback uses NVAPI, which is part of the NVIDIA driver. On other GPUs the fix switches
  itself off and the DLL only forwards `version.dll` calls.
- **Tested on one system** (see [Results](#results)). Windows 10, other drivers, other GPUs and laptops with other
  hybrid graphics setups are untested.
- Game updates can change how the game uses GPU memory. The fix could then stop helping, or stop being needed.

## Online play, co-op and anti-cheat

- As of Steam build 22928553 the game ships no anti-cheat (no Easy Anti-Cheat, no BattlEye in the game folder).
- The fix does not change game logic, saves or network traffic. It only changes which memory the GPU reads the
  game's per-frame upload buffers from. The rendered image is the same.
- **Co-op has not been tested with the fix yet.** You use it online at your own risk. If the game adds anti-cheat
  in a future update, remove the fix until it is checked.
- The game's crash reporter will see `version.dll` in crash dumps. If the game crashes, remove the fix and check
  whether the crash still happens before you report it to Moon Studios.

## How it works

NRFTW (Unity, Direct3D 12) keeps per-frame vertex, shader-resource and constant data in D3D12 `UPLOAD` heaps.
Those live in system memory, and the GPU reads them directly every frame. Inside a PC that is fine. Over
Thunderbolt without ReBAR every such read crosses a slow, high-latency link. Vertex fetch from system memory was
measured at about 1.1 GB/s, and the GPU spends most of the frame waiting.

The fix is a `version.dll` proxy. Windows loads it from the game folder instead of the system copy. It:

1. forwards every real `version.dll` function to `C:\Windows\System32\version.dll`;
2. hooks D3D12 heap creation with [MinHook](https://github.com/TsudaKageyu/minhook);
3. creates the game's `UPLOAD` heaps as `DEFAULT` heaps in VRAM. `Map` of a buffer in them returns a
   system-memory copy (a "shadow"). At every `ExecuteCommandLists` the pages the CPU wrote are copied to VRAM on
   the mod's own COPY queue, and the game's queue waits for that copy (stage 2e, `ShadowHeaps=1`);
4. falls back, for heaps it cannot take, to NVIDIA's `NvAPI_D3D12_CreateHeap2` with
   `NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM`. This places them in the CPU-visible VRAM window (BAR1), which exists
   even without ReBAR (stage 2c, the v0.1.0 fix, `VidmemHeaps=1`).

The GPU then reads that data from its own VRAM, and only changed data crosses Thunderbolt. The game sees the same
kind of heap, with the same data. More detail and measurements: [docs/how-it-works.md](docs/how-it-works.md).

## Building from source

Requirements: Windows 10/11 x64, Visual Studio 2019 or 2022 (or Build Tools) with the *Desktop development with
C++* workload.

```
build.bat
```

Output: `build\version.dll`. `package.bat` builds the release zip in `dist\` and prints its SHA-256.

Tests (they need an NVIDIA GPU; read the header of `tests\smoke.cpp`):

```
tests\build_smoke.bat
copy tests\egpu_fix.dev.ini build\egpu_fix.ini
build\smoke.exe 60 90 60          (must print SMOKE PASSED)
build\smoke.exe 30 30 30 break    (negative test, must FAIL)
```

## License and credits

- MIT License, see [LICENSE](LICENSE). Copyright (c) 2026 rfadeew.
- MinHook by Tsuda Kageyu (BSD 2-clause). NVAPI function IDs and struct layouts from NVIDIA's public NVAPI SDK
  (MIT). See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Not affiliated with Moon Studios, Private Division or NVIDIA.
