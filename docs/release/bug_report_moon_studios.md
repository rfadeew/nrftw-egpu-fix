# Bug report draft for Moon Studios

Where to send it: the official forum's bug report section (forum.norestforthewicked.com) or the in-game / support
bug report channel. Attach `logs/run8_vid0.log` and `logs/run9_vid1.log` only after you have checked them for
personal paths: the first line of these older logs contains the Steam library path. Offer the PIX captures on
request instead of attaching them (1.2 GB).

---

**Title:** Severe GPU stalls on eGPUs without Resizable BAR: shaders read per-frame data directly from UPLOAD heaps (13.7 FPS → 42 FPS when moved to VRAM)

**Summary**

On an NVIDIA eGPU over Thunderbolt 4 without Resizable BAR, the game is GPU-bound at 13–30 FPS in places where
the GPU should be mostly idle. In the same scenes the laptop's internal RTX 3060 (with ReBAR) is faster than an
RTX 5070 Ti eGPU. Other demanding D3D12 games run normally on the same eGPU.

The cause is that shaders read constants, vertex buffers and structured buffers directly from
`D3D12_HEAP_TYPE_UPLOAD` memory, which is in system memory. Over Thunderbolt every such read crosses a slow,
high-latency link. When the same heaps are created in CPU-visible VRAM instead (test below), GPU time per frame
drops from 73 ms to 7 ms.

**System**

- Laptop: Intel Core i7-12700H, Thunderbolt 4 (CPU-integrated controller), internal RTX 3060 Laptop 6 GB (ReBAR on).
- eGPU: RTX 5070 Ti 16 GB over TB4, monitor connected to the eGPU. BAR1 = 256 MiB (no ReBAR over TB).
- NVIDIA driver 617.42, Windows 11 25H2 (build 26200).
- Game: Steam build 22928553, D3D12 with Agility SDK 1.618 (`D3D12SDKVersion` 618).

**Observations** (worst spot, from D3D12 API hooks with GPU timestamps around every command list)

| Per frame | Value |
|---|---|
| FPS / GPU busy | 13.7 FPS / 73.1 ms |
| Root CBV binds | 7,948 graphics + 148 compute, 100% into 4 MB committed `UPLOAD` buffers (constant pages) |
| Vertex buffer views | 5,071, of which 1,796 (35%) in `UPLOAD` |
| SRV descriptors | 287, 100% on `UPLOAD` buffers |
| New `UPLOAD` buffers | 144 created and destroyed per frame (7.9 MB), placed in a 32 MB `UPLOAD` heap; used as VB and SRV |
| GPU-read system memory | about 50 MB per frame (32 MB in buffers ≥ 1 MB, 18 MB in about 590 small ranges) |
| Command lists with draws that use `UPLOAD` VBs | 35 lists, 1,924 draws, 59.0 ms GPU (30.6 µs per draw) |
| Command lists without `UPLOAD` VBs | 35 lists, 688 draws, 12.8 ms GPU (18.6 µs per draw) |

- PIX GPU capture: root parameter 0 (CBV) of a typical draw points into a 4 MB committed `UPLOAD` resource
  (`GENERIC_READ`).
- PIX timing capture: on the direct queue the whole frame is one execution, about 30 ms at a lighter spot and
  about 84 ms at the worst spot. There, all command lists are uniformly 2.5–3× longer with about the same draw
  count, CPU threads idle waiting on the GPU, and the copy queue nearly empty, so texture streaming is not the cause.
- Measured GPU vertex fetch from system memory over this link: about 1.1 GB/s.

**Experiment**

A local test DLL hooked `ID3D12Device::CreateHeap` / `ID3D12Device4::CreateHeap1`. It created the game's `UPLOAD`
heaps with `NvAPI_D3D12_CreateHeap2(..., NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM)`, which puts them in VRAM inside
the 256 MB BAR1 window. Nothing else was changed: same heap type and size, same addresses, same data, same
shaders. Same session, same spots:

| | Default | `UPLOAD` heaps in CPU-visible VRAM |
|---|---|---|
| Worst spot | 13.7 FPS, GPU busy 73 ms/frame | 42 FPS, GPU busy 7.2 ms/frame |
| Lighter spot | 29 FPS, GPU busy 34 ms/frame | 50–59 FPS (60 FPS cap), GPU busy about 8 ms/frame |

GPU time per draw, averaged over all draw lists, went from 27.5 µs to 2.5 µs. The game then became CPU-bound.

The gain comes from the 32 MB heap with the per-frame VB/SRV buffers. The 4 MB constant pages (committed
`UPLOAD` buffers) stayed in system memory in this test. A separate test redirected root CBVs to `DEFAULT`-heap
copies of the constant pages, and FPS did not change measurably. So the constant reads are not the bottleneck
here.

This workaround is limited. Only about 160–192 MB of BAR1 is usable. CPU writes into BAR1 over TB4 run at about
0.8 GB/s, and the job worker threads that fill these buffers became about 4× slower. A proper fix belongs in the
engine.

**Suggestions**

1. Stage the per-frame vertex and structured-buffer data into `DEFAULT` heaps. Write it to `UPLOAD` memory as
   today, then copy it with `CopyBufferRegion` (copy queue, or once at the start of the frame). A copy reads
   each byte once in large bursts. That is far cheaper over PCIe/Thunderbolt than vertex fetch and SRV loads
   reading system memory directly (about 1.1 GB/s measured, latency-bound).
2. Where `D3D12_FEATURE_DATA_D3D12_OPTIONS16::GPUUploadHeapSupported` is true (ReBAR systems), use
   `D3D12_HEAP_TYPE_GPU_UPLOAD` for this data. That alone does not help eGPUs without ReBAR, so (1) is still
   needed there.
3. Pool the per-frame VB/SRV buffers instead of creating and destroying about 144 placed resources per frame
   (about 10,000 per 5 s). This also removes CPU overhead on every system.

Logs with the full per-frame statistics, and PIX timing/GPU captures, are available on request. I have published the
test DLL as an open-source workaround for other eGPU users (https://github.com/rfadeew/nrftw-egpu-fix). It is not meant to replace a fix in the
game.
