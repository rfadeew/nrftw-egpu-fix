# Changelog

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
