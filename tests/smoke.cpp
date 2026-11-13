// Smoke test for build\version.dll without the game:
// loads the proxy, then does the D3D12 calls the hooks care about.
//   Phase 1: binding/copy/Map calls without draws, ~N frames of Present.
//   Phase 2: real draws and dispatches that read root CBVs from two 4 MB
//            UPLOAD "constant pages"; results are read back and checked.
//            Covers stage 2a ([Fix] ConstPages=1): redirects, per-list ranges,
//            several lists per ExecuteCommandLists, two submissions per frame,
//            re-executing lists without Reset, and a page being recreated.
//   Phase 3: per-frame buffers placed in a 32 MB UPLOAD heap (like the game),
//            used as VB + root SRV by real draws, results checked. Covers
//            stage 2c ([Fix] VidmemHeaps=1). Then a benchmark: 8 MB vertex
//            fetch + CPU write/read speed, UPLOAD heap vs a sysmem reference heap.
//   Phase 4: stage 2d ([Fix] HybridHeaps / HybridSrv / HybridCheck): per-frame
//            buffers in 32 MB UPLOAD heaps read as VB and through descriptor-
//            table SRVs (raw, structured stride 12, typed; created in a CPU
//            heap, copied into a shader-visible one), two submissions per
//            frame, SRVs created before their data is written, a buffer
//            destroyed and recreated at the same offset, a second heap, and a
//            "persistent" buffer rewritten in place every frame behind an SRV
//            created once. That last one MUST read stale data with HybridSrv=1
//            and no HybridCheck (checked from build\egpu_fix.ini), and must be
//            repaired with HybridCheck=1 (Measure=1).
// Usage: smoke.exe [phase1 frames] [phase2 frames] [phase3 frames] [phase4 frames] [break]
//        "break" skips one VB write in phase 3 (negative test: must FAIL).
//        [Fix] HybridTestNoCopy=1 in the ini is the stage 2d negative test (must FAIL).
// Exit code 0 = all checks passed.
// Check build\egpu_proxy.log afterwards.
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <stdio.h>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

// Agility SDK like the game (1.618): with build\D3D12\D3D12Core.dll present (copy it from
// <game>\D3D12\) phase 4 uses tight-aligned buffers (D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT,
// 4 KB offsets) like the game's; without it the OS runtime is used and they are skipped.
extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 618; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAILED 0x%08lx: %s (line %d)\n", hr_, #x, __LINE__); return 1; } } while (0)

static ID3D12Resource* MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES st,
                                  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp = { type };
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = flags;
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br = {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    return br;
}

static const char kHlsl[] = R"(
cbuffer CB : register(b0) { uint g_value; uint g_index; };
RWStructuredBuffer<uint> g_out : register(u0);
float4 VSMain(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
uint PSMain() : SV_Target { return g_value; }
[numthreads(1, 1, 1)] void CSMain() { g_out[g_index] = g_value; }

// Phase 3: value = vertex attribute (VB) ^ first dword of a root SRV.
ByteAddressBuffer g_srv : register(t0);
struct VbOut { float4 pos : SV_Position; nointerpolation uint val : VAL; };
VbOut VSVb(uint val : VAL, uint id : SV_VertexID)
{
    VbOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.val = val ^ g_srv.Load(0);
    return o;
}
uint PSVb(VbOut i) : SV_Target { return i.val; }
// Phase 3 benchmark: fetch a float4 per vertex; every point is clipped.
float4 VSBench(float4 v : POS) : SV_Position { return float4(2.0 + abs(v.x) + abs(v.y) + abs(v.z) + abs(v.w), 0, 0, 1); }
)";

// Phase 4 (shader model 5.1: register spaces): VB value ^ raw ^ structured (stride 12) ^ typed
// ^ "persistent" buffer, all four SRVs from one descriptor table.
static const char kHlsl4[] = R"(
struct VbOut { float4 pos : SV_Position; nointerpolation uint val : VAL; };
uint PSVb(VbOut i) : SV_Target { return i.val; }
ByteAddressBuffer       d4raw : register(t0, space1);
StructuredBuffer<uint3> d4st  : register(t1, space1);
Buffer<uint>            d4ty  : register(t2, space1);
ByteAddressBuffer       d4p   : register(t3, space1);
VbOut VSDesc(uint val : VAL, uint id : SV_VertexID)
{
    VbOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    o.val = val ^ d4raw.Load(0) ^ d4st[0].y ^ d4ty[0] ^ d4p.Load(0);
    return o;
}
)";

static ID3DBlob* Compile(const char* entry, const char* target, const char* src = kHlsl)
{
    ID3DBlob* code = nullptr; ID3DBlob* err = nullptr;
    if (FAILED(D3DCompile(src, strlen(src), "smoke.hlsl", nullptr, nullptr, entry, target, 0, 0, &code, &err))) {
        printf("shader %s failed: %s\n", entry, err ? (const char*)err->GetBufferPointer() : "?");
        return nullptr;
    }
    return code;
}

// ---- Phase 2 constants ---------------------------------------------------------
static const UINT kW = 64, kH = 16, kDraws = kW * kH, kDisp = 64;
static const UINT64 kPage = 4ull << 20, kCb = 256;
static const UINT64 kFrameStride = 160 << 10;   // ring advance per frame
static const UINT kRingSlots = 24;              // 24 * 160 KB + 144 KB < 4 MB

static UINT Val(UINT seed, UINT k) { return (seed * 2654435761u) ^ (k * 40503u + 0x9e3779b9u); }
// Phase 3 SRV value: different k mix, so VB ^ SRV still depends on k.
static UINT SrvVal(UINT seed, UINT k) { return Val(seed ^ 0x5bd1e995u, (k * 7u + 3u) ^ 0x3c3cu); }

struct Cb { UINT value, index; };

// Draw k: page A if even, B if odd, slot k/2. Dispatch j: page A after the draws.
static UINT64 DrawOff(UINT64 base, UINT k) { return base + (k >> 1) * kCb; }
static UINT64 DispOff(UINT64 base, UINT j) { return base + (kDraws / 2) * kCb + j * kCb; }

static void WriteConstants(BYTE* a, BYTE* b, UINT64 base, UINT seed)
{
    for (UINT k = 0; k < kDraws; ++k) {
        Cb* c = (Cb*)((k & 1 ? b : a) + DrawOff(base, k));
        c->value = Val(seed, k); c->index = k;
    }
    for (UINT j = 0; j < kDisp; ++j) {
        Cb* c = (Cb*)(a + DispOff(base, j));
        c->value = Val(seed, kDraws + j); c->index = j;
    }
}

