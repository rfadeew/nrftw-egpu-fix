// Feasibility probe for stage 2d change tracking (no version.dll involved).
//   A) Write-watch heap: VirtualAlloc(MEM_WRITE_WATCH) memory turned into a D3D12 heap with
//      ID3D12Device3::OpenExistingHeapFromAddress. Can the game's placed buffers (tight
//      alignment, GENERIC_READ) live in it? Does Map return our memory? Does GetWriteWatch
//      see CPU (and kernel) writes? GPU copy speed and correctness out of it.
//   B) Write protection on a normal UPLOAD heap mapping: is a placed buffer's Map pointer
//      inside the whole-heap mapping, does VirtualProtect work on it, cost per fault.
// Build: tests\build_wwprobe.bat -> build\wwprobe.exe; run from build\ (uses build\D3D12\D3D12Core.dll).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <vector>
#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern "C" { __declspec(dllexport) extern const UINT D3D12SDKVersion = 618; }
extern "C" { __declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\"; }

static const UINT64 kSz = 32ull << 20;
static const D3D12_RESOURCE_FLAGS kTight = (D3D12_RESOURCE_FLAGS)0x400; // D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT
static LARGE_INTEGER g_qf;
static double Ms(LONGLONG a, LONGLONG b) { return (b - a) * 1000.0 / g_qf.QuadPart; }
static LONGLONG Now() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }

static D3D12_RESOURCE_DESC BufDesc(UINT64 w, D3D12_RESOURCE_FLAGS f)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = w; d.Height = 1; d.DepthOrArraySize = 1;
    d.MipLevels = 1; d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; d.Flags = f;
    return d;
}

