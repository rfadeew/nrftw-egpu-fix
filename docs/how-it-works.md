# How it works

## The problem

*No Rest for the Wicked* is a Unity game that renders with Direct3D 12 (Agility SDK 1.618). Like many D3D12
engines, it writes per-frame data into `D3D12_HEAP_TYPE_UPLOAD` memory and lets shaders read it from there:

- 4 MB `UPLOAD` pages with constant buffers, bound as root CBVs (hundreds of binds per frame);
- per-frame vertex buffers and structured buffers (root SRVs). About 150 buffers per frame are created and
  destroyed as placed resources inside a 32 MB `UPLOAD` heap.

`UPLOAD` heaps live in system memory. The GPU reads them across PCIe every time a shader touches them. In a
desktop PC that link is short and fast, so this is a common, cheap pattern.

An eGPU sits behind Thunderbolt: PCIe 3.0 x4 tunnelled through the Thunderbolt controller, with much higher
latency. On the test system:

- GPU vertex fetch from system memory was measured at about **1.1 GB/s**;
- at the worst spot the GPU was busy **73 ms per frame** (13.7 FPS), almost all of it waiting on these reads;
- the RTX 5070 Ti eGPU was slower there than the laptop's internal RTX 3060 (which has ReBAR and a direct
  PCIe link).

The usual fix for this pattern is Resizable BAR plus `D3D12_HEAP_TYPE_GPU_UPLOAD`. That heap type lets an
application put CPU-written data directly into VRAM. Most Thunderbolt setups have no ReBAR. The game does not
use `GPU_UPLOAD` either.

## The fix

Even without ReBAR, NVIDIA GPUs expose a 256 MB window of VRAM to the CPU (BAR1). Since driver R465, NVAPI can
place D3D12 heaps in that window:

```
NvAPI_D3D12_CreateHeap2(device, &heapDesc, &{ NV_HEAP_PARAMS_VER1, NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM }, riid, &heap)
```

The proxy hooks `ID3D12Device::CreateHeap` and `ID3D12Device4::CreateHeap1`. For `UPLOAD` heaps it calls
`NvAPI_D3D12_CreateHeap2` instead. The game gets an ordinary `ID3D12Heap` of the same type and size, and places
its buffers in it as before. The CPU writes go through BAR1 into VRAM, and the GPU reads them locally.

Placement in the window is only a hint to the driver, so every new heap is checked:

- the process's local/non-local memory usage from DXGI before and after;
- the CPU write speed into the new heap (512 KB memset). Writes through BAR1 over Thunderbolt run at about
  0.8 GB/s; anything above 5 GB/s cannot be BAR1 and means the driver used system memory.

The second check matters. Once the window is full, the driver backs new "CPU-visible VRAM" heaps with cached
system memory, and DXGI still reports them as local. Writes into such a heap run at 40+ GB/s. On the test system
the window filled up after 5 heaps of 32 MB (160 MB) during a game session, and after 6 heaps (192 MB) in
`smoke.exe probe`.

When that happens, the fix releases the new heap before the game sees it, and creates a normal `UPLOAD` heap
instead, exactly as without the fix. It logs a `SYSMEM` line and lowers its cap to what fitted, so later heaps go
straight to the normal path. `VidmemCapMB` (192 by default) is only an upper limit:
`NvAPI_D3D12_QueryCpuVisibleVidmem` returns `NVAPI_NOT_SUPPORTED` on the test system, so the driver cannot tell
how much is free.

## Measurements

From `egpu_proxy.log` with `Measure=1` (GPU timestamps around every game command list), same session, same
spots:

| | Fix off | Fix on |
|---|---|---|
| Worst spot | 13.5–14.0 FPS, GPU busy 71–74 ms/frame | 41.7–45.6 FPS, GPU busy 7.0–7.3 ms/frame |
| Lighter spot | 28–29.5 FPS, GPU busy 34–35 ms/frame | 50–59 FPS (60 FPS cap), GPU busy about 8 ms/frame |

With the fix on, the worst spot is CPU-limited: the GPU is idle most of the frame.

## Costs

| Access | System memory | BAR1 window over TB4 |
|---|---|---|
| CPU write | about 16 GB/s | about 0.8 GB/s |
| CPU read | fast | about 3.5 MB/s |
| GPU read | about 1.1 GB/s (vertex fetch) | VRAM speed |

- The game's job worker threads write per-frame data straight into these heaps. At the worst spot their CPU
  time grew from about 3 to about 14 ms per frame per thread. That is now the limit there.
- A CPU read from these heaps would be extremely slow. No such reads have been observed.
- The game's heap pool only grows: 32 → 96 MB within about 2 minutes, 224 MB after an hour. Only the heaps that
  fit in the window get the speed-up; the rest stay in system memory, the same as without the fix.

## A 1-hour session

51 minutes of normal play in fix-only mode (`Measure=0`), with an earlier build:

- 612 stats blocks of 5 s. Average 54.8 FPS; 482 blocks at 50 FPS or more, 2 below 20 FPS (loading).
- 7 heaps of 32 MB were created: 5 in the window, 2 after it was full. That build kept those 2 in the driver's
  cached system memory; the release creates normal heaps for them instead.
- Creating a heap during gameplay caused no visible hitch: the longest frame in those 5 s blocks was 39–42 ms.
- Single frames of 300–500 ms appeared in 6% of the blocks. Shorter runs without the fix had them in 17–25% of
  the blocks, so they come from the game.

## What was tried and dropped

- **Stage 2a, DEFAULT-heap twins of the constant pages:** root CBV binds were redirected to VRAM copies,
  and the used ranges were copied at every `ExecuteCommandLists`. Correct, but no measurable gain. Still in the
  code as `ConstPages=1` (`Measure=1` only), off by default.
- **Committed `UPLOAD` buffers in the window** (`VidmemBuffers=1`): works in the smoke test, with no evidence
  that it helps in the game. Off by default.

## Code map

- `src/version_proxy.cpp`: `DllMain`, `version.dll` export forwarding, log, ini.
- `src/hooks.cpp`: hooks and statistics. The fix is in the "Stage 2c" section (`InitVidmem`, `TryVidHeap`,
  `HeapWriteSpeed`, `SpeedVerdict`).
- `src/vtbl_index.c`: D3D12/DXGI vtable slot indices.
- `tests/smoke.cpp`: a standalone D3D12 test that loads `build\version.dll` and checks rendering results through
  readback. Run it with every combination of `Measure` and `VidmemHeaps`, plus the `break` negative test.