// "probe" (stage 2c, set [Fix] VidmemCapMB high): creates up to 12 UPLOAD heaps
// of 32 MB through the hooked CreateHeap, maps a 32 MB buffer in each and checks:
// where the memory went (DXGI usage), CPU write speed, GPU copy speed out of it,
// and that the GPU sees the CPU data (twice: after the first and a second write).
template <class W>
static bool RunProbe(int argc, wchar_t** argv, IDXGIAdapter1* ad, ID3D12Device* dev, ID3D12CommandQueue* q, W& waitIdle)
{
    bool want = false;
    for (int i = 1; i < argc; ++i) want |= _wcsicmp(argv[i], L"probe") == 0;
    if (!want) return false;
    const UINT64 kSz = 32ull << 20;
    const int kN = 12;
    IDXGIAdapter3* a3 = nullptr;
    ad->QueryInterface(IID_PPV_ARGS(&a3));
    ID3D12Resource* def32 = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, kSz, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rb32 = MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, kSz, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tsRb = MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
    ID3D12QueryHeap* qh = nullptr;
    dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh));
    ID3D12CommandAllocator* al = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    UINT64 tsFreq = 1;
    q->GetTimestampFrequency(&tsFreq);
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    if (!a3 || !def32 || !rb32 || !tsRb || !qh || !cl) { printf("probe setup failed\n"); return true; }

    ID3D12Heap* heaps[kN] = {};
    ID3D12Resource* bufs[kN] = {};
    BYTE* maps[kN] = {};
    int n = 0;
    for (; n < kN; ++n) {
        DXGI_QUERY_VIDEO_MEMORY_INFO l0, n0, l1, n1;
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l0);
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n0);
        D3D12_HEAP_DESC hd = {};
        hd.SizeInBytes = kSz; hd.Alignment = 4ull << 20; hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        HRESULT hr = dev->CreateHeap(&hd, IID_PPV_ARGS(&heaps[n]));
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l1);
        a3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n1);
        if (SUCCEEDED(hr)) {
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = kSz; d.Height = 1; d.DepthOrArraySize = 1;
            d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = dev->CreatePlacedResource(heaps[n], 0, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&bufs[n]));
        }
        if (SUCCEEDED(hr)) hr = bufs[n]->Map(0, nullptr, (void**)&maps[n]);
        double wr = 0;
        if (SUCCEEDED(hr)) {
            LARGE_INTEGER t0, t1;
            QueryPerformanceCounter(&t0);
            memset(maps[n], 0x10 + n, (size_t)kSz);
            QueryPerformanceCounter(&t1);
            wr = 32.0 / ((t1.QuadPart - t0.QuadPart) / (double)fq.QuadPart);
        }
        printf("probe heap %2d: VRAM %+.1f MB, sysmem %+.1f MB | create/place/map 0x%08lx | CPU write %6.0f MB/s\n", n + 1,
               ((INT64)l1.CurrentUsage - (INT64)l0.CurrentUsage) / 1048576.0,
               ((INT64)n1.CurrentUsage - (INT64)n0.CurrentUsage) / 1048576.0, hr, wr);
        if (FAILED(hr)) break;
    }

    // GPU side: copy each buffer to DEFAULT (timed), then to READBACK, compare with the pattern.
    auto gpuCheck = [&](int h, BYTE pat, double* ms) -> UINT64 {
        al->Reset();
        cl->Reset(al, nullptr);
        cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        cl->CopyBufferRegion(def32, 0, bufs[h], 0, kSz);
        cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        D3D12_RESOURCE_BARRIER b = Transition(def32, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(rb32, 0, def32, 0, kSz);
        b = Transition(def32, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        cl->ResourceBarrier(1, &b);
        cl->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsRb, 0);
        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        waitIdle();
        UINT64* ts = nullptr;
        tsRb->Map(0, nullptr, (void**)&ts);
        *ms = (ts[1] - ts[0]) * 1000.0 / (double)tsFreq;
        D3D12_RANGE none = { 0, 0 };
        tsRb->Unmap(0, &none);
        const BYTE* m = nullptr;
        rb32->Map(0, nullptr, (void**)&m);
        UINT64 bad = 0;
        for (UINT64 i = 0; i < kSz; ++i) bad += m[i] != pat;
        rb32->Unmap(0, &none);
        return bad;
    };
    int good = 0;
    for (int h = 0; h < n; ++h) {
        double ms1 = 0, ms2 = 0;
        const UINT64 bad1 = gpuCheck(h, (BYTE)(0x10 + h), &ms1);
        memset(maps[h], 0x80 + h, (size_t)kSz); // second write after the GPU has used it
        const UINT64 bad2 = gpuCheck(h, (BYTE)(0x80 + h), &ms2);
        printf("probe heap %2d: GPU copy 32 MB %.3f / %.3f ms (%.1f GB/s) | GPU sees CPU data: %s (bad bytes %llu / %llu)\n",
               h + 1, ms1, ms2, 32.0 / 1024.0 / (ms2 / 1000.0), bad1 + bad2 ? "NO" : "yes", bad1, bad2);
        if (!bad1 && !bad2 && ms2 < 1.0) ++good;
    }
    printf("probe: %d of %d heaps OK and fast (VRAM): %d MB\n", good, n, good * 32);
    for (ID3D12Resource* r : bufs) if (r) r->Release();
    for (ID3D12Heap* h : heaps) if (h) h->Release();
    return true;
}

