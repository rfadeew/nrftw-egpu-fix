# r/eGPU post

Markdown mode. Replace `GITHUB_URL` and `NEXUS_URL`. Check the subreddit rules and flair before you post
(for example "Discussion" or "Guide").

## Title

No Rest for the Wicked: 13.7 → 42 FPS on a TB4 eGPU without ReBAR by moving UPLOAD heaps into BAR1 (NVAPI, open-source beta)

## Body

TL;DR: *No Rest for the Wicked* reads its per-frame vertex, constant and SRV data straight from D3D12 `UPLOAD`
heaps in system memory. Over Thunderbolt without ReBAR, that makes the GPU stall most of the frame. I wrote a
`version.dll` proxy that creates those heaps in the 256 MB CPU-visible VRAM window (BAR1) through
`NvAPI_D3D12_CreateHeap2` + `NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM`. Worst spot: 13.7 → 42 FPS. Public beta,
NVIDIA only, looking for testers with other setups.

**Setup**

- i7-12700H laptop, TB4 port.
- RTX 5070 Ti in an enclosure. BAR1 is 256 MiB (no ReBAR over TB here).
- Driver 617.42, Windows 11 25H2.

**What I found** (from GPU timestamps and resource tracking in a D3D12 hook)

- The game (Unity, D3D12) binds 4 MB `UPLOAD` constant pages as root CBVs. It also places about 150 per-frame
  VB/SRV buffers per frame into a 32 MB `UPLOAD` heap. The GPU reads all of it from system memory.
- Moving the constant pages to VRAM copies (a first attempt) changed nothing measurable. The per-frame VB/SRV heap
  was the bottleneck.
- Measured vertex fetch from sysmem over TB4: about 1.1 GB/s. At the worst spot the GPU was busy 73 ms per frame,
  mostly waiting on those reads. The 5070 Ti eGPU was slower there than the laptop's internal 3060, which has
  ReBAR.

**The fix**

- The proxy hooks `CreateHeap`/`CreateHeap1`. For `UPLOAD` heaps it calls `NvAPI_D3D12_CreateHeap2` with the
  CPU-visible-vidmem flag instead. The game gets a normal heap; the data just lives in VRAM.
- CPU writes now go through BAR1 at about 0.8 GB/s (vs about 16 GB/s to sysmem). That is fine here because the
  GPU-side win is much bigger.

| | Off | On |
|---|---|---|
| Worst spot | 13.7 FPS, GPU busy 73 ms | 42 FPS, GPU busy 7 ms |
| Lighter spot | 29 FPS | 50–59 FPS (60 cap) |

**Things eGPU people might find interesting**

- Only part of the 256 MB BAR1 is usable: 192 MB (6 × 32 MB heaps) in a synthetic test, 160 MB in a game session.
  Past that, NVAPI quietly backs new "CPU-visible VRAM" heaps with cached system memory, and DXGI still reports
  them as local. The only reliable test I found is the CPU write speed (BAR1 over TB: about 0.8 GB/s; cached
  sysmem: 40+ GB/s). The proxy measures it for every heap. When a heap misses the window, it drops it, creates a
  normal one, and stops trying.
- `NvAPI_D3D12_QueryCpuVisibleVidmem` returns `NVAPI_NOT_SUPPORTED` on this setup, so the window size is not
  available from the driver.
- With the fix on, the worst spot becomes CPU-limited: the game's job threads write per-frame data straight into
  BAR1 at TB speed (about 3 → 14 ms CPU per worker per frame).
- The same pattern (shaders reading `UPLOAD` heaps directly) probably hurts other D3D12 games on eGPUs. The
  approach could be generalised, but this release only targets this game.

**Limitations**

- Beta, one test system. NVIDIA only. With ReBAR, or on AMD/Intel, it switches itself off.
- Tested in one 1-hour session; longer sessions and online co-op are not tested yet. The game has no anti-cheat, and the mod does not touch game
  logic or the network. Still, use it online at your own risk.
- To uninstall, delete `version.dll` and `egpu_fix.ini` from the game folder.

**If you test it**, please post your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU and before/after FPS,
plus `egpu_proxy.log` from the game folder (it contains no user names or paths). Set `Measure=1` in
`egpu_fix.ini` for a detailed log.

- Source (MIT) and write-up: GITHUB_URL
- Download: NEXUS_URL (or GitHub Releases)
