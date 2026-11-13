# Changelog

## v0.2.0 (unreleased)

- New default fix, "stage 2e" (`[Fix] ShadowHeaps=1`): the game's `UPLOAD` heaps are created as `DEFAULT` heaps
  in VRAM. `Map` of a buffer in them returns a system-memory "shadow" (`VirtualAlloc` with `MEM_WRITE_WATCH`,
  wrapped as a D3D12 heap with `ID3D12Device3::OpenExistingHeapFromAddress`). At every game
  `ExecuteCommandLists`, the pages the CPU wrote (`GetWriteWatch`) are copied shadow -> VRAM on the mod's own
  COPY queue, and the game's queue waits on a fence for that copy. Every GPU read of that data is then a VRAM
  read, and only changed data crosses Thunderbolt (about 21 MB per frame at the heaviest spot).
  - Same gating as before: NVIDIA GPU without ReBAR, and the device must support existing heaps. It switches
    itself off otherwise and logs why.
  - It no longer depends on the 256 MB BAR1 window, so long sessions no longer fall back to system memory when
    the game's heap pool grows.
- The v0.1.0 fix (stage 2c, `VidmemHeaps=1`) stays on as the fallback: a heap that stage 2e cannot take goes to
  stage 2c, and otherwise stays a normal heap. `ShadowHeaps=0` gives the v0.1.0 behaviour.
- Costs: system RAM use rises by the size of the game's upload heaps (about 300-350 MB). Shadow heaps are not
  freed while the game runs (the game does not release these heaps today).
- Results on the test system (RTX 5070 Ti eGPU over TB4, no ReBAR), heaviest spot, same session: no fix about
  11-13 FPS; v0.1.0 31-34 FPS (16-26 FPS once the BAR1 window is full); v0.2.0 about 40-46 FPS.

## v0.1.0 (2026-11-13)

First public release.

- `version.dll` proxy: forwards every `version.dll` export to `System32`, hooks D3D12 heap creation (MinHook).
- The fix: the game's `UPLOAD` heaps are created in CPU-visible VRAM (BAR1) through
  `NvAPI_D3D12_CreateHeap2` + `NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM`.
  - Runs only on an NVIDIA GPU without ReBAR (`GPUUploadHeapSupported=0`). On other GPUs, with ReBAR, or without
    `nvapi64.dll` it switches itself off and logs why.
  - Every new heap's CPU write speed is measured. When the driver silently puts a heap in system memory (window
    full), the fix logs `SYSMEM`, creates a normal heap instead and puts no more heaps in the window. Upper
    limit: `VidmemCapMB=192`.
- Defaults: fix on, lean fix-only mode (`Measure=0`). `Measure=1` adds full measurement (GPU timing, buffer
  statistics) for bug reports.
- `egpu_proxy.log` in the game folder (previous run: `egpu_proxy.prev.log`), without user names or folder paths.
- Results on the test system (RTX 5070 Ti eGPU over TB4, no ReBAR): worst spot 13.7 → 42 FPS,
  lighter spot 29 → 50–59 FPS (60 FPS cap).
