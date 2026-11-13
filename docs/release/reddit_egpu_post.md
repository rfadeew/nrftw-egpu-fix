# r/eGPU post

Markdown mode. Replace `NEXUS_URL` (or use the GitHub Releases link). Check the subreddit rules and flair before you post
(for example "Discussion" or "Guide").

## Title

No Rest for the Wicked: about 12 → 40–46 FPS on a TB4 eGPU without ReBAR by moving UPLOAD heaps into VRAM with a write-watched shadow (open source, v0.2.0)

## Body

TL;DR: *No Rest for the Wicked* reads its per-frame vertex, constant and SRV data straight from D3D12 `UPLOAD`
heaps in system memory. Over Thunderbolt without ReBAR, that makes the GPU stall most of the frame. I wrote a
`version.dll` proxy that creates those heaps in VRAM and keeps a write-watched system-memory copy for the CPU;
only the changed pages are copied to VRAM each frame. Heaviest spot: about 12 → 40–46 FPS. NVIDIA only; looking
for testers with other setups. v0.1.0 put the heaps in the 256 MB BAR1 window instead (31–34 FPS, but 16–26 once
the window filled up in long sessions); v0.2.0 does not depend on that window.

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

**The fix (v0.2.0, "stage 2e")**

- The proxy hooks `CreateHeap`/`CreateHeap1`. A game `UPLOAD` heap becomes a `DEFAULT` heap in VRAM.
- `Map` of a buffer in it returns a system-memory "shadow" (`VirtualAlloc` with `MEM_WRITE_WATCH`, wrapped as a
  D3D12 heap with `OpenExistingHeapFromAddress`). The game's CPU writes stay fast, ordinary cached writes.
- At each game `ExecuteCommandLists` the pages the CPU wrote (`GetWriteWatch`) are copied shadow → VRAM on the
  proxy's own COPY queue, and the game's queue waits on a fence for it. Every GPU read is a VRAM read; only
  changed data crosses Thunderbolt (about 21 MB per frame at the heaviest spot).
- Fallback for heaps it cannot take: the v0.1.0 method (NVAPI heaps in the BAR1 window), then a normal heap.

| | No fix | v0.1.0 (BAR1) | v0.2.0 (shadow) |
|---|---|---|---|
| Heaviest spot | about 11–13 FPS | 31–34 FPS (16–26 once BAR1 is full) | about 40–46 FPS |

Per frame with v0.2.0: about 12 ms of GPU work plus about 7.6 ms of the GPU waiting for the copy over TB4
(copy-bandwidth bound, about 2.2–2.8 GB/s).

**Things eGPU people might find interesting**

- Only part of the 256 MB BAR1 is usable: 192 MB (6 × 32 MB heaps) in a synthetic test, 160 MB in a game session.
  Past that, NVAPI quietly backs new "CPU-visible VRAM" heaps with cached system memory, and DXGI still reports
  them as local. The only reliable test I found is the CPU write speed (BAR1 over TB: about 0.8 GB/s; cached
  sysmem: 40+ GB/s). The proxy measures it for every heap. When a heap misses the window, it drops it, creates a
  normal one, and stops trying.
- `NvAPI_D3D12_QueryCpuVisibleVidmem` returns `NVAPI_NOT_SUPPORTED` on this setup, so the window size is not
  available from the driver.
- With the v0.1.0 method, the worst spot became CPU-limited: the game's job threads wrote per-frame data straight
  into BAR1 at TB speed (about 3 → 14 ms CPU per worker per frame). The shadow copy avoids that: the CPU writes to
  normal system memory.
- Costs of v0.2.0: about 300–350 MB more system RAM (shadow heaps are not freed while the game runs), and a known
  aliasing risk: the copy target is one buffer placed over the whole VRAM heap, so it overlaps the game's buffers
  and is written on another queue (aliasing barrier only). Not seen to cause problems so far.
- The same pattern (shaders reading `UPLOAD` heaps directly) probably hurts other D3D12 games on eGPUs. The
  approach could be generalised, but this release only targets this game.

**Limitations**

- Tested on one system so far. NVIDIA only. `ShadowHeaps=0` in the ini gives the v0.1.0 behaviour. With ReBAR, or on AMD/Intel, it switches itself off.
- Tested in one 1-hour session; longer sessions and online co-op are not tested yet. The game has no anti-cheat, and the mod does not touch game
  logic or the network. Still, use it online at your own risk.
- To uninstall, delete `version.dll` and `egpu_fix.ini` from the game folder.

**If you test it**, please post your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU and before/after FPS,
plus `egpu_proxy.log` from the game folder (it contains no user names or paths). Set `Measure=1` in
`egpu_fix.ini` for a detailed log.

- Source (MIT) and write-up: https://github.com/rfadeew/nrftw-egpu-fix
- Download: NEXUS_URL (or GitHub Releases)