static ID3D12Resource* Committed(ID3D12Device* dev, D3D12_HEAP_TYPE t, UINT64 w, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = { t };
    D3D12_RESOURCE_DESC d = BufDesc(w, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* r = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static const char* MemType(DWORD t) { return t == MEM_PRIVATE ? "PRIVATE" : t == MEM_MAPPED ? "MAPPED" : t == MEM_IMAGE ? "IMAGE" : "?"; }

static void Region(const char* what, const void* p)
{
    MEMORY_BASIC_INFORMATION mi = {};
    VirtualQuery(p, &mi, sizeof(mi));
    printf("  %s %p: base %p alloc %p size %.2f MB type %s protect 0x%lx allocProtect 0x%lx\n", what, p, mi.BaseAddress,
           mi.AllocationBase, mi.RegionSize / 1048576.0, MemType(mi.Type), mi.Protect, mi.AllocationProtect);
}

// Write-protect fault handler for test B.
static volatile LONG g_faults = 0;
static BYTE* g_protLo = nullptr; static BYTE* g_protHi = nullptr;
static DWORD g_restore = PAGE_READWRITE;
static LONG CALLBACK Veh(EXCEPTION_POINTERS* e)
{
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || e->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    BYTE* a = (BYTE*)e->ExceptionRecord->ExceptionInformation[1];
    if (e->ExceptionRecord->ExceptionInformation[0] != 1 || a < g_protLo || a >= g_protHi) return EXCEPTION_CONTINUE_SEARCH;
    DWORD old;
    if (!VirtualProtect((void*)((uintptr_t)a & ~4095ull), 4096, g_restore, &old)) return EXCEPTION_CONTINUE_SEARCH;
    InterlockedIncrement(&g_faults);
    return EXCEPTION_CONTINUE_EXECUTION;
}

int wmain()
{
    QueryPerformanceFrequency(&g_qf);
    IDXGIFactory6* f = nullptr;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&f));
    IDXGIAdapter1* ad = nullptr;
    f->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad));
    DXGI_ADAPTER_DESC1 add = {};
    ad->GetDesc1(&add);
    printf("adapter: %ls\n", add.Description);
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { printf("no device\n"); return 1; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS19 o19 = {};
    dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS19, &o19, sizeof(o19));
    D3D12_FEATURE_DATA_EXISTING_HEAPS eh = {};
    HRESULT hrF = dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof(eh));
    printf("EXISTING_HEAPS: hr 0x%08lx Supported %d\n", hrF, eh.Supported);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q = nullptr;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12Fence* fence = nullptr;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 fv = 0;
    auto waitIdle = [&]() {
        q->Signal(fence, ++fv);
        if (fence->GetCompletedValue() < fv) { fence->SetEventOnCompletion(fv, ev); WaitForSingleObject(ev, INFINITE); }
    };
    ID3D12CommandAllocator* al = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Resource* def = Committed(dev, D3D12_HEAP_TYPE_DEFAULT, kSz, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* rb = Committed(dev, D3D12_HEAP_TYPE_READBACK, kSz, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource* tsRb = Committed(dev, D3D12_HEAP_TYPE_READBACK, 16, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_QUERY_HEAP_DESC qhd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0 };
    ID3D12QueryHeap* qh = nullptr;
    dev->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh));
    UINT64 tsFreq = 1;
    q->GetTimestampFrequency(&tsFreq);

    // Copies src[0, kSz) -> DEFAULT (timed) -> READBACK; returns bytes that differ from expect.
    auto gpuCheck = [&](ID3D12Resource* src, const BYTE* expect, double* ms) -> UINT64 {
        al->Reset();
        cl->Reset(al, nullptr);
        cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
        cl->CopyBufferRegion(def, 0, src, 0, kSz);
        cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; b.Transition.pResource = def;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        cl->CopyBufferRegion(rb, 0, def, 0, kSz);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cl->ResourceBarrier(1, &b);
        cl->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsRb, 0);
        cl->Close();
        ID3D12CommandList* l[] = { cl };
        q->ExecuteCommandLists(1, l);
        waitIdle();
        UINT64* ts = nullptr;
        D3D12_RANGE none = { 0, 0 };
        tsRb->Map(0, nullptr, (void**)&ts);
        *ms = (ts[1] - ts[0]) * 1000.0 / (double)tsFreq;
        tsRb->Unmap(0, &none);
        const BYTE* m = nullptr;
        rb->Map(0, nullptr, (void**)&m);
        UINT64 bad = 0;
        for (UINT64 i = 0; i < kSz; ++i) bad += m[i] != expect[i];
        rb->Unmap(0, &none);
        return bad;
    };
    std::vector<BYTE> expect(kSz);

    // ============ A) write-watch memory as a D3D12 heap ============
    printf("\n== A) OpenExistingHeapFromAddress on VirtualAlloc(MEM_WRITE_WATCH) ==\n");
    ID3D12Device3* dev3 = nullptr;
    dev->QueryInterface(IID_PPV_ARGS(&dev3));
    BYTE* mem = (BYTE*)VirtualAlloc(nullptr, kSz, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
    printf("VirtualAlloc(WRITE_WATCH) %p\n", mem);
    ID3D12Heap* wh = nullptr;
    HRESULT hr = dev3 && mem ? dev3->OpenExistingHeapFromAddress(mem, IID_PPV_ARGS(&wh)) : E_FAIL;
    printf("OpenExistingHeapFromAddress: 0x%08lx\n", hr);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_DESC hd = wh->GetDesc();
        printf("  heap desc: size %.1f MB align %llu type %d cpuPage %d pool %d flags 0x%x\n", hd.SizeInBytes / 1048576.0,
               hd.Alignment, hd.Properties.Type, hd.Properties.CPUPageProperty, hd.Properties.MemoryPoolPreference, hd.Flags);
        Region("memory", mem);
        // Game-like placed buffers.
        struct V { const char* name; UINT64 off, w; D3D12_RESOURCE_FLAGS f; };
        const V vs[] = {
            { "tight, off 0x21000, 12.1 KB", 0x21000, 12388, kTight },
            { "tight|xadapter, off 0x21000", 0x21000, 12388, kTight | D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER },
            { "none, off 0x30000 (64K-aligned? no)", 0x30000, 12388, D3D12_RESOURCE_FLAG_NONE },
            { "none, off 0x40000", 0x40000, 12388, D3D12_RESOURCE_FLAG_NONE },
            { "xadapter, off 0x40000", 0x40000, 12388, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER },
            { "whole heap, none", 0, kSz, D3D12_RESOURCE_FLAG_NONE },
            { "whole heap, xadapter", 0, kSz, D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER },
        };
        ID3D12Resource* whole = nullptr;
        ID3D12Resource* smallBuf = nullptr;
        UINT64 smallOff = 0;
        for (const V& v : vs) {
            D3D12_RESOURCE_DESC d = BufDesc(v.w, v.f);
            D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &d);
            ID3D12Resource* r = nullptr;
            HRESULT h2 = dev->CreatePlacedResource(wh, v.off, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
            void* p = nullptr;
            HRESULT h3 = r ? r->Map(0, nullptr, &p) : E_FAIL;
            printf("  placed %-36s: allocInfo size %llu align %llu | create 0x%08lx map 0x%08lx ptr %p (expected %p)%s\n", v.name,
                   ai.SizeInBytes, ai.Alignment, h2, h3, p, mem + v.off, p == mem + v.off ? " SAME" : "");
            if (r && v.off == 0 && !whole) whole = r;
            else if (r && !smallBuf) { smallBuf = r; smallOff = v.off; }
            else if (r) r->Release();
        }
        if (whole) {
            // Write watch: CPU writes through the mapped pointer.
            ULONG_PTR cnt = 0; DWORD gran = 0;
            std::vector<void*> pages(kSz / 4096);
            ResetWriteWatch(mem, kSz);
            BYTE* p = nullptr;
            whole->Map(0, nullptr, (void**)&p);
            p[0x21005] = 1; p[0x21005 + 5000] = 2; p[kSz - 1] = 3;
            cnt = pages.size();
            UINT r1 = GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
            printf("  write watch after 3 CPU writes: ret %u, %llu dirty page(s), gran %lu:", r1, (unsigned long long)cnt, gran);
            for (ULONG_PTR i = 0; i < cnt && i < 8; ++i) printf(" +0x%llx", (unsigned long long)((BYTE*)pages[i] - mem));
            printf("\n");
            // Kernel write: ReadFile into the heap.
            wchar_t tmp[MAX_PATH], fn[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            GetTempFileNameW(tmp, L"wwp", 0, fn);
            HANDLE fh = CreateFileW(fn, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            std::vector<BYTE> fbuf(65536, 0x5a);
            DWORD n = 0;
            WriteFile(fh, fbuf.data(), (DWORD)fbuf.size(), &n, nullptr);
            SetFilePointer(fh, 0, nullptr, FILE_BEGIN);
            BOOL okr = ReadFile(fh, p + 0x100000, 65536, &n, nullptr);
            CloseHandle(fh);
            cnt = pages.size();
            GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
            printf("  ReadFile 64 KB into heap: ok %d read %lu -> %llu dirty page(s) (expect 16)\n", okr, n, (unsigned long long)cnt);
            // Direct I/O (DMA into user pages, no CPU copy) and overlapped reads: does write watch see them?
            {
                GetTempFileNameW(tmp, L"wwd", 0, fn);
                HANDLE wf = CreateFileW(fn, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                std::vector<BYTE> pat(65536);
                for (size_t i = 0; i < pat.size(); ++i) pat[i] = (BYTE)(i * 37 + 11);
                WriteFile(wf, pat.data(), (DWORD)pat.size(), &n, nullptr);
                FlushFileBuffers(wf);
                CloseHandle(wf);
                struct M { const char* name; DWORD flags; UINT64 off; };
                const M ms[] = {
                    { "NO_BUFFERING, sync", FILE_FLAG_NO_BUFFERING, 0x200000 },
                    { "OVERLAPPED, buffered", FILE_FLAG_OVERLAPPED, 0x300000 },
                    { "OVERLAPPED | NO_BUFFERING", FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING, 0x400000 },
                };
                for (const M& m : ms) {
                    memset(p + m.off, 0, 65536);
                    cnt = pages.size();
                    GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
                    HANDLE rf = CreateFileW(fn, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, m.flags, nullptr);
                    BOOL ok = FALSE; DWORD got = 0;
                    if (rf != INVALID_HANDLE_VALUE) {
                        if (m.flags & FILE_FLAG_OVERLAPPED) {
                            OVERLAPPED ov = {};
                            ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                            ok = ReadFile(rf, p + m.off, 65536, nullptr, &ov);
                            if (!ok && GetLastError() == ERROR_IO_PENDING) ok = TRUE;
                            if (ok) ok = GetOverlappedResult(rf, &ov, &got, TRUE);
                            CloseHandle(ov.hEvent);
                        } else {
                            ok = ReadFile(rf, p + m.off, 65536, &got, nullptr);
                        }
                        CloseHandle(rf);
                    }
                    const bool dataOk = memcmp(p + m.off, pat.data(), 65536) == 0;
                    cnt = pages.size();
                    GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
                    printf("  ReadFile %-26s 64 KB into heap: ok %d read %lu data %s -> %llu dirty page(s) (expect 16)%s\n",
                           m.name, ok, got, dataOk ? "ok" : "WRONG", (unsigned long long)cnt,
                           cnt == 16 && ok && dataOk ? "" : "  <<< WRITE WATCH MISSED / READ FAILED");
                }
                DeleteFileW(fn);
            }
            // GetWriteWatch cost: 0 / 1000 / all pages dirty.
            for (UINT64 dirty : { 0ull, 1000ull, kSz / 4096 }) {
                for (UINT64 i = 0; i < dirty; ++i) p[(i * 4096 * 7) % kSz] = (BYTE)i;
                cnt = pages.size();
                const LONGLONG t0 = Now();
                GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
                const LONGLONG t1 = Now();
                printf("  GetWriteWatch(RESET) 32 MB with %llu written pages: %llu reported, %.3f ms\n", dirty,
                       (unsigned long long)cnt, Ms(t0, t1));
            }
            // CPU write speed into it (WRITE_BACK memory).
            LONGLONG t0 = Now();
            memset(p, 0x11, kSz);
            LONGLONG t1 = Now();
            printf("  CPU write 32 MB: %.0f MB/s\n", 32.0 / (Ms(t0, t1) / 1000.0));
            cnt = pages.size();
            t0 = Now();
            GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran);
            t1 = Now();
            printf("  after memset: %llu dirty pages, GetWriteWatch %.3f ms\n", (unsigned long long)cnt, Ms(t0, t1));
            // GPU: copy out of it twice (coherence after a second CPU write).
            double ms1 = 0, ms2 = 0;
            memset(expect.data(), 0x11, kSz);
            UINT64 bad1 = gpuCheck(whole, expect.data(), &ms1);
            for (UINT64 i = 0; i < kSz; ++i) p[i] = expect[i] = (BYTE)(i * 131 + 7);
            UINT64 bad2 = gpuCheck(whole, expect.data(), &ms2);
            printf("  GPU copy 32 MB out of it: %.3f / %.3f ms (%.2f GB/s) | GPU sees CPU data: %s (bad %llu / %llu)\n", ms1, ms2,
                   32.0 / 1024.0 / (ms2 / 1000.0), bad1 + bad2 ? "NO" : "yes", bad1, bad2);
            // Steady-state CPU write speed (pages resident; dirty bit set vs just reset).
            for (int pass = 0; pass < 3; ++pass) {
                if (pass == 2) { cnt = pages.size(); GetWriteWatch(WRITE_WATCH_FLAG_RESET, mem, kSz, pages.data(), &cnt, &gran); }
                t0 = Now();
                memset(p, 0x20 + pass, kSz);
                t1 = Now();
                printf("  CPU write 32 MB pass %d%s: %.0f MB/s\n", pass + 2, pass == 2 ? " (after RESET)" : "",
                       32.0 / (Ms(t0, t1) / 1000.0));
            }
            // Many scattered small copies (like dirty-page runs): 2000 x 12 KB, DIRECT queue timestamps.
            {
                const int kR = 2000; const UINT64 kRs = 12288;
                for (UINT64 i = 0; i < kSz; ++i) p[i] = expect[i] = (BYTE)(i * 17 + 3);
                al->Reset();
                cl->Reset(al, nullptr);
                cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
                for (int i = 0; i < kR; ++i) {
                    const UINT64 o = (UINT64)i * 16384;
                    cl->CopyBufferRegion(def, o, whole, o, kRs);
                }
                cl->EndQuery(qh, D3D12_QUERY_TYPE_TIMESTAMP, 1);
                cl->ResolveQueryData(qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, tsRb, 0);
                cl->Close();
                ID3D12CommandList* l[] = { cl };
                q->ExecuteCommandLists(1, l);
                waitIdle();
                UINT64* ts = nullptr;
                D3D12_RANGE none = { 0, 0 };
                tsRb->Map(0, nullptr, (void**)&ts);
                const double ms = (ts[1] - ts[0]) * 1000.0 / (double)tsFreq;
                tsRb->Unmap(0, &none);
                printf("  %d scattered copies x 12 KB (%.1f MB), DIRECT: %.3f ms (%.2f GB/s)\n", kR, kR * kRs / 1048576.0, ms,
                       kR * kRs / 1073741824.0 / (ms / 1000.0));
                // Same on a COPY queue (wall time incl. submission).
                D3D12_COMMAND_QUEUE_DESC cqd = { D3D12_COMMAND_LIST_TYPE_COPY };
                ID3D12CommandQueue* cq = nullptr;
                ID3D12CommandAllocator* ca = nullptr;
                ID3D12GraphicsCommandList* cc = nullptr;
                ID3D12Fence* cf = nullptr;
                dev->CreateCommandQueue(&cqd, IID_PPV_ARGS(&cq));
                dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&ca));
                dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, ca, nullptr, IID_PPV_ARGS(&cc));
                dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&cf));
                if (cq && cc && cf) {
                    // def is in COPY_DEST (explicit state); on a COPY queue that is fine.
                    for (int i = 0; i < kR; ++i) {
                        const UINT64 o = (UINT64)i * 16384;
                        cc->CopyBufferRegion(def, o, whole, o, kRs);
                    }
                    HRESULT hc = cc->Close();
                    ID3D12CommandList* l2[] = { cc };
                    const LONGLONG c0 = Now();
                    cq->ExecuteCommandLists(1, l2);
                    cq->Signal(cf, 1);
                    cf->SetEventOnCompletion(1, ev);
                    WaitForSingleObject(ev, 5000);
                    const LONGLONG c1 = Now();
                    printf("  same on a COPY queue: close 0x%08lx, done %d, wall %.3f ms\n", hc, cf->GetCompletedValue() == 1,
                           Ms(c0, c1));
                }
            }
            // The "VRAM heap" idea: the game's tight placed buffers in a DEFAULT heap, GENERIC_READ.
            {
                D3D12_HEAP_DESC dh = {};
                dh.SizeInBytes = kSz; dh.Alignment = 4ull << 20; dh.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
                ID3D12Heap* dhp = nullptr;
                HRESULT h1 = dev->CreateHeap(&dh, IID_PPV_ARGS(&dhp));
                D3D12_RESOURCE_DESC d = BufDesc(12388, kTight);
                ID3D12Resource* r = nullptr;
                HRESULT h2 = dhp ? dev->CreatePlacedResource(dhp, 0x21000, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                             IID_PPV_ARGS(&r)) : E_FAIL;
                void* mp = nullptr;
                HRESULT h3 = r ? r->Map(0, nullptr, &mp) : E_FAIL;
                printf("  DEFAULT heap 0x%08lx, tight placed GENERIC_READ at 0x21000: 0x%08lx, Map: 0x%08lx (expected to fail)\n",
                       h1, h2, h3);
                if (r) r->Release();
                if (dhp) dhp->Release();
            }
            // Small buffer's map pointer aliases the whole one?
            if (smallBuf) {
                BYTE* sp = nullptr;
                smallBuf->Map(0, nullptr, (void**)&sp);
                printf("  small buffer at +0x%llx maps to %p, whole+off = %p: %s\n", smallOff, sp, p + smallOff,
                       sp == p + smallOff ? "same VA" : "DIFFERENT VA");
            }
        }
        if (smallBuf) smallBuf->Release();
        if (whole) whole->Release();
        wh->Release();
    }

    // ============ B) write protection on a normal UPLOAD heap ============
    printf("\n== B) VirtualProtect on a normal UPLOAD heap mapping ==\n");
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = kSz; hd.Alignment = 4ull << 20; hd.Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Heap* uh = nullptr;
    hr = dev->CreateHeap(&hd, IID_PPV_ARGS(&uh));
    ID3D12Resource* uw = nullptr;
    ID3D12Resource* us = nullptr;
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_DESC d = BufDesc(kSz, D3D12_RESOURCE_FLAG_NONE);
        dev->CreatePlacedResource(uh, 0, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uw));
        d = BufDesc(12388, kTight);
        dev->CreatePlacedResource(uh, 0x21000, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&us));
    }
    if (uw && us) {
        BYTE* p = nullptr; BYTE* sp = nullptr;
        uw->Map(0, nullptr, (void**)&p);
        us->Map(0, nullptr, (void**)&sp);
        printf("whole map %p, smallBuf (+0x21000) map %p -> %s\n", p, sp, sp == p + 0x21000 ? "same VA" : "DIFFERENT VA");
        Region("whole", p);
        Region("small", sp);
        MEMORY_BASIC_INFORMATION mi = {};
        VirtualQuery(p, &mi, sizeof(mi));
        const DWORD cache = mi.Protect & (PAGE_WRITECOMBINE | PAGE_NOCACHE);
        DWORD old = 0;
        BOOL ok1 = VirtualProtect(p, 1 << 20, PAGE_READONLY, &old);
        DWORD e1 = GetLastError();
        if (ok1) VirtualProtect(p, 1 << 20, old, &old);
        BOOL ok2 = VirtualProtect(p, 1 << 20, PAGE_READONLY | cache, &old);
        DWORD e2 = GetLastError();
        printf("VirtualProtect(READONLY): %d (err %lu) | (READONLY|cache 0x%lx): %d (err %lu), old 0x%lx\n", ok1, e1, cache, ok2,
               e2, old);
        if (ok2) {
            Region("after protect", p);
            g_restore = old;
            g_protLo = p; g_protHi = p + (1 << 20);
            void* vh = AddVectoredExceptionHandler(1, Veh);
            LONGLONG t0 = Now();
            for (int i = 0; i < 256; ++i) p[i * 4096 + 17] = (BYTE)i;
            LONGLONG t1 = Now();
            printf("256 writes into protected pages: %ld faults, %.3f ms (%.2f us/fault)\n", g_faults, Ms(t0, t1),
                   Ms(t0, t1) * 1000.0 / 256);
            // Re-protect cost (with TLB flush) for 256 pages, one call.
            t0 = Now();
            VirtualProtect(p, 1 << 20, PAGE_READONLY | cache, &old);
            t1 = Now();
            printf("re-protect 1 MB: %.3f ms\n", Ms(t0, t1));
            // Kernel write into a protected page.
            wchar_t tmp[MAX_PATH], fn[MAX_PATH];
            GetTempPathW(MAX_PATH, tmp);
            GetTempFileNameW(tmp, L"wwp", 0, fn);
            HANDLE fh = CreateFileW(fn, GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
            std::vector<BYTE> fbuf(8192, 0x5a);
            DWORD n = 0;
            WriteFile(fh, fbuf.data(), (DWORD)fbuf.size(), &n, nullptr);
            SetFilePointer(fh, 0, nullptr, FILE_BEGIN);
            const LONG f0 = g_faults;
            BOOL okr = ReadFile(fh, p + 8192, 8192, &n, nullptr);
            DWORD er = GetLastError();
            CloseHandle(fh);
            printf("ReadFile into protected pages: ok %d (err %lu), read %lu, faults +%ld\n", okr, okr ? 0 : er, n, g_faults - f0);
            VirtualProtect(p, 1 << 20, old, &old);
            RemoveVectoredExceptionHandler(vh);
        }
        // Normal UPLOAD heap GPU copy speed for reference.
        for (UINT64 i = 0; i < kSz; ++i) expect[i] = (BYTE)(i * 131 + 7);
        LONGLONG t0 = Now();
        memcpy(p, expect.data(), kSz);
        LONGLONG t1 = Now();
        double ms = 0;
        UINT64 bad = gpuCheck(uw, expect.data(), &ms);
        printf("UPLOAD heap: CPU write %.0f MB/s, GPU copy 32 MB %.3f ms (%.2f GB/s), bad %llu\n", 32.0 / (Ms(t0, t1) / 1000.0),
               ms, 32.0 / 1024.0 / (ms / 1000.0), bad);
    } else {
        printf("UPLOAD heap setup failed 0x%08lx\n", hr);
    }
    return 0;
}