int wmain(int argc, wchar_t** argv)
{
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(nullptr, dll, MAX_PATH);
    wcscpy_s(wcsrchr(dll, L'\\') + 1, 64, L"version.dll");
    if (!LoadLibraryW(dll)) { printf("cannot load %ls\n", dll); return 1; }
    printf("loaded %ls\n", dll);

    IDXGIFactory6* f = nullptr;
    CHECK(CreateDXGIFactory2(0, IID_PPV_ARGS(&f)));
    IDXGIAdapter1* ad = nullptr;
    CHECK(f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad)));
    DXGI_ADAPTER_DESC1 add = {};
    ad->GetDesc1(&add);
    printf("adapter: %ls\n", add.Description);
    ID3D12Device* dev = nullptr;
    CHECK(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q = nullptr;
    CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));

    HWND hwnd = CreateWindowW(L"STATIC", L"smoke", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr, nullptr, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = 256; sd.Height = 256; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc = nullptr;
    CHECK(f->CreateSwapChainForHwnd(q, hwnd, &sd, nullptr, nullptr, &sc));

    ID3D12Fence* fence = nullptr;
    CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fenceVal = 0;
    auto waitIdle = [&]() {
        q->Signal(fence, ++fenceVal);
        if (fence->GetCompletedValue() < fenceVal) { fence->SetEventOnCompletion(fenceVal, ev); WaitForSingleObject(ev, INFINITE); }
    };

    if (RunProbe(argc, argv, ad, dev, q, waitIdle)) return 0;

    // ======================= Phase 1: bindings without draws =======================
    // Buffers: 4 MB UPLOAD "ring", 1 MB DEFAULT, placed UPLOAD buffer in an UPLOAD heap.
    ID3D12Resource* ring = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, 4 << 20, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* def  = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, 1 << 20, D3D12_RESOURCE_STATE_COMMON);
    ID3D12Resource* tmp  = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, 64 << 10, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!ring || !def || !tmp) { printf("buffer creation failed\n"); return 1; }
    D3D12_HEAP_DESC hd = {}; hd.SizeInBytes = 1 << 20; hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    ID3D12Heap* heap = nullptr;
    CHECK(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap)));
    D3D12_RESOURCE_DESC pd = {};
    pd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; pd.Width = 256 << 10; pd.Height = 1; pd.DepthOrArraySize = 1;
    pd.MipLevels = 1; pd.SampleDesc.Count = 1; pd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* placed = nullptr;
    CHECK(dev->CreatePlacedResource(heap, 64 << 10, &pd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&placed)));

    void* p = nullptr;
    CHECK(ring->Map(0, nullptr, &p));

    // CBV descriptor into the ring + SRV on the placed buffer.
    D3D12_DESCRIPTOR_HEAP_DESC dhd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4 };
    ID3D12DescriptorHeap* dh = nullptr;
    CHECK(dev->CreateDescriptorHeap(&dhd, IID_PPV_ARGS(&dh)));
    D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = { ring->GetGPUVirtualAddress() + 512, 256 };
    dev->CreateConstantBufferView(&cbv, dh->GetCPUDescriptorHandleForHeapStart());
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R32_TYPELESS; srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Buffer.NumElements = 1024; srv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    D3D12_CPU_DESCRIPTOR_HANDLE h1 = dh->GetCPUDescriptorHandleForHeapStart();
    h1.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    dev->CreateShaderResourceView(placed, &srv, h1);

    // Root signature: root CBV b0 + root SRV t0.
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
    D3D12_ROOT_SIGNATURE_DESC rsd = { 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3DBlob* blob = nullptr;
    CHECK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, nullptr));
    ID3D12RootSignature* rs = nullptr;
    CHECK(dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs)));

    ID3D12CommandAllocator* alloc = nullptr;
    CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
    ID3D12GraphicsCommandList* cl = nullptr;
    CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
    cl->Close();

    const int frames = argc > 1 ? _wtoi(argv[1]) : 200;
    for (int i = 0; i < frames; ++i) {
        CHECK(alloc->Reset());
        CHECK(cl->Reset(alloc, nullptr));
        cl->SetGraphicsRootSignature(rs);
        cl->SetComputeRootSignature(rs);
        for (int k = 0; k < 100; ++k) {   // 100 "draws" worth of ring-buffer constants
            cl->SetGraphicsRootConstantBufferView(0, ring->GetGPUVirtualAddress() + (k * 256) % (4 << 20));
            cl->SetGraphicsRootShaderResourceView(1, def->GetGPUVirtualAddress());
        }
        for (int k = 0; k < 10; ++k) cl->SetComputeRootConstantBufferView(0, ring->GetGPUVirtualAddress() + 4096);
        D3D12_VERTEX_BUFFER_VIEW vb[2] = { { ring->GetGPUVirtualAddress() + 65536, 1024, 16 },
                                           { def->GetGPUVirtualAddress(), 1024, 16 } };
        cl->IASetVertexBuffers(0, 2, vb);
        D3D12_INDEX_BUFFER_VIEW ib = { placed->GetGPUVirtualAddress(), 1024, DXGI_FORMAT_R16_UINT };
        cl->IASetIndexBuffer(&ib);
        cl->CopyBufferRegion(def, 0, ring, 0, 4096);
        cl->SetGraphicsRootConstantBufferView(0, 0x1000); // bogus VA -> "untracked"
        CHECK(cl->Close());
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        CHECK(sc->Present(1, 0));
        waitIdle();
        if (i == frames / 2) { tmp->Release(); tmp = nullptr; printf("released 64 KB upload buffer\n"); }
    }
    printf("phase 1: %d frames done\n", frames);

    // ======================= Phase 2: draws reading root CBVs =======================
    ID3DBlob* vs = Compile("VSMain", "vs_5_0");
    ID3DBlob* ps = Compile("PSMain", "ps_5_0");
    ID3DBlob* cs = Compile("CSMain", "cs_5_0");
    if (!vs || !ps || !cs) return 1;

    // Root signature: [0] root CBV b0, [1] root UAV u0.
    D3D12_ROOT_PARAMETER rp2[2] = {};
    rp2[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rp2[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    D3D12_ROOT_SIGNATURE_DESC rsd2 = { 2, rp2, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3DBlob* blob2 = nullptr;
    CHECK(D3D12SerializeRootSignature(&rsd2, D3D_ROOT_SIGNATURE_VERSION_1, &blob2, nullptr));
    ID3D12RootSignature* rs2 = nullptr;
    CHECK(dev->CreateRootSignature(0, blob2->GetBufferPointer(), blob2->GetBufferSize(), IID_PPV_ARGS(&rs2)));

    D3D12_GRAPHICS_PIPELINE_STATE_DESC gp = {};
    gp.pRootSignature = rs2;
    gp.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
    gp.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
    gp.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    gp.SampleMask = UINT_MAX;
    gp.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    gp.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    gp.RasterizerState.DepthClipEnable = TRUE;
    gp.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    gp.NumRenderTargets = 1;
    gp.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
    gp.SampleDesc.Count = 1;
    ID3D12PipelineState* gfxPso = nullptr;
    CHECK(dev->CreateGraphicsPipelineState(&gp, IID_PPV_ARGS(&gfxPso)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC cp = {};
    cp.pRootSignature = rs2;
    cp.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
    ID3D12PipelineState* csPso = nullptr;
    CHECK(dev->CreateComputePipelineState(&cp, IID_PPV_ARGS(&csPso)));

    // Render target kW x kH R32_UINT, UAV buffer, readback buffer.
    D3D12_HEAP_PROPERTIES defHp = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rtd = {};
    rtd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; rtd.Width = kW; rtd.Height = kH; rtd.DepthOrArraySize = 1;
    rtd.MipLevels = 1; rtd.Format = DXGI_FORMAT_R32_UINT; rtd.SampleDesc.Count = 1;
    rtd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ID3D12Resource* rt = nullptr;
    CHECK(dev->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &rtd, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&rt)));
    ID3D12Resource* uav = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, kDisp * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                     D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT64 rtBytes = 0;
    dev->GetCopyableFootprints(&rtd, 0, 1, 0, &fp, nullptr, nullptr, &rtBytes);
    const UINT64 uavOff = (rtBytes + 511) & ~511ull;
    ID3D12Resource* rb = MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, uavOff + kDisp * 4, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!uav || !rb) { printf("phase 2 buffer creation failed\n"); return 1; }

    D3D12_DESCRIPTOR_HEAP_DESC rhd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    CHECK(dev->CreateDescriptorHeap(&rhd, IID_PPV_ARGS(&rtvHeap)));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    dev->CreateRenderTargetView(rt, nullptr, rtv);

    // Two constant pages (4 MB UPLOAD committed, like the game's).
    ID3D12Resource* pageA = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, kPage, D3D12_RESOURCE_STATE_GENERIC_READ);
    ID3D12Resource* pageB = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, kPage, D3D12_RESOURCE_STATE_GENERIC_READ);
    if (!pageA || !pageB) { printf("page creation failed\n"); return 1; }
    BYTE *pA = nullptr, *pB = nullptr;
    CHECK(pageA->Map(0, nullptr, (void**)&pA));
    CHECK(pageB->Map(0, nullptr, (void**)&pB));

    // Three lists: L[0] draws 0..511, L[1] draws 512..1023 (submitted together),
    // L[2] dispatches + readback copies (second submission).
    ID3D12CommandAllocator* al[3] = {};
    ID3D12GraphicsCommandList* L[3] = {};
    for (int i = 0; i < 3; ++i) {
        CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al[i])));
        CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[i], nullptr, IID_PPV_ARGS(&L[i])));
        CHECK(L[i]->Close());
    }

    const int frames2 = argc > 2 ? _wtoi(argv[2]) : 60;
    int failures = 0, checks = 0, reexecs = 0;
    for (int fr = 0; fr < frames2; ++fr) {
        if (fr == frames2 / 2) {
            // Recreate page B (GPU is idle): the old twin must be retired, a new one created.
            pageB->Unmap(0, nullptr);
            pageB->Release();
            pageB = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, kPage, D3D12_RESOURCE_STATE_GENERIC_READ);
            if (!pageB) { printf("page B re-creation failed\n"); return 1; }
            CHECK(pageB->Map(0, nullptr, (void**)&pB));
            printf("recreated page B\n");
        }
        const UINT64 base = (fr % kRingSlots) * kFrameStride;
        const D3D12_GPU_VIRTUAL_ADDRESS vaA = pageA->GetGPUVirtualAddress(), vaB = pageB->GetGPUVirtualAddress();
        WriteConstants(pA, pB, base, (UINT)fr * 2 + 1);

        for (int li = 0; li < 2; ++li) {
            ID3D12GraphicsCommandList* c = L[li];
            CHECK(al[li]->Reset());
            CHECK(c->Reset(al[li], gfxPso));
            c->SetGraphicsRootSignature(rs2);
            c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_RECT sr = { 0, 0, (LONG)kW, (LONG)kH };
            c->RSSetScissorRects(1, &sr);
            c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT k = li * kDraws / 2; k < (li + 1) * kDraws / 2; ++k) {
                const D3D12_VIEWPORT vp = { (float)(k % kW), (float)(k / kW), 1.0f, 1.0f, 0.0f, 1.0f };
                c->RSSetViewports(1, &vp);
                c->SetGraphicsRootConstantBufferView(0, (k & 1 ? vaB : vaA) + DrawOff(base, k));
                c->DrawInstanced(3, 1, 0, 0);
            }
            CHECK(c->Close());
        }
        {
            ID3D12GraphicsCommandList* c = L[2];
            CHECK(al[2]->Reset());
            CHECK(c->Reset(al[2], csPso));
            c->SetComputeRootSignature(rs2);
            c->SetComputeRootUnorderedAccessView(1, uav->GetGPUVirtualAddress());
            for (UINT j = 0; j < kDisp; ++j) {
                c->SetComputeRootConstantBufferView(0, vaA + DispOff(base, j));
                c->Dispatch(1, 1, 1);
            }
            D3D12_RESOURCE_BARRIER b[2] = {
                Transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                Transition(uav, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
            c->ResourceBarrier(2, b);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
            src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            c->CopyBufferRegion(rb, uavOff, uav, 0, kDisp * 4);
            D3D12_RESOURCE_BARRIER b2[2] = {
                Transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET),
                Transition(uav, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
            c->ResourceBarrier(2, b2);
            CHECK(c->Close());
        }

        // Every 3rd frame also re-execute the same lists (no Reset) after rewriting
        // the constants in place: the copy must run again on the second submission.
        const int passes = fr % 3 == 2 ? 2 : 1;
        for (int pass = 0; pass < passes; ++pass) {
            const UINT seed = (UINT)fr * 2 + 1 + pass * 1000003u;
            if (pass) { WriteConstants(pA, pB, base, seed); ++reexecs; }
            ID3D12CommandList* sub1[] = { L[0], L[1] };
            q->ExecuteCommandLists(2, sub1);
            ID3D12CommandList* sub2[] = { L[2] };
            q->ExecuteCommandLists(1, sub2);
            if (!pass) CHECK(sc->Present(0, 0));
            waitIdle();

            BYTE* m = nullptr;
            CHECK(rb->Map(0, nullptr, (void**)&m));
            int bad = 0;
            for (UINT k = 0; k < kDraws; ++k) {
                const UINT got = *(const UINT*)(m + fp.Offset + (k / kW) * fp.Footprint.RowPitch + (k % kW) * 4);
                const UINT want = Val(seed, k);
                if (got != want && bad++ < 4)
                    printf("  frame %d pass %d: draw %u got 0x%08x want 0x%08x\n", fr, pass, k, got, want);
            }
            for (UINT j = 0; j < kDisp; ++j) {
                const UINT got = *(const UINT*)(m + uavOff + j * 4);
                const UINT want = Val(seed, kDraws + j);
                if (got != want && bad++ < 4)
                    printf("  frame %d pass %d: dispatch %u got 0x%08x want 0x%08x\n", fr, pass, j, got, want);
            }
            D3D12_RANGE none = { 0, 0 };
            rb->Unmap(0, &none);
            ++checks;
            if (bad) { ++failures; printf("frame %d pass %d: %d mismatches\n", fr, pass, bad); }
        }
    }
    printf("phase 2: %d frames, %d checks (%d re-executions), %d failed\n", frames2, checks, reexecs, failures);
    if (dev->GetDeviceRemovedReason() != S_OK) { printf("DEVICE REMOVED 0x%08lx\n", dev->GetDeviceRemovedReason()); return 1; }

    // ============ Phase 3: per-frame VB/SRV buffers placed in an UPLOAD heap ============
    // Like the game: one 32 MB UPLOAD heap (flags 0, 4 MB alignment); every frame
    // new small buffers are placed in it (ring), written once and used both as a
    // vertex buffer and as an SRV, then released. With [Fix] VidmemHeaps=1 the
    // heap lives in CPU-visible VRAM. Benchmark: one 8 MB vertex fetch from that
    // heap vs from a CUSTOM (L0, write-combine) heap = plain system memory that
    // is never redirected; plus CPU write/read speed into both.
    bool breakIt = false; // "break": skip the VB write of one buffer -> the check must fail
    for (int i = 1; i < argc; ++i) breakIt |= _wcsicmp(argv[i], L"break") == 0;
    const int frames3 = argc > 3 && _wtoi(argv[3]) > 0 ? _wtoi(argv[3]) : 60;
    {
        ID3DBlob* vsVb = Compile("VSVb", "vs_5_0");
        ID3DBlob* psVb = Compile("PSVb", "ps_5_0");
        ID3DBlob* vsBench = Compile("VSBench", "vs_5_0");
        if (!vsVb || !psVb || !vsBench) return 1;

        D3D12_ROOT_PARAMETER rp3[1] = {};
        rp3[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        D3D12_ROOT_SIGNATURE_DESC rsd3 = { 1, rp3, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ID3DBlob* blob3 = nullptr;
        CHECK(D3D12SerializeRootSignature(&rsd3, D3D_ROOT_SIGNATURE_VERSION_1, &blob3, nullptr));
        ID3D12RootSignature* rs3 = nullptr;
        CHECK(dev->CreateRootSignature(0, blob3->GetBufferPointer(), blob3->GetBufferSize(), IID_PPV_ARGS(&rs3)));

        D3D12_INPUT_ELEMENT_DESC ilVb[] = { { "VAL", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gv = gp;
        gv.pRootSignature = rs3;
        gv.VS = { vsVb->GetBufferPointer(), vsVb->GetBufferSize() };
        gv.PS = { psVb->GetBufferPointer(), psVb->GetBufferSize() };
        gv.InputLayout = { ilVb, 1 };
        ID3D12PipelineState* vbPso = nullptr;
        CHECK(dev->CreateGraphicsPipelineState(&gv, IID_PPV_ARGS(&vbPso)));

        D3D12_INPUT_ELEMENT_DESC ilB[] = { { "POS", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC gb = gp;
        gb.pRootSignature = rs3;
        gb.VS = { vsBench->GetBufferPointer(), vsBench->GetBufferSize() };
        gb.PS = {};
        gb.InputLayout = { ilB, 1 };
        gb.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        gb.NumRenderTargets = 0;
        gb.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        ID3D12PipelineState* benchPso = nullptr;
        CHECK(dev->CreateGraphicsPipelineState(&gb, IID_PPV_ARGS(&benchPso)));

        D3D12_HEAP_DESC uhd = {};
        uhd.SizeInBytes = 32ull << 20; uhd.Alignment = 4ull << 20;
        uhd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD; uhd.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap* upHeap = nullptr;
        CHECK(dev->CreateHeap(&uhd, IID_PPV_ARGS(&upHeap)));
        D3D12_HEAP_DESC shd = {};
        shd.SizeInBytes = 8ull << 20;
        shd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
        shd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
        shd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
        shd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
        ID3D12Heap* sysHeap = nullptr;
        CHECK(dev->CreateHeap(&shd, IID_PPV_ARGS(&sysHeap)));

        auto placeBuf = [&](ID3D12Heap* h, UINT64 off, UINT64 size) -> ID3D12Resource* {
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
            d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            ID3D12Resource* r = nullptr;
            HRESULT hr = dev->CreatePlacedResource(h, off, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr)) printf("CreatePlacedResource(off %llu, size %llu) failed 0x%08lx\n", off, size, hr);
            return r;
        };

        // ---- correctness: per-frame churned buffers used as VB + SRV ----
        const UINT kDyn = 256, kBufs = 16;                // draws (pixels) and buffers per frame
        const UINT64 kBufSize = 64 << 10, kSrvOff = 32 << 10, kSlot = 256;
        const UINT64 kRingBytes = 24ull << 20;            // [0, 24 MB); the benchmark buffer sits above
        UINT64 ringOff = 0;
        int fail3 = 0;
        for (int fr = 0; fr < frames3; ++fr) {
            if (ringOff + kBufs * kBufSize > kRingBytes) ringOff = 0;
            const UINT seed = 0x1234567u + (UINT)fr * 7919u;
            ID3D12Resource* bufs[kBufs] = {};
            BYTE* mp[kBufs] = {};
            for (UINT b = 0; b < kBufs; ++b) {
                bufs[b] = placeBuf(upHeap, ringOff + b * kBufSize, kBufSize);
                if (!bufs[b]) return 1;
                CHECK(bufs[b]->Map(0, nullptr, (void**)&mp[b]));
            }
            for (UINT k = 0; k < kDyn; ++k) {
                const UINT b = k % kBufs, s = k / kBufs;
                if (!(breakIt && fr == frames3 / 2 && b == 0)) {
                    UINT* v = (UINT*)(mp[b] + s * kSlot);
                    v[0] = v[1] = v[2] = Val(seed, k);
                }
                *(UINT*)(mp[b] + kSrvOff + s * kSlot) = SrvVal(seed, k);
            }
            for (UINT b = 0; b < kBufs; ++b) bufs[b]->Unmap(0, nullptr);

            ID3D12GraphicsCommandList* c = L[0];
            CHECK(al[0]->Reset());
            CHECK(c->Reset(al[0], vbPso));
            c->SetGraphicsRootSignature(rs3);
            c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
            const D3D12_RECT sr = { 0, 0, (LONG)kW, (LONG)kH };
            c->RSSetScissorRects(1, &sr);
            c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            for (UINT k = 0; k < kDyn; ++k) {
                const UINT b = k % kBufs, s = k / kBufs;
                const D3D12_GPU_VIRTUAL_ADDRESS va = bufs[b]->GetGPUVirtualAddress();
                const D3D12_VIEWPORT vp = { (float)(k % kW), (float)(k / kW), 1.0f, 1.0f, 0.0f, 1.0f };
                c->RSSetViewports(1, &vp);
                const D3D12_VERTEX_BUFFER_VIEW vbv = { va + s * kSlot, 12, 4 };
                c->IASetVertexBuffers(0, 1, &vbv);
                c->SetGraphicsRootShaderResourceView(0, va + kSrvOff + s * kSlot);
                c->DrawInstanced(3, 1, 0, 0);
            }
            D3D12_RESOURCE_BARRIER b1 = Transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            c->ResourceBarrier(1, &b1);
            D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
            dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
            src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            D3D12_RESOURCE_BARRIER b2 = Transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            c->ResourceBarrier(1, &b2);
            CHECK(c->Close());
            ID3D12CommandList* sub[] = { c };
            q->ExecuteCommandLists(1, sub);
            CHECK(sc->Present(0, 0));
            waitIdle();

            BYTE* m = nullptr;
            CHECK(rb->Map(0, nullptr, (void**)&m));
            int bad = 0;
            for (UINT k = 0; k < kDyn; ++k) {
                const UINT got = *(const UINT*)(m + fp.Offset + (k / kW) * fp.Footprint.RowPitch + (k % kW) * 4);
                const UINT want = Val(seed, k) ^ SrvVal(seed, k);
                if (got != want && bad++ < 4) printf("  phase 3 frame %d: draw %u got 0x%08x want 0x%08x\n", fr, k, got, want);
            }
            D3D12_RANGE none = { 0, 0 };
            rb->Unmap(0, &none);
            if (bad) { ++fail3; printf("phase 3 frame %d: %d mismatches\n", fr, bad); }
            for (UINT b = 0; b < kBufs; ++b) bufs[b]->Release();
            ringOff += kBufs * kBufSize;
        }
        failures += fail3;
        printf("phase 3: %d frames x %u placed VB/SRV buffers, %d failed%s\n", frames3, kBufs, fail3,
               breakIt ? " (BREAK mode: failures expected)" : "");

        // ---- benchmark: 8 MB vertex fetch, CPU write/read speed ----
        const UINT kBenchVerts = 512 * 1024;
        const UINT64 kBenchBytes = kBenchVerts * 16ull;
        ID3D12Resource* benchRes[2] = { placeBuf(upHeap, 24ull << 20, kBenchBytes), placeBuf(sysHeap, 0, kBenchBytes) };
        if (!benchRes[0] || !benchRes[1]) return 1;
        const char* const benchName[2] = { "UPLOAD heap (redirect target)", "CUSTOM L0 heap (sysmem reference)" };
        float* srcData = (float*)VirtualAlloc(nullptr, kBenchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        for (UINT64 i = 0; i < kBenchBytes / 4; ++i) srcData[i] = (float)(i & 1023) * 0.001f;

        D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
        ID3D12QueryHeap* qh = nullptr;
        CHECK(dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh)));
        ID3D12Resource* tsRb = MakeBuffer(dev, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
        UINT64 tsFreq = 1;
        CHECK(q->GetTimestampFrequency(&tsFreq));
        LARGE_INTEGER qpf; QueryPerformanceFrequency(&qpf);

        const int kIters = 8;
        double gpuMin[2] = { 1e9, 1e9 }, gpuSum[2] = {}, wrBest[2] = {}, rdMBs[2] = {};
        for (int it = 0; it < kIters; ++it) {
            for (int h = 0; h < 2; ++h) {
                void* mapped = nullptr;
                CHECK(benchRes[h]->Map(0, nullptr, &mapped));
                LARGE_INTEGER t0, t1;
                QueryPerformanceCounter(&t0);
                memcpy(mapped, srcData, (size_t)kBenchBytes);
                QueryPerformanceCounter(&t1);
                const double wr = kBenchBytes / 1048576.0 / ((t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart);
                if (wr > wrBest[h]) wrBest[h] = wr;
                if (it == 0) { // CPU read of 256 KB (informational: games should never do this)
                    volatile UINT64 sink = 0;
                    const UINT64* p64 = (const UINT64*)mapped;
                    QueryPerformanceCounter(&t0);
                    for (UINT i = 0; i < (256u << 10) / 8; ++i) sink += p64[i];
                    QueryPerformanceCounter(&t1);
                    rdMBs[h] = 0.25 / ((t1.QuadPart - t0.QuadPart) / (double)qpf.QuadPart);
                }
                benchRes[h]->Unmap(0, nullptr);

                ID3D12GraphicsCommandList* c = L[0];
                CHECK(al[0]->Reset());
                CHECK(c->Reset(al[0], benchPso));
                c->SetGraphicsRootSignature(rs3);
                c->SetGraphicsRootShaderResourceView(0, benchRes[h]->GetGPUVirtualAddress());
                const D3D12_VIEWPORT vp = { 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f };
                c->RSSetViewports(1, &vp);
                const D3D12_RECT sr = { 0, 0, 1, 1 };
                c->RSSetScissorRects(1, &sr);
                c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_POINTLIST);
                const D3D12_VERTEX_BUFFER_VIEW vbv = { benchRes[h]->GetGPUVirtualAddress(), (UINT)kBenchBytes, 16 };
                c->IASetVertexBuffers(0, 1, &vbv);
                c->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
                c->DrawInstanced(kBenchVerts, 1, 0, 0);
                c->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
                c->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsRb, 0);
                CHECK(c->Close());
                ID3D12CommandList* sub[] = { c };
                q->ExecuteCommandLists(1, sub);
                waitIdle();
                UINT64* ts = nullptr;
                CHECK(tsRb->Map(0, nullptr, (void**)&ts));
                const double ms = (ts[1] - ts[0]) * 1000.0 / (double)tsFreq;
                D3D12_RANGE none = { 0, 0 };
                tsRb->Unmap(0, &none);
                gpuSum[h] += ms;
                if (ms < gpuMin[h]) gpuMin[h] = ms;
            }
        }
        for (int h = 0; h < 2; ++h)
            printf("bench %-34s: GPU fetch of 8 MB VB min %.3f ms avg %.3f ms (%.0f MB/s) | CPU write %.0f MB/s, CPU read %.1f MB/s\n",
                   benchName[h], gpuMin[h], gpuSum[h] / kIters, 8.0 / (gpuMin[h] / 1000.0), wrBest[h], rdMBs[h]);
        VirtualFree(srcData, 0, MEM_RELEASE);
        benchRes[0]->Release(); benchRes[1]->Release();
        // Release the heaps (GPU is idle): stage 2d must retire upHeap's mirror during phase 4 (see the log).
        upHeap->Release(); sysHeap->Release();
    }
    if (dev->GetDeviceRemovedReason() != S_OK) { printf("DEVICE REMOVED 0x%08lx\n", dev->GetDeviceRemovedReason()); return 1; }

    // ============ Phase 4: stage 2d, SRVs through descriptor tables ============
    const int frames4 = argc > 4 && _wtoi(argv[4]) > 0 ? _wtoi(argv[4]) : 60;
    {
        // What the DLL is configured to do (it reads the same ini): which failures are expected.
        wchar_t ini[MAX_PATH];
        GetModuleFileNameW(nullptr, ini, MAX_PATH);
        wcscpy_s(wcsrchr(ini, L'\\') + 1, 64, L"egpu_fix.ini");
        auto iniInt = [&](const wchar_t* sec, const wchar_t* key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, ini); };
        const bool hooksOn = iniInt(L"General", L"Hooks", 1) != 0;
        const bool measure = iniInt(L"General", L"Measure", 0) != 0;
        // The smoke heaps fit into the CPU-visible VRAM window, so with VidmemHeaps=1 stage 2c takes them all.
        const bool hyb = hooksOn && iniInt(L"Fix", L"HybridHeaps", 0) && !iniInt(L"Fix", L"VidmemHeaps", 1);
        const bool hybSrv = hyb && iniInt(L"Fix", L"HybridSrv", 0);
        const bool hybCheck = hyb && measure && iniInt(L"Fix", L"HybridCheck", 0);
        const bool noCopy = hyb && iniInt(L"Fix", L"HybridTestNoCopy", 0);
        const bool expectPStale = hybSrv && !hybCheck && !noCopy;
        printf("phase 4: ini says hybrid %d, SRV %d, check %d, no-copy test %d -> persistent buffer %s\n",
               hyb, hybSrv, hybCheck, noCopy, expectPStale ? "MUST read stale data" : "must read fresh data");

        ID3DBlob* vsD = Compile("VSDesc", "vs_5_1", kHlsl4);
        ID3DBlob* psD = Compile("PSVb", "ps_5_1", kHlsl4);
        if (!vsD || !psD) return 1;
        D3D12_DESCRIPTOR_RANGE range = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 1, 0 };
        D3D12_ROOT_PARAMETER rp4 = {};
        rp4.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rp4.DescriptorTable = { 1, &range };
        D3D12_ROOT_SIGNATURE_DESC rsd4 = { 1, &rp4, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
        ID3DBlob* blob4 = nullptr;
        CHECK(D3D12SerializeRootSignature(&rsd4, D3D_ROOT_SIGNATURE_VERSION_1, &blob4, nullptr));
        ID3D12RootSignature* rs4 = nullptr;
        CHECK(dev->CreateRootSignature(0, blob4->GetBufferPointer(), blob4->GetBufferSize(), IID_PPV_ARGS(&rs4)));
        D3D12_INPUT_ELEMENT_DESC il[] = { { "VAL", 0, DXGI_FORMAT_R32_UINT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 } };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC g4 = gp;
        g4.pRootSignature = rs4;
        g4.VS = { vsD->GetBufferPointer(), vsD->GetBufferSize() };
        g4.PS = { psD->GetBufferPointer(), psD->GetBufferSize() };
        g4.InputLayout = { il, 1 };
        ID3D12PipelineState* pso4 = nullptr;
        CHECK(dev->CreateGraphicsPipelineState(&g4, IID_PPV_ARGS(&pso4)));

        // Layout of each 64 KB per-frame buffer; slot s = draw k / kB4.
        const UINT kDyn4 = 256, kB4 = 16, kP4 = 32;              // main draws, buffers per frame, persistent draws
        const UINT64 kBuf4 = 64 << 10;
        const UINT64 kRawOff = 16 << 10, kStOff = 12 * 2731, kTyOff = 48 << 10, kPvbOff = 60 << 10;
        const UINT64 kRing4 = 24ull << 20, kPOff = 28ull << 20;  // ring [0, 24 MB), persistent buffer at 28 MB
        auto rawVal = [](UINT seed, UINT k) { return Val(seed ^ 0x1111u, k); };
        auto stVal = [](UINT seed, UINT k) { return Val(seed ^ 0x2222u, k); };
        auto tyVal = [](UINT seed, UINT k) { return Val(seed ^ 0x3333u, k); };
        auto pvbVal = [](UINT seed, UINT j) { return Val(seed ^ 0x4444u, j); };
        auto pVal = [](int fr) { return Val(0xabcdefu, (UINT)fr + 1); };

        D3D12_HEAP_DESC hd4 = {};
        hd4.SizeInBytes = 32ull << 20; hd4.Alignment = 4ull << 20;
        hd4.Properties.Type = D3D12_HEAP_TYPE_UPLOAD; hd4.Flags = D3D12_HEAP_FLAG_NONE;
        ID3D12Heap* heapA = nullptr;
        ID3D12Heap* heapB = nullptr; // second heap, from the middle of the phase
        CHECK(dev->CreateHeap(&hd4, IID_PPV_ARGS(&heapA)));
        bool tight = false; // tight-aligned buffers (Agility runtime): 4 KB offsets like the game's
        auto place4 = [&](ID3D12Heap* h, UINT64 off, UINT64 size) -> ID3D12Resource* {
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = 1; d.DepthOrArraySize = 1;
            d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (tight) d.Flags = (D3D12_RESOURCE_FLAGS)0x400; // D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT
            ID3D12Resource* r = nullptr;
            HRESULT hr = dev->CreatePlacedResource(h, off, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
            if (FAILED(hr)) printf("phase 4: CreatePlacedResource(off %llu) failed 0x%08lx\n", off, hr);
            return r;
        };
        {
            D3D12_RESOURCE_DESC d = {};
            d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = 608; d.Height = 1; d.DepthOrArraySize = 1;
            d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            d.Flags = (D3D12_RESOURCE_FLAGS)0x400;
            ID3D12Resource* r = nullptr;
            tight = SUCCEEDED(dev->CreatePlacedResource(heapA, 31ull << 20, &d, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                        nullptr, IID_PPV_ARGS(&r))) && r;
            if (r) r->Release();
            printf("phase 4: tight-aligned buffers %s\n", tight ? "ON (Agility runtime), 4 KB offsets"
                                                              : "not supported (no build\\D3D12\\D3D12Core.dll), 64 KB offsets");
        }
        // Buffer b of a frame: ring + b * stride (+ 4 KB for odd b with tight alignment).
        const UINT64 kStride4 = tight ? 72 << 10 : 64 << 10;
        auto bufOff = [&](UINT64 ring, UINT b) { return ring + b * kStride4 + (tight && (b & 1) ? 0x1000 : 0); };

        // Descriptors: CPU heap (created there) -> shader-visible heap (copied), like an engine does.
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const UINT nDesc = kDyn4 * 4 + 4;                         // a table per main draw + one table for the P draws
        D3D12_DESCRIPTOR_HEAP_DESC cpuHd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, nDesc + 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        D3D12_DESCRIPTOR_HEAP_DESC gpuHd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, nDesc, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        ID3D12DescriptorHeap* cpuHeap = nullptr;
        ID3D12DescriptorHeap* gpuHeap = nullptr;
        CHECK(dev->CreateDescriptorHeap(&cpuHd, IID_PPV_ARGS(&cpuHeap)));
        CHECK(dev->CreateDescriptorHeap(&gpuHd, IID_PPV_ARGS(&gpuHeap)));
        auto cpuH = [&](UINT i) { D3D12_CPU_DESCRIPTOR_HANDLE h = cpuHeap->GetCPUDescriptorHandleForHeapStart(); h.ptr += (SIZE_T)i * inc; return h; };
        auto gpuH = [&](UINT i) { D3D12_GPU_DESCRIPTOR_HANDLE h = gpuHeap->GetGPUDescriptorHandleForHeapStart(); h.ptr += (UINT64)i * inc; return h; };
        auto rawDesc = [](UINT64 byteOff, UINT n) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
            d.Format = DXGI_FORMAT_R32_TYPELESS; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = byteOff / 4; d.Buffer.NumElements = n; d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            return d;
        };
        auto stDesc = [](UINT64 first) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
            d.Format = DXGI_FORMAT_UNKNOWN; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = first; d.Buffer.NumElements = 1; d.Buffer.StructureByteStride = 12;
            return d;
        };
        auto tyDesc = [](UINT64 first) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d = {};
            d.Format = DXGI_FORMAT_R32_UINT; d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = first; d.Buffer.NumElements = 1;
            return d;
        };
        // Null descriptors fill the unused slots of a table (they read 0).
        auto nullSrvs = [&](UINT base, bool raw, bool st, bool ty, bool p) {
            D3D12_SHADER_RESOURCE_VIEW_DESC d;
            if (raw) { d = rawDesc(0, 4); dev->CreateShaderResourceView(nullptr, &d, cpuH(base + 0)); }
            if (st)  { d = stDesc(0);     dev->CreateShaderResourceView(nullptr, &d, cpuH(base + 1)); }
            if (ty)  { d = tyDesc(0);     dev->CreateShaderResourceView(nullptr, &d, cpuH(base + 2)); }
            if (p)   { d = rawDesc(0, 4); dev->CreateShaderResourceView(nullptr, &d, cpuH(base + 3)); }
        };

        // Persistent buffer P: SRV created once (its own CPU slot nDesc), rewritten in place every frame.
        ID3D12Resource* P = place4(heapA, kPOff, kBuf4);
        if (!P) return 1;
        BYTE* pMap = nullptr;
        CHECK(P->Map(0, nullptr, (void**)&pMap));
        *(UINT*)pMap = pVal(-1);
        { D3D12_SHADER_RESOURCE_VIEW_DESC d = rawDesc(0, 4); dev->CreateShaderResourceView(P, &d, cpuH(nDesc)); }

        UINT64 ringOff = 0;
        int mainBad = 0, pStale = 0, pWrongOther = 0;
        for (int fr = 0; fr < frames4; ++fr) {
            if (fr == frames4 / 2) { CHECK(dev->CreateHeap(&hd4, IID_PPV_ARGS(&heapB))); ringOff = 0; printf("phase 4: second heap\n"); }
            ID3D12Heap* heap = heapB ? heapB : heapA;
            if (ringOff + kB4 * kStride4 + 0x1000 > kRing4) ringOff = 0;
            const UINT seed = 0x7654321u + (UINT)fr * 104729u;
            ID3D12Resource* bufs[kB4] = {};
            BYTE* mp[kB4] = {};
            for (UINT b = 0; b < kB4; ++b) {
                const UINT64 off = bufOff(ringOff, b);
                if (b == 2) {
                    // A buffer with an SRV that dies, then another one at the same offset.
                    ID3D12Resource* decoy = place4(heap, off, kBuf4);
                    if (!decoy) return 1;
                    D3D12_SHADER_RESOURCE_VIEW_DESC d = stDesc(1);
                    dev->CreateShaderResourceView(decoy, &d, cpuH(nDesc - 1)); // P table slot, overwritten below
                    decoy->Release();
                }
                bufs[b] = place4(heap, off, kBuf4);
                if (!bufs[b]) return 1;
                CHECK(bufs[b]->Map(0, nullptr, (void**)&mp[b]));
            }
            auto writeDraw = [&](UINT k) {
                const UINT b = k % kB4, s = k / kB4;
                UINT* v = (UINT*)(mp[b] + s * 256);
                v[0] = v[1] = v[2] = Val(seed, k);
                *(UINT*)(mp[b] + kRawOff + s * 256) = rawVal(seed, k);
                *(UINT*)(mp[b] + kStOff + 24 * s + 4) = stVal(seed, k);
                *(UINT*)(mp[b] + kTyOff + s * 256) = tyVal(seed, k);
            };
            // First half of the data now, then all SRVs, the second half only after the first submission.
            for (UINT k = 0; k < kDyn4 / 2; ++k) writeDraw(k);
            for (UINT j = 0; j < kP4; ++j) {
                UINT* v = (UINT*)(mp[j % kB4] + kPvbOff + (j / kB4) * 256);
                v[0] = v[1] = v[2] = pvbVal(seed, j);
            }
            for (UINT k = 0; k < kDyn4; ++k) {
                const UINT b = k % kB4, s = k / kB4;
                D3D12_SHADER_RESOURCE_VIEW_DESC d = rawDesc(kRawOff + s * 256, 4);
                dev->CreateShaderResourceView(bufs[b], &d, cpuH(k * 4 + 0));
                d = stDesc((kStOff + 24 * s) / 12);
                dev->CreateShaderResourceView(bufs[b], &d, cpuH(k * 4 + 1));
                d = tyDesc((kTyOff + s * 256) / 4);
                dev->CreateShaderResourceView(bufs[b], &d, cpuH(k * 4 + 2));
                nullSrvs(k * 4, false, false, false, true);
            }
            nullSrvs(kDyn4 * 4, true, true, true, false);
            dev->CopyDescriptorsSimple(1, cpuH(kDyn4 * 4 + 3), cpuH(nDesc), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            {
                D3D12_CPU_DESCRIPTOR_HANDLE dst = gpuHeap->GetCPUDescriptorHandleForHeapStart();
                dev->CopyDescriptorsSimple(nDesc, dst, cpuH(0), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }

            auto record = [&](ID3D12GraphicsCommandList* c, ID3D12CommandAllocator* a, UINT k0, UINT k1, bool last) -> HRESULT {
                HRESULT hr = a->Reset();
                if (SUCCEEDED(hr)) hr = c->Reset(a, pso4);
                if (FAILED(hr)) return hr;
                c->SetGraphicsRootSignature(rs4);
                c->SetDescriptorHeaps(1, &gpuHeap);
                c->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                const D3D12_RECT sr = { 0, 0, (LONG)kW, (LONG)kH };
                c->RSSetScissorRects(1, &sr);
                c->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                for (UINT k = k0; k < k1; ++k) {
                    const UINT b = k % kB4, s = k / kB4;
                    const D3D12_VIEWPORT vp = { (float)(k % kW), (float)(k / kW), 1.0f, 1.0f, 0.0f, 1.0f };
                    c->RSSetViewports(1, &vp);
                    const D3D12_VERTEX_BUFFER_VIEW vbv = { bufs[b]->GetGPUVirtualAddress() + s * 256, 12, 4 };
                    c->IASetVertexBuffers(0, 1, &vbv);
                    c->SetGraphicsRootDescriptorTable(0, gpuH(k * 4));
                    c->DrawInstanced(3, 1, 0, 0);
                }
                if (last) {
                    c->SetGraphicsRootDescriptorTable(0, gpuH(kDyn4 * 4));
                    for (UINT j = 0; j < kP4; ++j) {
                        const UINT k = kDyn4 + j;
                        const D3D12_VIEWPORT vp = { (float)(k % kW), (float)(k / kW), 1.0f, 1.0f, 0.0f, 1.0f };
                        c->RSSetViewports(1, &vp);
                        const D3D12_VERTEX_BUFFER_VIEW vbv = { bufs[j % kB4]->GetGPUVirtualAddress() + kPvbOff + (j / kB4) * 256, 12, 4 };
                        c->IASetVertexBuffers(0, 1, &vbv);
                        c->DrawInstanced(3, 1, 0, 0);
                    }
                    D3D12_RESOURCE_BARRIER b1 = Transition(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    c->ResourceBarrier(1, &b1);
                    D3D12_TEXTURE_COPY_LOCATION dst = {}, src = {};
                    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
                    src.pResource = rt; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
                    c->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    D3D12_RESOURCE_BARRIER b2 = Transition(rt, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    c->ResourceBarrier(1, &b2);
                }
                return c->Close();
            };
            CHECK(record(L[0], al[0], 0, kDyn4 / 2, false));
            { ID3D12CommandList* sub[] = { L[0] }; q->ExecuteCommandLists(1, sub); }
            // Once the first submission has run on the GPU (its mirror copies are done; without the
            // wait they may still pick up what we write next): the rest of the data, and P rewritten
            // in place (no new SRV, no VB use).
            waitIdle();
            for (UINT k = kDyn4 / 2; k < kDyn4; ++k) writeDraw(k);
            *(UINT*)pMap = pVal(fr);
            CHECK(record(L[1], al[1], kDyn4 / 2, kDyn4, true));
            { ID3D12CommandList* sub[] = { L[1] }; q->ExecuteCommandLists(1, sub); }
            CHECK(sc->Present(0, 0));
            waitIdle();

            BYTE* m = nullptr;
            CHECK(rb->Map(0, nullptr, (void**)&m));
            auto px = [&](UINT k) { return *(const UINT*)(m + fp.Offset + (k / kW) * fp.Footprint.RowPitch + (k % kW) * 4); };
            int bad = 0;
            for (UINT k = 0; k < kDyn4; ++k) {
                const UINT want = Val(seed, k) ^ rawVal(seed, k) ^ stVal(seed, k) ^ tyVal(seed, k);
                if (px(k) != want && bad++ < 4) printf("  phase 4 frame %d: draw %u got 0x%08x want 0x%08x\n", fr, k, px(k), want);
            }
            int pFresh = 0, pOld = 0; // P read this frame's value / an earlier frame's value
            for (UINT j = 0; j < kP4; ++j) {
                const UINT got = px(kDyn4 + j) ^ pvbVal(seed, j);
                if (got == pVal(fr)) { ++pFresh; continue; }
                for (int f = -1; f < fr; ++f) if (got == pVal(f)) { ++pOld; break; }
            }
            D3D12_RANGE none = { 0, 0 };
            rb->Unmap(0, &none);
            if (bad) { ++mainBad; printf("phase 4 frame %d: %d mismatches\n", fr, bad); }
            if (pFresh != (int)kP4) {
                if (pOld == (int)kP4) ++pStale; else ++pWrongOther;
            }
            for (UINT b = 0; b < kB4; ++b) bufs[b]->Release();
            ringOff += kB4 * kStride4 + (64 << 10); // shift the ring phase (offset mod 12 varies: alias and twin SRVs)
        }
        P->Release();
        heapA->Release();
        if (heapB) heapB->Release();
        int fail4 = mainBad + pWrongOther;
        const bool pOk = expectPStale ? pStale > 0 : pStale == 0;
        if (!pOk) ++fail4;
        failures += fail4;
        printf("phase 4: %d frames: main draws failed in %d, persistent buffer stale in %d frame(s), other P errors %d"
               " -> %s%s\n", frames4, mainBad, pStale, pWrongOther,
               pOk ? "as expected" : expectPStale ? "NOT STALE: SRV redirect not active?" : "STALE: SRV mirror not refreshed",
               noCopy ? " (no-copy test mode: failures expected)" : "");
    }
    if (dev->GetDeviceRemovedReason() != S_OK) { printf("DEVICE REMOVED 0x%08lx\n", dev->GetDeviceRemovedReason()); return 1; }

    printf("waiting for stats...\n");
    Sleep(2500);
    printf(failures ? "SMOKE FAILED\n" : "SMOKE PASSED\n");
    return failures ? 1 : 0;
}
