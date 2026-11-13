// Stage 1: D3D12 / DXGI logging hooks.
//
// Goal: find out which buffers in system memory (UPLOAD / READBACK / CUSTOM L0
// heaps) the GPU reads during rendering, and how (root CBV/SRV, CBV/SRV
// descriptors, vertex/index buffers, copies). Over Thunderbolt without ReBAR
// every such read crosses PCIe, which is our suspected bottleneck.
//
// Hook chain:
//   DllMain -> D3D12CreateDevice, CreateDXGIFactory{,1,2}      (exports, MinHook)
//   first device -> device methods, queue ExecuteCommandLists, command list
//                   methods, ID3D12Resource::Map                (MinHook on vtable targets)
//   first factory -> CreateSwapChain{,ForHwnd} -> Present{,1}   (frame boundaries)
//
// Every buffer created through the device gets a record {GPU VA range, heap
// category}. Destruction is detected through a private-data IUnknown attached
// to the resource (D3D12 releases it when the resource dies). Binding calls
// look the GPU VA up in that range map (with a per-thread cache) and bump
// per-thread counters; a background thread writes aggregated stats every
// [Stats] IntervalSec seconds.
//
// Stage 2a ([Fix] ConstPages=1): every 4 MB committed UPLOAD buffer (the
// engine's constant ring pages) gets a DEFAULT-heap twin. Root CBV binds into
// a page are redirected to the twin and the byte range is recorded per command
// list; ExecuteCommandLists prepends our own command list that copies those
// ranges page -> twin. See the "Stage 2a" section below.
//
// Stage 2c ([Fix] VidmemHeaps / VidmemBuffers): UPLOAD heaps / committed UPLOAD
// buffers are created through NVAPI in CPU-visible VRAM (the 256 MB BAR1
// window), so the GPU reads them from VRAM. Only on an NVIDIA GPU without
// ReBAR (it switches itself off otherwise). See the "Stage 2c" section below.
//
// Stage 2d ([Fix] HybridHeaps / HybridSrv / HybridCheck): UPLOAD heaps that
// stage 2c did not place in VRAM stay in system memory and get a DEFAULT-heap
// mirror; VB/IB views (and optionally buffer SRVs) are redirected into it and
// the referenced ranges are copied at ExecuteCommandLists. See "Stage 2d".

#include "common.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <tlhelp32.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <stdio.h>
#include <stdarg.h>
#include <smmintrin.h>

#include "vtbl_index.h"

// ============================================================================
// Helpers
// ============================================================================
static void Appendf(std::string& s, const char* fmt, ...)
{
    char buf[1024];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    s += buf;
}

static double MB(uint64_t b) { return b / (1024.0 * 1024.0); }

static LARGE_INTEGER g_qpf;
static inline int64_t Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
static inline double QpcMs(int64_t dt) { return dt * 1000.0 / (double)g_qpf.QuadPart; }

// Call count + total/max duration (QPC ticks), updated from any thread.
struct TimeStat {
    std::atomic<uint64_t> n{0}, sum{0}, max{0};
    uint64_t snapN = 0, snapSum = 0; // stats thread only
};
static inline void AddTime(TimeStat& s, int64_t dt)
{
    const uint64_t d = dt > 0 ? (uint64_t)dt : 0;
    s.n.fetch_add(1, std::memory_order_relaxed);
    s.sum.fetch_add(d, std::memory_order_relaxed);
    uint64_t m = s.max.load(std::memory_order_relaxed);
    while (d > m && !s.max.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
}
struct TimeDelta { uint64_t n; double ms, maxMs; };
static TimeDelta TakeDelta(TimeStat& s)
{
    const uint64_t n = s.n.load(std::memory_order_relaxed), sum = s.sum.load(std::memory_order_relaxed);
    TimeDelta d = { n - s.snapN, QpcMs((int64_t)(sum - s.snapSum)), QpcMs((int64_t)s.max.exchange(0)) };
    s.snapN = n; s.snapSum = sum;
    return d;
}

// CPU time spent inside D3D12/DXGI calls of the game (measured in our detours).
enum ApiTime { AT_CCR_UPLOAD_SMALL, AT_CCR_OTHER, AT_MAP, AT_ECL, AT_PRESENT, AT_COUNT };
static const char* const kApiTimeName[AT_COUNT] = {
    "CreateCommitted UPLOAD buf<4MB", "CreateCommitted other", "Map", "ExecuteCommandLists(game)", "Present"
};
static TimeStat g_apiTime[AT_COUNT];

// "D3D12Core.dll+0x1234" for a code address.
static std::string DescribeAddr(const void* p)
{
    char out[MAX_PATH + 64];
    HMODULE m = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)p, &m) && m) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(m, path, MAX_PATH);
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;
        _snprintf_s(out, sizeof(out), _TRUNCATE, "%ls+0x%llx", name,
                    (unsigned long long)((const uint8_t*)p - (const uint8_t*)m));
    } else {
        _snprintf_s(out, sizeof(out), _TRUNCATE, "%p", p);
    }
    return out;
}

static const char* ListTypeName(D3D12_COMMAND_LIST_TYPE t)
{
    switch (t) {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:        return "DIRECT";
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:        return "BUNDLE";
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:       return "COMPUTE";
    case D3D12_COMMAND_LIST_TYPE_COPY:          return "COPY";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE:  return "VIDEO_DECODE";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS: return "VIDEO_PROCESS";
    case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE:  return "VIDEO_ENCODE";
    default:                                    return "?";
    }
}

// ============================================================================
// Hook installation
// ============================================================================
static std::recursive_mutex g_installMutex;
static std::map<void*, std::string> g_hookedTargets; // target -> name of first hook

// Creates a MinHook hook on `target` and queues it for enabling (call
// ApplyHooks() afterwards). Skips targets that are already hooked: different
// vtable slots / list types may share one implementation.
static bool HookTarget(void* target, void* detour, void** orig, const char* name)
{
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    if (!target) { Log("  hook %s: null target", name); return false; }
    auto it = g_hookedTargets.find(target);
    if (it != g_hookedTargets.end()) {
        if (it->second != name)
            Log("  hook %s: target %s already hooked as %s (shared implementation)",
                name, DescribeAddr(target).c_str(), it->second.c_str());
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_QueueEnableHook(target);
    if (s != MH_OK) {
        Log("  hook %s: MinHook error %s at %s", name, MH_StatusToString(s), DescribeAddr(target).c_str());
        return false;
    }
    g_hookedTargets[target] = name;
    Log("  hooked %s @ %s", name, DescribeAddr(target).c_str());
    return true;
}

static bool HookVT(void** vt, int idx, void* detour, void** orig, const char* name)
{
    return HookTarget(vt[idx], detour, orig, name);
}

static void ApplyHooks()
{
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    MH_STATUS s = MH_ApplyQueued();
    if (s != MH_OK) Log("MH_ApplyQueued failed: %s", MH_StatusToString(s));
}

// ============================================================================
// D3DKMT kernel thunks ([Stats] KmtStats=1, measurement only)
// ============================================================================
// Everything the D3D12 runtime and the driver's user-mode part ask of the
// kernel (allocations, GPU VA mapping, residency, submissions, sync) goes
// through the NtGdiDdDDI* syscall stubs in win32u.dll. All of these take one
// pointer argument; the detour forwards the four register arguments anyway.
static const char* const kKmtNames[] = {
    "CreateAllocation", "DestroyAllocation", "DestroyAllocation2",
    "ReserveGpuVirtualAddress", "MapGpuVirtualAddress", "UpdateGpuVirtualAddress", "FreeGpuVirtualAddress",
    "Lock", "Lock2", "Unlock", "Unlock2",
    "MakeResident", "Evict", "OfferAllocations", "ReclaimAllocations2", "SetAllocationPriority",
    "UpdateAllocationProperty", "QueryVideoMemoryInfo", "ChangeVideoMemoryReservation",
    "SubmitCommand", "SubmitCommandToHwQueue", "SubmitSignalSyncObjectsToHwQueue", "SubmitWaitForSyncObjectsToHwQueue",
    "SignalSynchronizationObjectFromCpu", "SignalSynchronizationObjectFromGpu", "SignalSynchronizationObjectFromGpu2",
    "WaitForSynchronizationObjectFromCpu", "WaitForSynchronizationObjectFromGpu",
    "CreateSynchronizationObject", "DestroySynchronizationObject",
    "Present", "PresentMultiPlaneOverlay3", "CreatePagingQueue", "Escape",
};
static constexpr int kKmtCount = (int)(sizeof(kKmtNames) / sizeof(kKmtNames[0]));
static TimeStat g_kmt[kKmtCount];

using PFN_Kmt = LONG(NTAPI*)(void*, void*, void*, void*);
template <int K> struct KmtHook {
    static inline PFN_Kmt o = nullptr;
    static LONG NTAPI D(void* a, void* b, void* c, void* d)
    {
        const int64_t t0 = Qpc();
        const LONG st = o(a, b, c, d);
        AddTime(g_kmt[K], Qpc() - t0);
        return st;
    }
};
template <int K> static void InstallKmtOne(HMODULE m)
{
    char name[96];
    sprintf_s(name, "NtGdiDdDDI%s", kKmtNames[K]);
    void* t = (void*)GetProcAddress(m, name);
    if (!t) { Log("  kmt: %s not exported", name); return; }
    HookTarget(t, (void*)&KmtHook<K>::D, (void**)&KmtHook<K>::o, name);
}
template <size_t... I> static void InstallKmtHooks(HMODULE m, std::index_sequence<I...>)
{
    (InstallKmtOne<(int)I>(m), ...);
}

// ============================================================================
// Statistics model
// ============================================================================
// HC_UPLOAD_VID: UPLOAD memory we placed in CPU-visible VRAM through NVAPI (stage 2c).
enum HeapCat : int {
    HC_DEFAULT, HC_UPLOAD, HC_READBACK, HC_GPU_UPLOAD, HC_CUSTOM_SYS, HC_CUSTOM_VID, HC_UPLOAD_VID, HC_UNTRACKED, HC_COUNT
};
static const char* const kCatName[HC_COUNT] = {
    "DEFAULT", "UPLOAD", "READBACK", "GPU_UPLOAD", "CUSTOM_SYS", "CUSTOM_VID", "UPLOAD_VID", "untracked"
};
static bool IsSysmem(int c) { return c == HC_UPLOAD || c == HC_READBACK || c == HC_CUSTOM_SYS; }

static HeapCat Classify(const D3D12_HEAP_PROPERTIES& p)
{
    switch (p.Type) {
    case D3D12_HEAP_TYPE_DEFAULT:    return HC_DEFAULT;
    case D3D12_HEAP_TYPE_UPLOAD:     return HC_UPLOAD;
    case D3D12_HEAP_TYPE_READBACK:   return HC_READBACK;
    case D3D12_HEAP_TYPE_GPU_UPLOAD: return HC_GPU_UPLOAD;
    case D3D12_HEAP_TYPE_CUSTOM:
        return p.MemoryPoolPreference == D3D12_MEMORY_POOL_L1 ? HC_CUSTOM_VID : HC_CUSTOM_SYS;
    default:                         return HC_UNTRACKED;
    }
}

// How a GPU VA / buffer was used.
enum Use : int {
    U_RCBV_G, U_RCBV_C, U_RSRV_G, U_RSRV_C, U_VB, U_IB, U_CBV_DESC, U_SRV_DESC, U_COPY_SRC, U_COPY_DST, U_COUNT
};
static const char* const kUseName[U_COUNT] = {
    "rootCBV gfx", "rootCBV cs", "rootSRV gfx", "rootSRV cs", "VB", "IB",
    "CBV desc", "SRV desc", "copy src", "copy dst"
};

enum Misc : int {
    M_DRAW, M_DRAW_IDX, M_DISPATCH, M_EXEC_INDIRECT, M_CL_RESET, M_CL_CLOSE,
    M_CL_RESET_DXGI, M_CL_CLOSE_DXGI, // made by DXGI itself inside Present (not the game)
    M_MAP,
    M_FIX_REDIRECT, // stage 2a: root CBV binds redirected to a twin
    M_FIX_SKIP,     // stage 2a: root CBV binds into a twinned page left on sysmem (list not tracked)
    M_HYB_VB,       // stage 2d: vertex buffer views redirected into a heap mirror
    M_HYB_IB,       // stage 2d: index buffer views redirected
    M_HYB_SKIP,     // stage 2d: VB/IB binds into a mirrored heap left on sysmem (list not tracked)
    M_HYB_SRV_ALIAS,// stage 2d: buffer SRVs redirected to the mirror's whole-heap buffer (FirstElement shifted)
    M_HYB_SRV_TWIN, // stage 2d: buffer SRVs redirected to a per-buffer twin placed in the mirror
    M_HYB_SRV_SKIP, // stage 2d: SRVs on mirrored buffers left on sysmem (no desc, not a buffer view, twin failed)
    M_COUNT
};

// > 0 while this thread is inside IDXGISwapChain::Present{,1}. DXGI records and
// submits its own command lists there; we count those separately.
static thread_local int t_presentDepth = 0;

// Per-thread counters: written only by the owning thread (no lock prefix),
// summed by the stats thread.
struct ThreadStats {
    std::atomic<uint64_t> use[U_COUNT][HC_COUNT];
    std::atomic<uint64_t> misc[M_COUNT];
};
static std::mutex g_tsMutex;
static std::vector<ThreadStats*> g_threadStats;
static thread_local ThreadStats* t_stats = nullptr;

static ThreadStats* TS()
{
    ThreadStats* s = t_stats;
    if (!s) {
        s = new ThreadStats;
        for (auto& row : s->use) for (auto& a : row) a.store(0, std::memory_order_relaxed);
        for (auto& a : s->misc) a.store(0, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_tsMutex);
        g_threadStats.push_back(s);
        t_stats = s;
    }
    return s;
}

static inline void Bump(std::atomic<uint64_t>& a, uint64_t n = 1)
{
    a.store(a.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

// ---- Buffer records -----------------------------------------------------------
struct HybHeap; // stage 2d

struct ResRec {
    uint64_t id = 0;
    ID3D12Resource* res = nullptr;   // not AddRef'd; valid while alive
    uint64_t va = 0, size = 0;
    HeapCat cat = HC_UNTRACKED;
    const char* how = "";            // which create call
    uint64_t heapOffset = 0;         // placed resources
    uint32_t heapFlags = 0, resFlags = 0, initState = 0;
    uint64_t createdFrame = 0;
    std::atomic<bool> alive{true};
    std::atomic<uint64_t> use[U_COUNT];
    std::atomic<uint64_t> maps{0};
    uint64_t snap[U_COUNT] = {};     // stats thread only
    uint64_t snapMaps = 0;
    // Byte range [winLo, winHi) referenced by GPU-read bindings since the last
    // game submission: roughly what a stage-2 copy would have to transfer.
    std::atomic<uint64_t> winLo{UINT64_MAX}, winHi{0};
    std::atomic<bool> inWindow{false};
    ResRec* nextTouched = nullptr;
    // Stage 2a: DEFAULT-heap twin (constant pages only). Set before the record
    // is registered and never cleared; twinDead is guarded by g_fixMutex.
    ID3D12Resource* twin = nullptr;
    uint64_t twinVA = 0;
    bool twinDead = false;
    // Stage 2d: placed in a mirrored UPLOAD heap (set before registration).
    HybHeap* hh = nullptr;
    // HybridCheck state (guarded by g_fixMutex, except hcSrv)
    std::atomic<bool> hcSrv{false};  // an SRV on it was redirected into the mirror
    bool hcTracked = false;          // in g_hcList: hashed at its last copy, re-checked at later submissions
    uint64_t hcHash = 0, hcCopyEcl = 0;
    uint32_t hcChanges = 0;
    ResRec() { for (auto& a : use) a.store(0, std::memory_order_relaxed); }
};

static bool IsGpuRead(int u) { return u != U_COPY_SRC && u != U_COPY_DST; }

// Records are never freed (cached pointers stay valid); dead ones just leave the map.
static SRWLOCK g_resLock = SRWLOCK_INIT;
static std::map<uint64_t, ResRec*> g_byVa;   // key: start VA
static std::vector<ResRec*> g_allRecs;
static std::atomic<uint64_t> g_resGen{1};
static std::atomic<uint64_t> g_nextResId{1};

static std::atomic<uint64_t> g_bufCreated[HC_COUNT], g_bufCreatedBytes[HC_COUNT];
static std::atomic<uint64_t> g_bufDestroyed[HC_COUNT], g_bufDestroyedBytes[HC_COUNT];
static std::atomic<uint64_t> g_heapCreated[HC_COUNT], g_heapCreatedBytes[HC_COUNT];
static std::atomic<int> g_createLogged{0};

static std::atomic<uint64_t> g_frames{0};

static void RegisterRec(ResRec* r)
{
    AcquireSRWLockExclusive(&g_resLock);
    const uint64_t end = r->va + r->size;
    auto it = g_byVa.lower_bound(r->va);
    if (it != g_byVa.begin()) {
        auto p = std::prev(it);
        if (p->first + p->second->size > r->va) it = p;
    }
    // Drop anything overlapping: stale entries (VA reuse) or aliased placed resources.
    while (it != g_byVa.end() && it->first < end) it = g_byVa.erase(it);
    g_byVa[r->va] = r;
    g_allRecs.push_back(r);
    g_resGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_resLock);
}

static void FixOnPageDestroyed(ResRec* r); // stage 2a
static void HcOnDestroyed(ResRec* r);       // stage 2d HybridCheck

static void OnResourceDestroyed(ResRec* r)
{
    if (r->twin) FixOnPageDestroyed(r);
    if (r->hh && g_cfg.hybridCheck) HcOnDestroyed(r);
    r->alive.store(false, std::memory_order_relaxed);
    AcquireSRWLockExclusive(&g_resLock);
    auto it = g_byVa.find(r->va);
    if (it != g_byVa.end() && it->second == r) g_byVa.erase(it);
    g_resGen.fetch_add(1, std::memory_order_release);
    ReleaseSRWLockExclusive(&g_resLock);
    g_bufDestroyed[r->cat].fetch_add(1, std::memory_order_relaxed);
    g_bufDestroyedBytes[r->cat].fetch_add(r->size, std::memory_order_relaxed);
}

// Per-thread lookup cache. On a miss it caches the gap between neighbours,
// so repeated lookups of untracked VAs are cheap too.
struct LookupCache { uint64_t gen = 0, lo = 0, hi = 0; ResRec* rec = nullptr; };
static thread_local LookupCache t_cache;

static ResRec* Lookup(uint64_t va)
{
    LookupCache& c = t_cache;
    if (c.gen == g_resGen.load(std::memory_order_acquire) && va >= c.lo && va < c.hi) return c.rec;

    AcquireSRWLockShared(&g_resLock);
    const uint64_t gen = g_resGen.load(std::memory_order_relaxed);
    ResRec* rec = nullptr;
    uint64_t lo = 0, hi = UINT64_MAX;
    auto it = g_byVa.upper_bound(va);
    if (it != g_byVa.end()) hi = it->first;
    if (it != g_byVa.begin()) {
        auto p = std::prev(it);
        const uint64_t pend = p->first + p->second->size;
        if (va < pend) { rec = p->second; lo = p->first; hi = pend; }
        else lo = pend;
    }
    ReleaseSRWLockShared(&g_resLock);
    c.gen = gen; c.lo = lo; c.hi = hi; c.rec = rec;
    return rec;
}

// ---- Submission windows ----------------------------------------------------------
// Records touched by GPU-read bindings since the last game ExecuteCommandLists
// (lock-free push; the ECL hook takes the whole list at once).
static std::atomic<ResRec*> g_touched{nullptr};

static void PushTouched(ResRec* r)
{
    ResRec* h = g_touched.load(std::memory_order_relaxed);
    do { r->nextTouched = h; }
    while (!g_touched.compare_exchange_weak(h, r, std::memory_order_release, std::memory_order_relaxed));
}

enum { WC_BIG, WC_SMALL, WC_COUNT }; // buffer size >= 1 MB (persistent pages) vs smaller
static std::atomic<uint64_t> g_winBytes[WC_COUNT], g_winBufs[WC_COUNT], g_winCount{0};

static void CloseWindow()
{
    ResRec* r = g_touched.exchange(nullptr, std::memory_order_acquire);
    while (r) {
        ResRec* next = r->nextTouched;
        const uint64_t lo = r->winLo.exchange(UINT64_MAX, std::memory_order_relaxed);
        const uint64_t hi = r->winHi.exchange(0, std::memory_order_relaxed);
        r->inWindow.store(false, std::memory_order_release);
        if (hi > lo) {
            const int c = r->size >= (1u << 20) ? WC_BIG : WC_SMALL;
            g_winBytes[c].fetch_add(hi - lo, std::memory_order_relaxed);
            g_winBufs[c].fetch_add(1, std::memory_order_relaxed);
        }
        r = next;
    }
    g_winCount.fetch_add(1, std::memory_order_relaxed);
}

static inline void AtomicMin(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v < cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}
static inline void AtomicMax(std::atomic<uint64_t>& a, uint64_t v)
{
    uint64_t cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

// Per-record counters and byte ranges are batched per thread (small cache of
// (record, use) slots) to avoid cache-line ping-pong on hot ring pages; slots
// are flushed on eviction and on CommandList::Close().
struct PendSlot { ResRec* rec; int use; uint64_t n, lo, hi; };
struct PendCache { PendSlot s[4]; int victim; };
static thread_local PendCache t_pend = {};

static void FlushSlot(PendSlot& p)
{
    if (p.rec && p.n) {
        ResRec* r = p.rec;
        r->use[p.use].fetch_add(p.n, std::memory_order_relaxed);
        if (p.hi > p.lo) {
            AtomicMin(r->winLo, p.lo);
            AtomicMax(r->winHi, p.hi);
            if (!r->inWindow.exchange(true, std::memory_order_acq_rel)) PushTouched(r);
        }
    }
    p.rec = nullptr; p.n = 0;
}

static inline void FlushPending()
{
    for (PendSlot& s : t_pend.s) FlushSlot(s);
}

static constexpr uint64_t kToEnd = ~0ull; // "size = rest of the buffer"

// size: bytes the binding covers (0 = unknown, assume 256; kToEnd = rest of buffer).
// Returns the buffer record containing va (nullptr if untracked).
static inline ResRec* CountVA(Use u, uint64_t va, uint64_t size = 0)
{
    if (!va) return nullptr;
    ResRec* r = Lookup(va);
    const int cat = r ? r->cat : HC_UNTRACKED;
    Bump(TS()->use[u][cat]);
    if (!r || cat == HC_DEFAULT) return r;

    uint64_t lo = 0, hi = 0;
    if (IsGpuRead(u) && IsSysmem(cat)) { // byte ranges only for real sysmem (not UPLOAD_VID)
        lo = va - r->va;
        hi = size == kToEnd ? r->size : lo + (size ? size : 256);
        if (hi > r->size) hi = r->size;
    }
    PendCache& pc = t_pend;
    for (PendSlot& s : pc.s) {
        if (s.rec == r && s.use == u) {
            ++s.n;
            if (hi > lo) { if (lo < s.lo) s.lo = lo; if (hi > s.hi) s.hi = hi; }
            return r;
        }
    }
    PendSlot& v = pc.s[pc.victim];
    pc.victim = (pc.victim + 1) & 3;
    FlushSlot(v);
    v.rec = r; v.use = u; v.n = 1; v.lo = lo; v.hi = hi;
    return r;
}

static inline void CountRes(Use u, ID3D12Resource* r, uint64_t offset, uint64_t size = 0)
{
    if (!r) return;
    const uint64_t va = r->GetGPUVirtualAddress(); // 0 for textures
    if (va) CountVA(u, va + offset, size);
}

static uint32_t FormatBytes(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_UINT: case DXGI_FORMAT_R32G32B32A32_SINT:
        return 16;
    case DXGI_FORMAT_R32G32B32_TYPELESS: case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R32G32B32_UINT: case DXGI_FORMAT_R32G32B32_SINT:
        return 12;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM: case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_TYPELESS: case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R32G32_UINT: case DXGI_FORMAT_R32G32_SINT:
        return 8;
    case DXGI_FORMAT_R16G16_TYPELESS: case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_UINT: case DXGI_FORMAT_R16G16_SNORM: case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_UINT: case DXGI_FORMAT_R16_SNORM: case DXGI_FORMAT_R16_SINT:
        return f >= DXGI_FORMAT_R16_TYPELESS ? 2 : 4;
    case DXGI_FORMAT_R8_TYPELESS: case DXGI_FORMAT_R8_UNORM: case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SNORM: case DXGI_FORMAT_R8_SINT:
        return 1;
    default:
        return 4;
    }
}

// ---- Destruction tracker ---------------------------------------------------------
// {5c1f0e7a-3b2d-4f61-9a8e-27c4d1e0b913}
static const GUID kTrackerGuid = { 0x5c1f0e7a, 0x3b2d, 0x4f61, { 0x9a, 0x8e, 0x27, 0xc4, 0xd1, 0xe0, 0xb9, 0x13 } };

struct Tracker final : public IUnknown {
    std::atomic<ULONG> ref{1};
    ResRec* rec;
    bool armed = false;
    explicit Tracker(ResRec* r) : rec(r) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref;
        if (r == 0) {
            if (armed) OnResourceDestroyed(rec);
            delete this;
        }
        return r;
    }
};

// ============================================================================
// Stage 2a: DEFAULT-heap twins for the 4 MB UPLOAD constant pages
// ============================================================================
// Over Thunderbolt without ReBAR every shader read of an UPLOAD page crosses
// PCIe. For each 4 MB committed UPLOAD buffer we create a twin of the same
// size in a DEFAULT heap (VRAM):
//   * Set{Graphics,Compute}RootConstantBufferView into a page is redirected to
//     the same offset in the twin, and [offset, offset + CbvPad) is recorded in
//     the command list's ListState (root CBV size is unknown).
//   * ExecuteCommandLists on a DIRECT queue (game submissions only, never the
//     DXGI-internal ones inside Present) gathers the ranges of all submitted
//     lists and prepends our own command list: CopyBufferRegion page -> twin
//     per merged range, then a COPY_DEST -> VERTEX_AND_CONSTANT_BUFFER barrier
//     per twin (twins start in COMMON, the copy promotes them to COPY_DEST,
//     buffers decay back to COMMON after every ExecuteCommandLists).
//   * Ranges live until the list's next Reset, so a list executed twice is
//     copied twice.
// Safety: the page itself stays valid, so anything not redirected (VB/IB,
// copies, ExecuteIndirect arguments, binds in lists we never saw Reset) still
// reads sysmem correctly. We only redirect in DIRECT lists whose Reset we saw,
// so every redirected bind has its range recorded where ECL will find it.
// Over-copying is harmless: ECL boundaries serialize our copy against earlier
// submissions, and every later submission re-copies its own ranges first.
static constexpr uint64_t kConstPageSize = 4ull << 20;
static constexpr uint64_t kMergeGap = 16 << 10;   // merge ranges closer than this
static constexpr size_t kMaxFixAllocators = 32;

static std::mutex g_fixMutex;                   // twins' death, injection, deferred releases
static std::atomic<bool> g_fixRedirect{false};  // cleared if the copy path ever fails
static thread_local int t_inFix = 0;            // > 0 while recording our own command list

static std::atomic<uint64_t> g_fixTwinsLive{0}, g_fixTwinsTotal{0};
static std::atomic<uint64_t> g_fixInjections{0}, g_fixCopyRegions{0}, g_fixCopyBytes{0}, g_fixFailures{0};

// Byte range [lo, hi) of a constant page (2a) or of a mirrored heap (2d).
template <class K> struct KRange { K* k; uint64_t lo, hi; };
using ListRange = KRange<ResRec>;
using HybRange = KRange<HybHeap>;

static inline bool NearRange(uint64_t elo, uint64_t ehi, uint64_t lo, uint64_t hi, uint64_t gap)
{
    return lo <= ehi + gap && hi + gap >= elo;
}

// Adds a range to a list's set: extends a recent entry of the same key if within
// gap, else appends. Bounded search (allocations are mostly sequential);
// ExecuteCommandLists sorts and merges everything anyway.
template <class K>
static void AddRange(std::vector<KRange<K>>& v, size_t& last, K* k, uint64_t lo, uint64_t hi, uint64_t gap)
{
    if (last < v.size()) {
        KRange<K>& e = v[last];
        if (e.k == k && NearRange(e.lo, e.hi, lo, hi, gap)) {
            if (lo < e.lo) e.lo = lo;
            if (hi > e.hi) e.hi = hi;
            return;
        }
    }
    const size_t n = v.size(), from = n > 16 ? n - 16 : 0;
    for (size_t i = n; i-- > from;) {
        KRange<K>& e = v[i];
        if (e.k == k && NearRange(e.lo, e.hi, lo, hi, gap)) {
            if (lo < e.lo) e.lo = lo;
            if (hi > e.hi) e.hi = hi;
            last = i;
            return;
        }
    }
    v.push_back({ k, lo, hi });
    last = v.size() - 1;
}

// Sorts by (key, lo) and merges ranges closer than gap, in place. Returns the
// exact union of the input in bytes (before gap filling).
template <class K>
static uint64_t MergeRanges(std::vector<KRange<K>>& v, uint64_t gap = kMergeGap)
{
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end(), [](const KRange<K>& a, const KRange<K>& b) {
        return a.k != b.k ? a.k < b.k : a.lo < b.lo;
    });
    uint64_t uni = 0, ulo = v[0].lo, uhi = v[0].hi;
    const K* uk = v[0].k;
    size_t o = 0;
    for (size_t i = 1; i < v.size(); ++i) {
        const KRange<K> r = v[i];
        if (r.k == uk && r.lo <= uhi) { if (r.hi > uhi) uhi = r.hi; }
        else { uni += uhi - ulo; uk = r.k; ulo = r.lo; uhi = r.hi; }
        KRange<K>& e = v[o];
        if (r.k == e.k && r.lo <= e.hi + gap) { if (r.hi > e.hi) e.hi = r.hi; }
        else v[++o] = r;
    }
    uni += uhi - ulo;
    v.resize(o + 1);
    return uni;
}

// Bytes of a that are also in b (both sorted and merged with gap 0).
template <class K>
static uint64_t OverlapBytes(const std::vector<KRange<K>>& a, const std::vector<KRange<K>>& b)
{
    uint64_t n = 0;
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const KRange<K>& x = a[i];
        const KRange<K>& y = b[j];
        if (x.k != y.k) { if (x.k < y.k) ++i; else ++j; continue; }
        const uint64_t lo = x.lo > y.lo ? x.lo : y.lo, hi = x.hi < y.hi ? x.hi : y.hi;
        if (hi > lo) n += hi - lo;
        if (x.hi < y.hi) ++i; else ++j;
    }
    return n;
}

// What a command list recorded ([Stats] GpuTiming, to explain its GPU time).
struct ListCounters {
    uint32_t draws, dispatches, execInd;
    uint32_t vbSys, vbAll; // vertex buffer views: in sysmem (UPLOAD/READBACK/CUSTOM_SYS) / all
    uint32_t rootCbv;
};

// Per command list; touched by the recording thread, read by ExecuteCommandLists
// after Close (the game orders those). Never freed (lists are reused).
struct ListState {
    bool open = false;    // recording, started by a Reset we saw
    bool known = false;   // the current recording started with a Reset we saw (cnt is complete)
    bool direct = false;  // D3D12_COMMAND_LIST_TYPE_DIRECT
    ListCounters cnt = {};
    size_t last = 0, hlast = 0;     // entries extended last
    std::vector<ListRange> ranges;  // stage 2a: constant page ranges read through redirected root CBVs
    std::vector<HybRange> hranges;  // stage 2d: mirrored heap ranges read through redirected VB/IB views

    void Add(ResRec* r, uint64_t lo, uint64_t hi) { AddRange(ranges, last, r, lo, hi, kMergeGap); }
    // Exact (gap 0): the copy merges with [Fix] HybridMergeKB later, the stats need the exact union.
    void AddH(HybHeap* h, uint64_t lo, uint64_t hi) { AddRange(hranges, hlast, h, lo, hi, 0); }
};

static SRWLOCK g_listLock = SRWLOCK_INIT;
static std::map<const void*, ListState*> g_lists; // command list pointer -> state (never erased)
static thread_local const void* t_curList = nullptr;
static thread_local ListState* t_curState = nullptr;

static ListState* FindListState(const void* list)
{
    if (list == t_curList) return t_curState;
    AcquireSRWLockShared(&g_listLock);
    auto it = g_lists.find(list);
    ListState* ls = it != g_lists.end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_listLock);
    if (ls) { t_curList = list; t_curState = ls; } // entries are permanent, so caching is safe
    return ls;
}

static ListState* GetOrCreateListState(const void* list)
{
    if (ListState* ls = FindListState(list)) return ls;
    AcquireSRWLockExclusive(&g_listLock);
    ListState*& slot = g_lists[list];
    if (!slot) slot = new ListState;
    ListState* ls = slot;
    ReleaseSRWLockExclusive(&g_listLock);
    t_curList = list; t_curState = ls;
    return ls;
}

static void OnListReset(ID3D12GraphicsCommandList* cl, bool ok)
{
    ListState* ls = GetOrCreateListState(cl);
    ls->ranges.clear();
    ls->hranges.clear();
    ls->last = ls->hlast = 0;
    ls->cnt = {};
    ls->direct = cl->GetType() == D3D12_COMMAND_LIST_TYPE_DIRECT;
    ls->open = ls->known = ok && t_presentDepth == 0;
}

static void OnListClose(ID3D12GraphicsCommandList* cl)
{
    if (ListState* ls = FindListState(cl)) ls->open = false;
}

// Counters of a list being recorded (nullptr unless GpuTiming and we saw its Reset).
static inline ListCounters* Counters(ID3D12GraphicsCommandList* cl)
{
    if (!g_cfg.gpuTiming) return nullptr;
    ListState* ls = FindListState(cl);
    return ls && ls->open ? &ls->cnt : nullptr;
}

// Root CBV bind into a twinned page: returns the address to pass on.
static D3D12_GPU_VIRTUAL_ADDRESS RedirectCbv(ID3D12GraphicsCommandList* cl, ResRec* r, D3D12_GPU_VIRTUAL_ADDRESS va)
{
    ListState* ls = FindListState(cl);
    if (!ls || !ls->open || !ls->direct || !g_fixRedirect.load(std::memory_order_relaxed)) {
        Bump(TS()->misc[M_FIX_SKIP]);
        return va;
    }
    const uint64_t off = va - r->va;
    uint64_t hi = off + (uint64_t)g_cfg.cbvPad;
    if (hi > r->size) hi = r->size;
    ls->Add(r, off, hi);
    Bump(TS()->misc[M_FIX_REDIRECT]);
    return r->twinVA + off;
}

// ---- GPU timing ([Stats] GpuTiming=1, measurement only) ----------------------------
// Every game submission on a DIRECT queue becomes
//   TS[0], seg 0, TS[1], seg 1, ..., seg m-1, post
// where the segments are our copy list (if any) and the game's lists, TS[k] are
// pre-recorded lists with a single timestamp query k, and "post" writes query m
// and resolves 0..m into a readback slot. Two banks of query indices alternate
// between consecutive submissions. Results are read once our fence passes.
static constexpr int kTsBanks = 2, kTsPerBank = 192, kTsSlots = 32;

struct TsSeg { ListCounters c; bool known, copy; };
struct TsSlot {
    bool pending = false;
    uint64_t fence = 0;
    int64_t submitQpc = 0;
    uint32_t bank = 0, ecl = 0;   // ecl = index of this submission within its frame
    std::vector<TsSeg> segs;
};

enum ListKind { LK_DRAW_SYSVB, LK_DRAW, LK_COMPUTE, LK_INDIRECT, LK_FIXCOPY, LK_OTHER, LK_COUNT };
static const char* const kListKindName[LK_COUNT] = {
    "draw+sysmemVB", "draw", "compute", "indirect", "our copy", "other/unknown"
};
struct KindAgg { uint64_t lists, draws, vbSys, vbAll, dispatches; double ms; };
struct SlowList { double ms; uint32_t ecl, pos; TsSeg seg; };
static constexpr int kSlowN = 6;
struct GpuAgg {                     // guarded by g_fixMutex; taken by the stats thread
    uint64_t timed = 0, skipped = 0, anomalies = 0;
    double busyMs = 0, starvedMs = 0, stalledMs = 0, latSumMs = 0, latMaxMs = 0;
    KindAgg kind[LK_COUNT] = {};
    SlowList slow[kSlowN] = {};
    int nSlow = 0;
};
static GpuAgg g_gpuAgg;

// Per DIRECT queue: our command lists, a pool of allocators and a fence.
struct FixAlloc { ID3D12CommandAllocator* alloc; uint64_t fence; };
struct FixQueue {
    ID3D12CommandQueue* q = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    uint64_t lastSignaled = 0;
    ID3D12GraphicsCommandList* list = nullptr;  // copy list
    std::vector<FixAlloc> allocs;
    std::vector<ListRange> scratch;
    std::vector<HybRange> hscratch;
    std::vector<HybRange> hframe;   // stage 2d: exact ranges referenced by earlier submissions of this frame (stats)
    uint64_t hframeNo = ~0ull;
    // Stage 2d with [Fix] HybridCopyQueue: our COPY queue; game submissions wait for its fence.
    ID3D12CommandQueue* cq = nullptr;
    ID3D12Fence* cfence = nullptr;
    uint64_t cSignaled = 0;
    ID3D12GraphicsCommandList* clist = nullptr;
    std::vector<FixAlloc> callocs;
    std::vector<D3D12_RESOURCE_BARRIER> barriers;
    std::vector<ID3D12CommandList*> submit;
    // GPU timing
    bool timing = false;
    ID3D12QueryHeap* qheap = nullptr;
    ID3D12Resource* rb = nullptr;
    const uint64_t* rbPtr = nullptr;
    ID3D12CommandAllocator* tsAlloc = nullptr;
    ID3D12GraphicsCommandList* tsList[kTsBanks][kTsPerBank] = {};
    ID3D12GraphicsCommandList* post = nullptr;
    TsSlot slots[kTsSlots];
    uint32_t nextSlot = 0, nextBank = 0;
    double gpuToQpc = 0;
    int64_t calGpu = 0, calCpu = 0, calAt = 0, prevEnd = 0;
    uint64_t lastFrame = ~0ull;
    uint32_t eclInFrame = 0;
};
static std::vector<FixQueue*> g_fixQueues; // guarded by g_fixMutex

// Our objects the GPU may still use (twins of dead pages / buffers, retired heap
// mirrors), released once every queue passed the value it had signaled when
// they were retired (guarded by g_fixMutex). what == nullptr: not logged.
struct PendingRelease { IUnknown* obj; uint64_t id; const char* what; std::vector<std::pair<FixQueue*, uint64_t>> waits; };
static std::vector<PendingRelease> g_pendingRel;

static void DeferRelease(IUnknown* obj, uint64_t id, const char* what) // caller holds g_fixMutex
{
    PendingRelease p = { obj, id, what, {} };
    for (FixQueue* fq : g_fixQueues) p.waits.push_back({ fq, fq->lastSignaled });
    g_pendingRel.push_back(std::move(p));
}

static void FixOnPageDestroyed(ResRec* r)
{
    std::lock_guard<std::mutex> lk(g_fixMutex);
    if (r->twinDead) return;
    r->twinDead = true;
    DeferRelease(r->twin, r->id, "twin of dead UPLOAD");
    g_fixTwinsLive.fetch_sub(1, std::memory_order_relaxed);
}

static void ReleasePending() // caller holds g_fixMutex
{
    for (size_t i = 0; i < g_pendingRel.size();) {
        PendingRelease& p = g_pendingRel[i];
        bool done = true;
        for (auto& w : p.waits) done = done && w.first->fence->GetCompletedValue() >= w.second;
        if (done) {
            p.obj->Release();
            if (p.what) Log("fix: released %s #%llu", p.what, p.id);
            g_pendingRel[i] = std::move(g_pendingRel.back());
            g_pendingRel.pop_back();
        } else {
            ++i;
        }
    }
}

// ============================================================================
// Stage 2c: UPLOAD memory in CPU-visible VRAM (NVAPI)
// ============================================================================
// Without ReBAR the CPU still sees a 256 MB window of VRAM (BAR1). Since R465
// NVAPI can put UPLOAD heaps / committed UPLOAD resources there:
//   NvAPI_D3D12_CreateHeap2 + NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM
//   NvAPI_D3D12_CreateCommittedResource + NV_D3D12_RESOURCE_FLAG_CPUVISIBLE_VIDMEM
// The GPU then reads VRAM instead of system memory over Thunderbolt; CPU writes
// go through BAR1 (write-combined, posted). CPU reads from it are very slow.
//   [Fix] VidmemHeaps=1   the game's UPLOAD heaps (its per-frame VB/SRV buffers
//                         are placed in a 32 MB one)
//   [Fix] VidmemBuffers=1 committed UPLOAD buffers up to VidmemBufMaxKB
//   [Fix] VidmemReserveMB of the window is always left free.
// The flag is only a hint, so each creation compares the window's free bytes
// before/after; only memory that really landed there is recorded as UPLOAD_VID.
// NVAPI is called through nvapi_QueryInterface with the IDs from the public
// SDK (github.com/NVIDIA/nvapi, nvapi_interface.h).
namespace nv {
using Status = int; // NvAPI_Status, 0 = NVAPI_OK
struct Params { uint32_t version; uint32_t flags; }; // NV_HEAP_PARAMS_V1 / NV_RESOURCE_PARAMS_V1
constexpr uint32_t kParamsVer = (uint32_t)sizeof(Params) | (1u << 16); // MAKE_NVAPI_VERSION(..., 1)
constexpr uint32_t kHeapFlagCpuVisibleVidmem = 1; // NV_D3D12_HEAP_FLAG_CPUVISIBLE_VIDMEM
constexpr uint32_t kResFlagCpuVisibleVidmem = 2;  // NV_D3D12_RESOURCE_FLAG_CPUVISIBLE_VIDMEM
constexpr uint32_t kIdInitialize = 0x0150e828, kIdCreateHeap2 = 0x924be9d6;
constexpr uint32_t kIdQueryCpuVisibleVidmem = 0x26322bc3, kIdCreateCommittedResource = 0x027e98ae;
using PFN_QueryInterface = void*(__cdecl*)(uint32_t);
using PFN_Initialize = Status(__cdecl*)();
using PFN_CreateHeap2 = Status(__cdecl*)(ID3D12Device*, const D3D12_HEAP_DESC*, const Params*, REFIID, void**);
using PFN_QueryCpuVisibleVidmem = Status(__cdecl*)(ID3D12Device*, uint64_t* total, uint64_t* free);
using PFN_CreateCommittedResource = Status(__cdecl*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                                     const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                                     const D3D12_CLEAR_VALUE*, const Params*, REFIID, void**, bool*);
static PFN_CreateHeap2 CreateHeap2;
static PFN_QueryCpuVisibleVidmem QueryCpuVisibleVidmem;
static PFN_CreateCommittedResource CreateCommittedResource;
} // namespace nv

// Stage 2c runs for one device only: the first one on an NVIDIA GPU without ReBAR.
// The device vtables live in D3D12Core.dll and are shared by all devices, so the
// hooks check the adapter LUID of the calling device (VidDevice()).
static std::atomic<bool> g_vidOn{false};   // set (release) after g_vidLuid / g_vidAdapter / nv:: are ready
static std::atomic<bool> g_hybOn{false};   // stage 2d runs for the device g_vidLuid (set after g_vidLuid)
static const char* g_hybOffReason = "no D3D12 device yet";
static std::atomic<bool> g_shOn{false};    // stage 2e runs for the device g_vidLuid
static const char* g_shOffReason = "no D3D12 device yet";
static LUID g_vidLuid = {};                // the device the fixes run for (2c and/or 2d)
static const char* g_vidOffReason = "no D3D12 device yet"; // for the stats line while g_vidOn is false
static bool g_nvQueryOk = false;           // QueryCpuVisibleVidmem works (budget from the driver)
static IDXGIAdapter3* g_vidAdapter = nullptr; // process memory usage checks (kept for the process lifetime)
// Marks heaps created in CPU-visible VRAM (placed resources inherit UPLOAD_VID).
static const GUID kVidHeapGuid = { 0x8d3e61b2, 0x47c9, 0x4a0e, { 0xb5, 0x1f, 0x6a, 0x92, 0x0c, 0xd4, 0x3e, 0x77 } };
static const GUID kVidTrackGuid = { 0x2b7c94d0, 0x1e5a, 0x4c83, { 0x8f, 0x06, 0xd3, 0x41, 0x9e, 0x7a, 0x52, 0xc8 } };
static std::atomic<uint64_t> g_vidHeaps{0}, g_vidHeapBytes{0}, g_vidBufs{0}, g_vidBufBytes{0};
static std::atomic<uint64_t> g_vidNoRoom{0}, g_vidFailed{0}, g_vidIgnored{0}, g_vidUnverified{0};
static std::atomic<uint64_t> g_vidLiveBytes{0}; // our CPU-visible VRAM allocations still alive
static std::atomic<int> g_vidBufLogged{0};

// Attached to a heap / buffer we placed in CPU-visible VRAM; gives its bytes
// back to g_vidLiveBytes when the object dies.
struct VidTracker final : public IUnknown {
    std::atomic<ULONG> ref{1};
    uint64_t bytes;
    explicit VidTracker(uint64_t b) : bytes(b) { g_vidLiveBytes.fetch_add(b); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref;
        if (r == 0) { g_vidLiveBytes.fetch_sub(bytes); delete this; }
        return r;
    }
};

static bool SameLuid(const LUID& a, const LUID& b) { return a.LowPart == b.LowPart && a.HighPart == b.HighPart; }

// Loads nvapi64.dll and the three functions, once per process.
static bool LoadNvapi()
{
    static int state = 0; // 0 = not tried, 1 = OK, -1 = failed
    if (state) return state > 0;
    state = -1;
    g_vidOffReason = "NVAPI not available";
    wchar_t path[MAX_PATH];
    GetSystemDirectoryW(path, MAX_PATH);
    wcscat_s(path, L"\\nvapi64.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) { Log("vidmem: cannot load nvapi64.dll (error %lu), stage 2c off", GetLastError()); return false; }
    auto qi = (nv::PFN_QueryInterface)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) { Log("vidmem: nvapi_QueryInterface not exported, stage 2c off"); return false; }
    auto init = (nv::PFN_Initialize)qi(nv::kIdInitialize);
    nv::CreateHeap2 = (nv::PFN_CreateHeap2)qi(nv::kIdCreateHeap2);
    nv::QueryCpuVisibleVidmem = (nv::PFN_QueryCpuVisibleVidmem)qi(nv::kIdQueryCpuVisibleVidmem);
    nv::CreateCommittedResource = (nv::PFN_CreateCommittedResource)qi(nv::kIdCreateCommittedResource);
    if (!init || !nv::CreateHeap2 || !nv::CreateCommittedResource) {
        Log("vidmem: NVAPI functions missing (Initialize %p CreateHeap2 %p CreateCommittedResource %p), stage 2c off",
            (void*)init, (void*)nv::CreateHeap2, (void*)nv::CreateCommittedResource);
        return false;
    }
    const nv::Status st = init();
    if (st != 0) { Log("vidmem: NvAPI_Initialize failed: status %d, stage 2c off", st); return false; }
    state = 1;
    return true;
}

// Called for every device the game creates (caller holds g_installMutex).
// Switches stage 2c on for the first device on an NVIDIA GPU without ReBAR;
// everything else is logged once per adapter and left alone.
static void InitVidmem(ID3D12Device* dev)
{
    const bool want2c = g_cfg.vidmemHeaps || g_cfg.vidmemBuffers;
    if (!want2c) g_vidOffReason = "disabled in egpu_fix.ini";
    if (!g_cfg.hybridHeaps) g_hybOffReason = "disabled in egpu_fix.ini";
    if (!g_cfg.shadowHeaps) g_shOffReason = "disabled in egpu_fix.ini";
    if (!want2c && !g_cfg.hybridHeaps && !g_cfg.shadowHeaps) return;
    // Already running for a device (D3D12 returns the same device per adapter).
    if (g_vidOn.load() || g_hybOn.load() || g_shOn.load()) return;
    const LUID luid = dev->GetAdapterLuid();
    static std::vector<LUID> seen;
    for (const LUID& l : seen) if (SameLuid(l, luid)) return;
    seen.push_back(luid);

    // DXGI adapter of this device: vendor, and local / non-local usage tells where an allocation went.
    IDXGIAdapter3* ad = nullptr;
    using PFN_CDF2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
    HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
    auto cdf2 = dxgi ? (PFN_CDF2)GetProcAddress(dxgi, "CreateDXGIFactory2") : nullptr;
    IDXGIFactory4* f = nullptr;
    if (cdf2 && SUCCEEDED(cdf2(0, IID_PPV_ARGS(&f))) && f) {
        f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad));
        f->Release();
    }
    // The device checks gate both stage 2c and stage 2d.
    auto off = [&](const char* reason) {
        if (want2c) g_vidOffReason = reason;
        if (g_cfg.hybridHeaps) g_hybOffReason = reason;
        if (g_cfg.shadowHeaps) g_shOffReason = reason;
        if (ad) ad->Release();
    };
    DXGI_ADAPTER_DESC1 desc = {};
    if (!ad || FAILED(ad->GetDesc1(&desc))) {
        Log("vidmem: cannot query the device's adapter, the fix is off for it");
        off("adapter unknown");
        return;
    }
    if (desc.VendorId != 0x10DE) {
        Log("vidmem: %ls is not an NVIDIA GPU (vendor 0x%04x), the fix is off for it", desc.Description, desc.VendorId);
        off("not an NVIDIA GPU");
        return;
    }
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = {};
    const bool rebar = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))) &&
                       o16.GPUUploadHeapSupported;
    if (rebar && !g_cfg.vidmemWithReBAR) {
        Log("vidmem: %ls has ReBAR (GPUUploadHeapSupported=1), the fix is not needed: off for it"
            " ([Fix] VidmemWithReBAR=1 forces it on)", desc.Description);
        off("ReBAR active, not needed");
        return;
    }
    g_vidLuid = luid;
    if (g_cfg.shadowHeaps) {
        D3D12_FEATURE_DATA_EXISTING_HEAPS eh = {};
        ID3D12Device3* d3 = nullptr;
        const bool ok = SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &eh, sizeof(eh))) && eh.Supported &&
                        SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d3))) && d3;
        if (d3) d3->Release();
        if (ok) {
            Log("shadow: stage 2e ON for %ls: the game's UPLOAD heaps become DEFAULT (VRAM) heaps; Map returns a sysmem"
                " shadow (write-watched), dirty pages are copied at each game ExecuteCommandLists (merge gaps <= %d KB)%s",
                desc.Description, g_cfg.shadowMergeKB,
                g_cfg.shadowNoCopy ? " -- TEST MODE: copies disabled, rendering WILL be wrong" : "");
            g_shOffReason = "";
            g_shOn.store(true, std::memory_order_release);
        } else {
            Log("shadow: stage 2e OFF: existing heaps (OpenExistingHeapFromAddress) not supported by this device");
            g_shOffReason = "existing heaps not supported";
        }
    }
    if (g_cfg.hybridHeaps) {
        Log("hybrid: stage 2d ON for %ls: UPLOAD heaps not placed in CPU-visible VRAM get a DEFAULT mirror"
            " (VB/IB redirects; SRV redirects %s, %d copies per new SRV; HybridCheck %s)%s", desc.Description,
            g_cfg.hybridSrv ? "ON" : "off", g_cfg.hybridSrvCopies, g_cfg.hybridCheck ? "ON" : "off",
            g_cfg.hybridNoCopy ? " -- TEST MODE: copies disabled, rendering WILL be wrong" : "");
        g_hybOffReason = "";
        g_hybOn.store(true, std::memory_order_release);
    }
    if (!want2c) { ad->Release(); return; }
    if (!LoadNvapi()) { ad->Release(); return; }

    uint64_t total = 0, fr = 0;
    const nv::Status qs = nv::QueryCpuVisibleVidmem ? nv::QueryCpuVisibleVidmem(dev, &total, &fr) : -3;
    g_nvQueryOk = qs == 0;
    if (g_nvQueryOk)
        Log("vidmem: NVAPI ready; CPU-visible VRAM total %.1f MB, free %.1f MB; budget = driver free minus reserve %d MB",
            MB(total), MB(fr), g_cfg.vidmemReserveMB);
    else
        Log("vidmem: NVAPI ready; QueryCpuVisibleVidmem not available (status %d); budget = own cap %d MB",
            qs, g_cfg.vidmemCapMB);
    Log("vidmem: stage 2c ON for %ls (ReBAR %s): heaps %s, committed buffers %s (<= %d KB)",
        desc.Description, rebar ? "on, forced by VidmemWithReBAR" : "off",
        g_cfg.vidmemHeaps ? "ON" : "off", g_cfg.vidmemBuffers ? "ON" : "off", g_cfg.vidmemBufMaxKB);
    g_vidAdapter = ad; // kept for the process lifetime
    g_vidOffReason = "";
    g_vidOn.store(true, std::memory_order_release);
}

// Stage 2c applies to this device (one of the shared-vtable hooks is calling).
static bool VidDevice(ID3D12Device* d)
{
    return g_vidOn.load(std::memory_order_acquire) && SameLuid(d->GetAdapterLuid(), g_vidLuid);
}

// Stage 2d applies to this device.
static bool HybDevice(ID3D12Device* d)
{
    return g_hybOn.load(std::memory_order_acquire) && SameLuid(d->GetAdapterLuid(), g_vidLuid);
}

struct VidSnap { bool nvOk, dxOk; uint64_t nvFree, local, nonLocal; };
static VidSnap TakeVidSnap(ID3D12Device* d)
{
    VidSnap s = {};
    uint64_t total = 0;
    if (g_nvQueryOk) s.nvOk = nv::QueryCpuVisibleVidmem(d, &total, &s.nvFree) == 0;
    if (g_vidAdapter) {
        DXGI_QUERY_VIDEO_MEMORY_INFO l = {}, n = {};
        s.dxOk = SUCCEEDED(g_vidAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &l)) &&
                 SUCCEEDED(g_vidAdapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &n));
        s.local = l.CurrentUsage; s.nonLocal = n.CurrentUsage;
    }
    return s;
}

// Live bytes at the moment the driver first put a "CPU-visible VRAM" allocation
// in system memory instead: the real usable window (0 = not hit yet). It can be
// smaller than VidmemCapMB (160 MB in a game session on the test system, 192 MB
// in smoke.exe probe), so from then on it is the cap.
static std::atomic<uint64_t> g_vidWindowFull{0};

static bool VidRoom(const VidSnap& s, uint64_t size)
{
    const uint64_t live = g_vidLiveBytes.load(), full = g_vidWindowFull.load();
    if (full && live + size > full) return false;
    if (s.nvOk) return s.nvFree >= size + ((uint64_t)g_cfg.vidmemReserveMB << 20);
    return live + size <= ((uint64_t)g_cfg.vidmemCapMB << 20);
}

// The driver ignored the CPU-visible VRAM hint (window full). The caller
// releases the object and creates a normal one, exactly as without the fix.
static void OnWindowFull(const char* what, uint64_t size, const char* why)
{
    g_vidIgnored.fetch_add(1, std::memory_order_relaxed);
    const uint64_t live = g_vidLiveBytes.load();
    const uint64_t mark = live ? live : 1; // 1 = no room at all (window used by others)
    uint64_t prev = g_vidWindowFull.load();
    while ((prev == 0 || mark < prev) && !g_vidWindowFull.compare_exchange_weak(prev, mark)) {}
    Log("vidmem: UPLOAD %s %.2f MB via NVAPI -> SYSMEM (%s): the CPU-visible VRAM window is full at %.1f MB of ours."
        " Using a normal %s instead, as without the fix; cap is now %.1f MB",
        what, MB(size), why, MB(live), what, MB(g_vidWindowFull.load()));
}

// Where did an allocation of `size` go? 1 = VRAM, 0 = sysmem, -1 = can't tell
// (e.g. other threads allocating at the same time). desc gets the evidence.
static int VidVerdict(const VidSnap& a, const VidSnap& b, uint64_t size, char* desc, size_t descSize)
{
    const int64_t half = (int64_t)((size < 65536 ? 65536 : size) / 2);
    int v = -1;
    desc[0] = 0;
    if (a.dxOk && b.dxOk) {
        const int64_t dl = (int64_t)(b.local - a.local), dn = (int64_t)(b.nonLocal - a.nonLocal);
        _snprintf_s(desc, descSize, _TRUNCATE, "process VRAM %+.2f MB, sysmem %+.2f MB", dl / 1048576.0, dn / 1048576.0);
        if (dl >= half && dn < half) v = 1;
        else if (dn >= half && dl < half) v = 0;
    }
    if (a.nvOk && b.nvOk) {
        const size_t n = strlen(desc);
        _snprintf_s(desc + n, descSize - n, _TRUNCATE, "%sCPU-visible free %.1f -> %.1f MB", n ? ", " : "",
                    MB(a.nvFree), MB(b.nvFree));
        if (v < 0) v = (int64_t)(a.nvFree - b.nvFree) >= half ? 1 : 0;
    }
    return v;
}

static bool IsVidHeap(ID3D12Heap* heap)
{
    uint32_t v = 0;
    UINT sz = sizeof(v);
    return SUCCEEDED(heap->GetPrivateData(kVidHeapGuid, &sz, &v)) && sz == sizeof(v) && v == 1;
}

static void AttachVidTracker(void* obj, uint64_t bytes)
{
    ID3D12Object* o = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(obj)->QueryInterface(IID_PPV_ARGS(&o))) && o) {
        VidTracker* t = new VidTracker(bytes);
        o->SetPrivateDataInterface(kVidTrackGuid, t);
        t->Release();
        o->Release();
    }
}

static const char* VerdictText(int v)
{
    return v > 0 ? "VRAM" : v == 0 ? "SYSMEM: driver ignored the hint" : "unverified, assuming VRAM";
}

// The reliable placement test is the CPU write speed. Writes through BAR1 over
// Thunderbolt run at ~0.8 GB/s; once the window is full (~192 MB here) the
// driver silently backs new allocations with cached system memory (~40 GB/s),
// while DXGI still counts them as local (see smoke.exe probe). Returns MB/s.
static constexpr double kBarMaxMBs = 5000; // faster than this can't be BAR1 over TB
static double CpuWriteSpeed(void* mapped, size_t bytes)
{
    double best = 0;
    for (int i = 0; i < 2; ++i) {
        const int64_t t0 = Qpc();
        memset(mapped, 0, bytes);
        const double s = QpcMs(Qpc() - t0) / 1000.0;
        const double mbs = s > 0 ? bytes / 1048576.0 / s : 1e9;
        if (mbs > best) best = mbs;
    }
    return best;
}

// Write speed of a fresh heap, through a temporary buffer placed at offset 0
// (contents of a new heap are undefined anyway). 0 = could not test.
static double HeapWriteSpeed(ID3D12Device* d, void* heapObj, const D3D12_HEAP_DESC* hd)
{
    if (hd->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) return 0;
    ID3D12Heap* heap = nullptr;
    if (FAILED(static_cast<IUnknown*>(heapObj)->QueryInterface(IID_PPV_ARGS(&heap))) || !heap) return 0;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = hd->SizeInBytes < (512u << 10) ? hd->SizeInBytes : (512u << 10);
    rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    double mbs = 0;
    if (SUCCEEDED(d->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r))) && r) {
        void* m = nullptr;
        if (SUCCEEDED(r->Map(0, nullptr, &m)) && m) {
            mbs = CpuWriteSpeed(m, (size_t)rd.Width);
            r->Unmap(0, nullptr);
        }
        r->Release();
    }
    heap->Release();
    return mbs;
}

static double BufferWriteSpeed(void* resObj, uint64_t width)
{
    ID3D12Resource* r = nullptr;
    if (FAILED(static_cast<IUnknown*>(resObj)->QueryInterface(IID_PPV_ARGS(&r))) || !r) return 0;
    double mbs = 0;
    void* m = nullptr;
    if (SUCCEEDED(r->Map(0, nullptr, &m)) && m) {
        mbs = CpuWriteSpeed(m, (size_t)(width < (256u << 10) ? width : (256u << 10)));
        r->Unmap(0, nullptr);
    }
    r->Release();
    return mbs;
}

// Final verdict: write speed if measured, else the DXGI/NVAPI evidence.
static int SpeedVerdict(double mbs, int fallback, char* why, size_t whySize)
{
    if (mbs <= 0) return fallback;
    const size_t n = strlen(why);
    _snprintf_s(why + n, whySize - n, _TRUNCATE, "%sCPU write %.0f MB/s", n ? ", " : "", mbs);
    return mbs < kBarMaxMBs ? 1 : 0;
}

// Creates an UPLOAD heap in CPU-visible VRAM. Returns false (nothing created)
// when it does not apply or fails; the caller then creates a normal heap.
// *inVid: the memory is in VRAM (or could not be verified and is trusted).
static bool TryVidHeap(ID3D12Device* d, const D3D12_HEAP_DESC* hd, REFIID riid, void** ppv, bool* inVid)
{
    if (!g_cfg.vidmemHeaps || !hd || !ppv || hd->Properties.Type != D3D12_HEAP_TYPE_UPLOAD || !VidDevice(d))
        return false;
    const VidSnap a = TakeVidSnap(d);
    if (!VidRoom(a, hd->SizeInBytes)) {
        g_vidNoRoom.fetch_add(1, std::memory_order_relaxed);
        Log("vidmem: UPLOAD heap %.2f MB: normal heap in system memory (no room in the window, ours live %.1f MB)",
            MB(hd->SizeInBytes), MB(g_vidLiveBytes.load()));
        return false;
    }
    const nv::Params p = { nv::kParamsVer, nv::kHeapFlagCpuVisibleVidmem };
    void* out = nullptr;
    const nv::Status st = nv::CreateHeap2(d, hd, &p, riid, &out);
    if (st != 0 || !out) {
        g_vidFailed.fetch_add(1, std::memory_order_relaxed);
        Log("vidmem: NvAPI_D3D12_CreateHeap2(%.2f MB) failed: status %d; creating a normal heap", MB(hd->SizeInBytes), st);
        return false;
    }
    const VidSnap b = TakeVidSnap(d);
    char why[200];
    const int v = SpeedVerdict(HeapWriteSpeed(d, out, hd), VidVerdict(a, b, hd->SizeInBytes, why, sizeof(why)),
                               why, sizeof(why));
    if (v == 0) { // not in the window: drop it (nobody has seen it yet), the caller makes a normal heap
        OnWindowFull("heap", hd->SizeInBytes, why);
        static_cast<IUnknown*>(out)->Release();
        return false;
    }
    *inVid = true;
    ID3D12Heap* h = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(out)->QueryInterface(IID_PPV_ARGS(&h))) && h) {
        const uint32_t one = 1;
        h->SetPrivateData(kVidHeapGuid, sizeof(one), &one);
        h->Release();
    }
    AttachVidTracker(out, hd->SizeInBytes);
    g_vidHeaps.fetch_add(1, std::memory_order_relaxed);
    g_vidHeapBytes.fetch_add(hd->SizeInBytes, std::memory_order_relaxed);
    if (v < 0) g_vidUnverified.fetch_add(1, std::memory_order_relaxed);
    Log("vidmem: UPLOAD heap %.2f MB (flags 0x%x) via NVAPI -> %s (%s)", MB(hd->SizeInBytes), (unsigned)hd->Flags,
        VerdictText(v), why);
    *ppv = out;
    return true;
}

// Same for a committed UPLOAD buffer.
static bool TryVidBuffer(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                         const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st0, const D3D12_CLEAR_VALUE* cv,
                         REFIID riid, void** ppv, bool* inVid)
{
    if (!g_cfg.vidmemBuffers || !hp || !desc || !ppv) return false;
    if (hp->Type != D3D12_HEAP_TYPE_UPLOAD || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return false;
    if (desc->Width > (uint64_t)g_cfg.vidmemBufMaxKB * 1024 || !VidDevice(d)) return false;
    const VidSnap a = TakeVidSnap(d);
    if (!VidRoom(a, desc->Width)) {
        if (g_vidNoRoom.fetch_add(1, std::memory_order_relaxed) < 20)
            Log("vidmem: UPLOAD buffer %.2f MB stays in sysmem: no room (ours live %.1f MB)",
                MB(desc->Width), MB(g_vidLiveBytes.load()));
        return false;
    }
    const nv::Params p = { nv::kParamsVer, nv::kResFlagCpuVisibleVidmem };
    void* out = nullptr;
    bool supported = false;
    const nv::Status st = nv::CreateCommittedResource(d, hp, hf, desc, st0, cv, &p, riid, &out, &supported);
    if (st != 0 || !out) {
        if (g_vidFailed.fetch_add(1, std::memory_order_relaxed) < 20)
            Log("vidmem: NvAPI_D3D12_CreateCommittedResource(%.2f MB) failed: status %d; creating a normal buffer",
                MB(desc->Width), st);
        return false;
    }
    const VidSnap b = TakeVidSnap(d);
    char why[200];
    const int v = SpeedVerdict(BufferWriteSpeed(out, desc->Width), VidVerdict(a, b, desc->Width, why, sizeof(why)),
                               why, sizeof(why));
    if (v == 0) { // not in the window: drop it, the caller makes a normal buffer
        OnWindowFull("buffer", desc->Width, why);
        static_cast<IUnknown*>(out)->Release();
        return false;
    }
    *inVid = true;
    AttachVidTracker(out, desc->Width < 65536 ? 65536 : desc->Width);
    g_vidBufs.fetch_add(1, std::memory_order_relaxed);
    g_vidBufBytes.fetch_add(desc->Width, std::memory_order_relaxed);
    if (v < 0) g_vidUnverified.fetch_add(1, std::memory_order_relaxed);
    if (v < 0 || desc->Width >= (1u << 20) || g_vidBufLogged.fetch_add(1, std::memory_order_relaxed) < 30)
        Log("vidmem: UPLOAD buffer %.2f MB via NVAPI -> %s (%s)", MB(desc->Width), VerdictText(v), why);
    *ppv = out;
    return true;
}

// ============================================================================
// Resource creation
// ============================================================================
static void InstallResourceHooks(ID3D12Resource* res);

static thread_local int t_createDepth = 0;
struct CreateGuard {
    bool outer;
    CreateGuard() : outer(t_createDepth++ == 0) {}
    ~CreateGuard() { --t_createDepth; }
};

static void CreateTwin(ID3D12Device* dev, ResRec* r); // stage 2a
static HybHeap* FindHyb(uint64_t va);                 // stage 2d
static bool IsShVheap(const ID3D12Heap* heap);        // stage 2e

// dev: only for committed resources (twin creation), nullptr otherwise.
// vid: committed buffer created in CPU-visible VRAM (stage 2c).
static void OnResourceCreated(ID3D12Device* dev, const char* how, const D3D12_HEAP_PROPERTIES* hp, ID3D12Heap* heap,
                              uint64_t heapOffset, const D3D12_RESOURCE_DESC* desc, D3D12_HEAP_FLAGS hf,
                              uint32_t initState, void* pv, bool vid = false)
{
    if (!g_cfg.measure) return; // fix-only mode keeps no buffer records
    if (!pv || !desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
    ID3D12Resource* res = nullptr;
    if (FAILED(static_cast<IUnknown*>(pv)->QueryInterface(IID_PPV_ARGS(&res))) || !res) return;

    HeapCat cat = HC_UNTRACKED;
    if (hp) {
        cat = Classify(*hp);
    } else if (heap) {
        D3D12_HEAP_DESC hd = heap->GetDesc();
        if (IsShVheap(heap)) { // stage 2e: GetDesc says UPLOAD, but the memory is VRAM
            hd.Properties = {};
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        }
        cat = Classify(hd.Properties);
        hf = hd.Flags;
        if (cat == HC_UPLOAD && g_vidOn.load(std::memory_order_relaxed) && IsVidHeap(heap)) cat = HC_UPLOAD_VID;
    }
    if (vid && cat == HC_UPLOAD) cat = HC_UPLOAD_VID;

    ResRec* r = new ResRec;
    r->id = g_nextResId.fetch_add(1, std::memory_order_relaxed);
    r->res = res;
    r->va = res->GetGPUVirtualAddress();
    r->size = desc->Width;
    r->cat = cat;
    r->how = how;
    r->heapOffset = heapOffset;
    r->heapFlags = (uint32_t)hf;
    r->resFlags = (uint32_t)desc->Flags;
    r->initState = initState;
    r->createdFrame = g_frames.load(std::memory_order_relaxed);
    if (heap && cat == HC_UPLOAD && r->va) r->hh = FindHyb(r->va); // stage 2d

    g_bufCreated[cat].fetch_add(1, std::memory_order_relaxed);
    g_bufCreatedBytes[cat].fetch_add(r->size, std::memory_order_relaxed);

    // Stage 2a: the twin must exist before the record is visible to Lookup().
    if (g_cfg.constPages && dev && hp && cat == HC_UPLOAD && r->size == kConstPageSize && r->va)
        CreateTwin(dev, r);

    if (r->va) RegisterRec(r);

    Tracker* tr = new Tracker(r);
    if (SUCCEEDED(res->SetPrivateDataInterface(kTrackerGuid, tr))) tr->armed = true;
    tr->Release();

    if (cat != HC_DEFAULT) {
        // Big sysmem buffers (ring buffers etc.) are always logged, small ones up to the limit.
        const bool big = r->size >= (1u << 20);
        const int n = big ? 0 : g_createLogged.fetch_add(1, std::memory_order_relaxed);
        if (big || n < g_cfg.logCreateLimit) {
            Log("create %s buffer #%llu %s size=%llu (%.2f MB) va=0x%llx heapOff=0x%llx heapFlags=0x%x resFlags=0x%x init=0x%x frame=%llu",
                kCatName[cat], r->id, how, r->size, MB(r->size), r->va, r->heapOffset, r->heapFlags,
                r->resFlags, r->initState, r->createdFrame);
        } else if (n == g_cfg.logCreateLimit) {
            Log("create log limit (%d) reached; small sysmem buffers are only counted from now on", g_cfg.logCreateLimit);
        }
    }

    InstallResourceHooks(res);
    res->Release();
}

static void OnHeapCreated(const char* how, const D3D12_HEAP_DESC* hd, void* pv, bool vid = false)
{
    if (!hd || !pv) return;
    HeapCat c = Classify(hd->Properties);
    if (vid && c == HC_UPLOAD) c = HC_UPLOAD_VID;
    g_heapCreated[c].fetch_add(1, std::memory_order_relaxed);
    g_heapCreatedBytes[c].fetch_add(hd->SizeInBytes, std::memory_order_relaxed);
    if (c != HC_DEFAULT)
        Log("create %s heap (%s) size=%llu (%.2f MB) flags=0x%x align=%llu",
            kCatName[c], how, hd->SizeInBytes, MB(hd->SizeInBytes), (unsigned)hd->Flags, hd->Alignment);
}

// ---- Device detours --------------------------------------------------------------
using PFN_CCR  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                             const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                             const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CCR1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                             const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
                                             const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CCR2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                             const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES,
                                             const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CCR3 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
                                             const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT,
                                             const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*,
                                             UINT32, const DXGI_FORMAT*, REFIID, void**);
using PFN_CPR  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
                                             D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CPR1 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                             D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
using PFN_CPR2 = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
                                             D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32,
                                             const DXGI_FORMAT*, REFIID, void**);
using PFN_CH   = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_HEAP_DESC*, REFIID, void**);
using PFN_CH1  = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device4*, const D3D12_HEAP_DESC*,
                                             ID3D12ProtectedResourceSession*, REFIID, void**);
using PFN_CCBV = void(STDMETHODCALLTYPE*)(ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*,
                                          D3D12_CPU_DESCRIPTOR_HANDLE);
using PFN_CSRV = void(STDMETHODCALLTYPE*)(ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*,
                                          D3D12_CPU_DESCRIPTOR_HANDLE);

static PFN_CCR  o_CCR;  static PFN_CCR1 o_CCR1; static PFN_CCR2 o_CCR2; static PFN_CCR3 o_CCR3;
static PFN_CPR  o_CPR;  static PFN_CPR1 o_CPR1; static PFN_CPR2 o_CPR2;
static PFN_CH   o_CH;   static PFN_CH1  o_CH1;
static PFN_CCBV o_CCBV; static PFN_CSRV o_CSRV;

// ============================================================================
// Stage 2d: DEFAULT-heap mirrors of the game's UPLOAD heaps ("hybrid")
// ============================================================================
// Stage 2c puts UPLOAD heaps into the 256 MB CPU-visible VRAM window: the GPU
// reads them fast, but CPU writes crawl (~0.8 GB/s over TB4) and only 5-6
// heaps fit. Here an UPLOAD heap stays in system memory (fast CPU writes) and
// gets a DEFAULT heap of the same size (VRAM, no size limit); the GPU reads
// the mirror:
//   * In both heaps we place one whole-heap buffer at offset 0: "src" in the
//     game's heap, "dst" in the mirror. A placed buffer's GPU VA is the heap's
//     base VA + offset (checked for every buffer the game places there), so a
//     VA inside the game heap maps to dst VA + (va - src VA).
//   * IASetVertexBuffers / IASetIndexBuffer views into a mirrored heap are
//     redirected (DIRECT lists whose Reset we saw) and the view's byte range
//     is recorded in the list's ListState: exact, the view size is known.
//   * [Fix] HybridSrv: CreateShaderResourceView of a buffer in a mirrored heap
//     gets the dst buffer with FirstElement shifted by the buffer's heap offset
//     (when that is a multiple of the element size), else a twin of the buffer
//     placed at the same offset in the mirror (kept in the buffer's private
//     data, released with it). Descriptors the game copies around carry the
//     redirect. Which submission reads an SRV is unknown, so the view's range
//     is copied at the next HybridSrvCopies game submissions. That is correct
//     if the game writes the data before then and never rewrites it in place
//     while the descriptor is in use; [Fix] HybridCheck measures exactly that.
//   * ExecuteCommandLists on a DIRECT queue (game submissions) prepends our
//     list: the merged ranges are copied src -> dst, then each dst goes
//     COPY_DEST -> GENERIC_READ (buffers decay to COMMON after every ECL).
// Safety: the game's heap stays intact, so whatever we don't redirect (lists
// whose Reset we missed, bundles, ExecuteIndirect arguments, root CBV/SRV,
// other queues) still reads correct data from system memory. The only risk is
// a stale mirror under an SRV. The game reuses heap offsets only after its own
// fences and our copies run on the same queue before its lists, so earlier
// submissions are done with old data at an offset before we overwrite it
// (over-copied neighbour bytes are the current system-memory bytes anyway).
static constexpr int kMaxHybHeaps = 64;

struct HybHeap {
    uint64_t id = 0;
    ID3D12Heap* game = nullptr;      // the game's heap; we hold one public reference to see when the game lets go
    ID3D12Heap* mirror = nullptr;    // our DEFAULT heap of the same size
    ID3D12Resource* src = nullptr;   // whole-heap buffer at offset 0 of the game heap (copy source)
    ID3D12Resource* dst = nullptr;   // whole-heap buffer at offset 0 of the mirror (copy target, SRV alias)
    uint64_t size = 0, srcVA = 0, dstVA = 0;
    const uint8_t* cpu = nullptr;    // src mapped (HybridCheck only; guarded by g_fixMutex)
    std::atomic<bool> bad{false};    // VA layout check failed: no new redirects into this heap
    bool dead = false;               // retired: the game released the heap (g_fixMutex)
};
struct HybTable { int n; HybHeap* h[kMaxHybHeaps]; };
static std::atomic<const HybTable*> g_hybTable{nullptr}; // immutable snapshots (old ones are leaked: tiny, rare)
static std::mutex g_hybTableMutex;                       // writers
static std::atomic<bool> g_hybRedirect{true};            // cleared if the copy path ever fails
static std::atomic<uint64_t> g_hybHeaps{0}, g_hybLiveBytes{0}, g_hybFailed{0}, g_hybRetired{0}, g_hybBadLayout{0};
static std::atomic<uint64_t> g_hybInj{0}, g_hybRegions{0}, g_hybBytes{0}, g_hybSrvQueued{0}, g_hybSrvQueuedBytes{0};
static std::atomic<uint64_t> g_hybTwinsLive{0}, g_hybTwinsTotal{0}, g_hybTwinFail{0}, g_hybPlaced{0};
// Bytes the submitted lists / new SRVs referenced (exact union per submission), and the part of it
// already referenced by an earlier submission of the same frame.
static std::atomic<uint64_t> g_hybUnionBytes{0}, g_hybRecopyBytes{0}, g_hybAsync{0};

static inline HybHeap* FindHyb(const HybTable* t, uint64_t va)
{
    for (int i = 0; i < t->n; ++i) {
        HybHeap* h = t->h[i];
        if (va - h->srcVA < h->size) return h->bad.load(std::memory_order_relaxed) ? nullptr : h;
    }
    return nullptr;
}
static HybHeap* FindHyb(uint64_t va)
{
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    return t && va ? FindHyb(t, va) : nullptr;
}

static void PublishHybTable(HybHeap* add, HybHeap* remove) // caller holds g_hybTableMutex
{
    const HybTable* cur = g_hybTable.load(std::memory_order_relaxed);
    HybTable* t = new HybTable();
    t->n = 0;
    if (cur)
        for (int i = 0; i < cur->n; ++i)
            if (cur->h[i] != remove) t->h[t->n++] = cur->h[i];
    if (add && t->n < kMaxHybHeaps) t->h[t->n++] = add;
    g_hybTable.store(t, std::memory_order_release);
}

static D3D12_RESOURCE_DESC BufferDesc(uint64_t width)
{
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = width; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

// CreatePlacedResource past our hook (no record, no stats).
static HRESULT PlaceRaw(ID3D12Device* d, ID3D12Heap* heap, uint64_t off, const D3D12_RESOURCE_DESC* rd,
                        D3D12_RESOURCE_STATES st, ID3D12Resource** out)
{
    *out = nullptr;
    return o_CPR ? o_CPR(d, heap, off, rd, st, nullptr, IID_PPV_ARGS(out))
                 : d->CreatePlacedResource(heap, off, rd, st, nullptr, IID_PPV_ARGS(out));
}

// Called after the game created a heap (not one stage 2c put into CPU-visible VRAM).
static void HybOnHeapCreated(ID3D12Device* d, const D3D12_HEAP_DESC* hd, void* pv)
{
    if (!hd || !pv || hd->Properties.Type != D3D12_HEAP_TYPE_UPLOAD || !HybDevice(d)) return;
    if (hd->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) return;
    const uint64_t size = hd->SizeInBytes & ~0xffffull; // whole 64 KB pages (our buffers need them)
    if (!size) return;
    std::lock_guard<std::mutex> lk(g_hybTableMutex);
    const HybTable* cur = g_hybTable.load(std::memory_order_relaxed);
    if (cur && cur->n >= kMaxHybHeaps) {
        static bool warned = false;
        if (!warned) { warned = true; Log("hybrid: %d mirrored heaps, no more mirrors", kMaxHybHeaps); }
        return;
    }
    HybHeap* h = new HybHeap;
    h->size = size;
    HRESULT hr = static_cast<IUnknown*>(pv)->QueryInterface(IID_PPV_ARGS(&h->game));
    D3D12_HEAP_DESC md = {};
    md.SizeInBytes = size;
    md.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    md.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (SUCCEEDED(hr)) hr = o_CH ? o_CH(d, &md, IID_PPV_ARGS(&h->mirror)) : E_FAIL;
    const D3D12_RESOURCE_DESC wd = BufferDesc(size);
    if (SUCCEEDED(hr)) hr = PlaceRaw(d, h->game, 0, &wd, D3D12_RESOURCE_STATE_GENERIC_READ, &h->src);
    if (SUCCEEDED(hr)) hr = PlaceRaw(d, h->mirror, 0, &wd, D3D12_RESOURCE_STATE_COMMON, &h->dst);
    if (SUCCEEDED(hr) && g_cfg.hybridCheck) {
        void* p = nullptr;
        if (SUCCEEDED(h->src->Map(0, nullptr, &p))) h->cpu = (const uint8_t*)p;
    }
    if (FAILED(hr)) {
        g_hybFailed.fetch_add(1, std::memory_order_relaxed);
        Log("hybrid: mirroring UPLOAD heap %.2f MB failed (0x%08lx); it stays in system memory, as without the fix",
            MB(hd->SizeInBytes), hr);
        if (h->dst) h->dst->Release();
        if (h->src) h->src->Release();
        if (h->mirror) h->mirror->Release();
        if (h->game) h->game->Release();
        delete h;
        return;
    }
    h->srcVA = h->src->GetGPUVirtualAddress();
    h->dstVA = h->dst->GetGPUVirtualAddress();
    h->id = g_hybHeaps.fetch_add(1, std::memory_order_relaxed) + 1;
    g_hybLiveBytes.fetch_add(size, std::memory_order_relaxed);
    h->src->SetName(L"egpu_fix mirror source (whole UPLOAD heap)");
    h->dst->SetName(L"egpu_fix mirror (whole heap)");
    h->mirror->SetName(L"egpu_fix UPLOAD heap mirror");
    PublishHybTable(h, nullptr);
    Log("hybrid: UPLOAD heap #%llu %.2f MB (flags 0x%x, align %llu) mirrored: game VA 0x%llx -> DEFAULT VA 0x%llx"
        " (live mirrors %.0f MB)", h->id, MB(size), (unsigned)hd->Flags, hd->Alignment, h->srcVA, h->dstVA,
        MB(g_hybLiveBytes.load()));
}

// The game placed a resource: if in a mirrored heap, check that its VA is heap VA + offset.
static void HybOnPlaced(ID3D12Heap* heap, uint64_t off, const D3D12_RESOURCE_DESC* desc, void* pv)
{
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    if (!t || !heap || !pv) return;
    for (int i = 0; i < t->n; ++i) {
        HybHeap* h = t->h[i];
        if (h->game != heap) continue;
        if (!desc || desc->Dimension != D3D12_RESOURCE_DIMENSION_BUFFER) return;
        ID3D12Resource* r = nullptr;
        if (FAILED(static_cast<IUnknown*>(pv)->QueryInterface(IID_PPV_ARGS(&r))) || !r) return;
        const uint64_t va = r->GetGPUVirtualAddress();
        r->Release();
        g_hybPlaced.fetch_add(1, std::memory_order_relaxed);
        if (va != h->srcVA + off && !h->bad.exchange(true)) {
            g_hybBadLayout.fetch_add(1, std::memory_order_relaxed);
            Log("hybrid: heap #%llu: buffer placed at offset 0x%llx has VA 0x%llx, expected 0x%llx;"
                " no more redirects into this heap", h->id, off, va, h->srcVA + off);
        }
        return;
    }
}

// The game released a mirrored heap (only our reference is left): stop
// redirecting into it, release our objects once the GPU is done. Checked every
// 64 submissions (caller holds g_fixMutex).
static void HybPollRetire()
{
    static uint32_t tick = 0;
    if (++tick % 64) return;
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    if (!t) return;
    for (int i = 0; i < t->n; ++i) {
        HybHeap* h = t->h[i];
        h->game->AddRef();
        if (h->game->Release() != 1) continue;
        {
            std::lock_guard<std::mutex> lk(g_hybTableMutex);
            PublishHybTable(nullptr, h);
        }
        h->dead = true;
        if (h->cpu) { h->src->Unmap(0, nullptr); h->cpu = nullptr; }
        DeferRelease(h->src, h->id, "mirror source buffer of released UPLOAD heap");
        DeferRelease(h->dst, h->id, "mirror buffer of released UPLOAD heap");
        DeferRelease(h->mirror, h->id, "DEFAULT mirror of released UPLOAD heap");
        h->game->Release(); // the heap itself dies with src (placed resources keep their heap alive internally)
        g_hybRetired.fetch_add(1, std::memory_order_relaxed);
        g_hybLiveBytes.fetch_sub(h->size, std::memory_order_relaxed);
        Log("hybrid: the game released UPLOAD heap #%llu; its mirror goes once the GPU is done", h->id);
    }
}

// ============================================================================
// Stage 2e: shadow heaps (UPLOAD heaps that live in VRAM)
// ============================================================================
// The game's UPLOAD heap becomes a DEFAULT heap in VRAM ("vheap", what the game
// gets), so every GPU read of its buffers (VB/IB/SRV/CBV/indirect/copies, any
// queue) hits VRAM and no redirects are needed. The CPU side lives in a
// system-memory "shadow" (VirtualAlloc with MEM_WRITE_WATCH):
//   * ID3D12Resource::Map / Unmap of a buffer inside a vheap are answered by us
//     (D3D12 refuses Map on DEFAULT heaps): shadow + (VA - heap base VA).
//   * The shadow is wrapped as a D3D12 heap (OpenExistingHeapFromAddress) with a
//     whole-heap CROSS_ADAPTER buffer "src"; "dst" is the whole-heap buffer at
//     offset 0 of the vheap. A placed buffer's GPU VA is heap VA + offset, so
//     a VA in [vaLo, vaLo + size) maps to shadow + (VA - vaLo).
//   * At every game ExecuteCommandLists (any queue) GetWriteWatch tells which
//     pages the CPU wrote; they are copied src -> dst on our own COPY queue and
//     the game's queue waits for that copy (capture -> submit -> Wait happen
//     under one mutex, so every later game submission waits for every earlier
//     capture; the copy carries whatever the CPU wrote before the submission).
//   * GetHeapProperties / Heap::GetDesc report UPLOAD for these objects.
// Heaps are never retired: we keep vheap, dst and the shadow for the process
// lifetime (the game's UPLOAD heap pool only grows). Placed resources may
// outlive the heap and still be mapped into the shadow, and a released VA
// range could be reused by an unrelated buffer that Map must not redirect.
static constexpr int kMaxShHeaps = 256;

struct ShHeap {
    uint64_t id = 0;
    ID3D12Heap* vheap = nullptr;     // DEFAULT heap the game got; we hold one reference for the process lifetime
    ID3D12Heap* sheap = nullptr;     // the shadow memory as a D3D12 heap
    ID3D12Resource* src = nullptr;   // whole-heap CROSS_ADAPTER buffer in sheap (copy source)
    ID3D12Resource* dst = nullptr;   // whole-heap buffer at offset 0 of vheap (copy target)
    uint8_t* shadow = nullptr;
    uint64_t size = 0, vaLo = 0;
    std::vector<void*> pages;        // GetWriteWatch output (g_shMutex)
    // Next sync copies the whole heap (g_shMutex): only after a failed copy submission. Not at
    // creation: the DEFAULT heap and the VirtualAlloc'ed shadow both start zeroed.
    bool needFull = false;
    bool released = false;           // the game dropped its references (kept anyway; g_shMutex)
    bool hot = true;                 // had dirty pages at its last scan (the stream thread scans only these; g_shMutex)
};
struct ShTable { int n; ShHeap* h[kMaxShHeaps]; };
static std::atomic<const ShTable*> g_shTable{nullptr}; // immutable snapshots (old ones are leaked: tiny, rare)
static std::mutex g_shTableMutex;                      // writers of the table / heap creation
static std::mutex g_shMutex;                           // sync: capture -> submit -> wait, g_sh
static std::atomic<uint64_t> g_shHeaps{0}, g_shLiveBytes{0}, g_shFailed{0};
static std::atomic<uint64_t> g_shSyncs{0}, g_shSyncsCopy{0}, g_shDirtyBytes{0}, g_shRegions{0}, g_shCopyBytes{0};
static std::atomic<uint64_t> g_shGwwTicks{0}, g_shWaits{0}, g_shCopyFail{0}, g_shWaitFail{0}, g_shReleased{0};
static std::atomic<ID3D12Device*> g_shDev{nullptr};  // the device our COPY queue belongs to

struct ShCtx {                                         // guarded by g_shMutex
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* cq = nullptr;                  // our COPY queue
    ID3D12Fence* fence = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    HANDLE ev = nullptr;
    std::vector<FixAlloc> allocs;
    bool tried = false;
};
static ShCtx g_sh;
static uint64_t g_shValue = 0;                         // last value signaled on g_sh.fence (g_shMutex)

static bool ShDevice(ID3D12Device* d)
{
    return g_shOn.load(std::memory_order_acquire) && SameLuid(d->GetAdapterLuid(), g_vidLuid);
}

// The ShHeap whose vheap range holds va; nullptr if none.
static inline ShHeap* FindSh(uint64_t va)
{
    const ShTable* t = g_shTable.load(std::memory_order_acquire);
    if (!t || !va) return nullptr;
    for (int i = 0; i < t->n; ++i)
        if (va - t->h[i]->vaLo < t->h[i]->size) return t->h[i];
    return nullptr;
}
static bool IsShVheap(const ID3D12Heap* heap)
{
    const ShTable* t = g_shTable.load(std::memory_order_acquire);
    if (!t) return false;
    for (int i = 0; i < t->n; ++i)
        if (t->h[i]->vheap == heap) return true;
    return false;
}

static void PublishShTable(ShHeap* add, ShHeap* remove = nullptr) // caller holds g_shTableMutex
{
    const ShTable* cur = g_shTable.load(std::memory_order_relaxed);
    ShTable* t = new ShTable();
    t->n = 0;
    if (cur)
        for (int i = 0; i < cur->n; ++i)
            if (cur->h[i] != remove) t->h[t->n++] = cur->h[i];
    if (add && t->n < kMaxShHeaps) t->h[t->n++] = add;
    g_shTable.store(t, std::memory_order_release);
}

// Our COPY queue, fence and list (once). False: stage 2e cannot work.
static bool ShEnsureCtx(ID3D12Device* d)
{
    std::lock_guard<std::mutex> lk(g_shMutex);
    ShCtx& c = g_sh;
    if (c.cq) return true;
    if (c.tried) return false;
    c.tried = true;
    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ID3D12CommandAllocator* a = nullptr;
    HRESULT hr = d->CreateCommandQueue(&qd, IID_PPV_ARGS(&c.cq));
    if (SUCCEEDED(hr)) hr = d->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c.fence));
    if (SUCCEEDED(hr)) hr = d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&a));
    if (SUCCEEDED(hr)) hr = d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, a, nullptr, IID_PPV_ARGS(&c.list));
    if (SUCCEEDED(hr)) hr = c.list->Close();
    c.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !c.ev) {
        Log("shadow: cannot create the COPY queue / fence / list (0x%08lx): stage 2e cannot copy, heaps stay as the game made them", hr);
        if (a) a->Release();
        if (c.list) c.list->Release();
        if (c.fence) c.fence->Release();
        if (c.cq) c.cq->Release();
        c.list = nullptr; c.fence = nullptr; c.cq = nullptr;
        return false;
    }
    c.dev = d;
    g_shDev.store(d, std::memory_order_release);
    d->AddRef(); // process lifetime
    c.allocs.push_back({ a, 0 });
    c.cq->SetName(L"egpu_fix shadow COPY queue");
    Log("shadow: COPY queue %p ready", c.cq);
    return true;
}

// Index of an allocator the GPU is done with (grows the pool or waits); -1 on failure (caller holds g_shMutex).
static int ShAcquireAlloc(ShCtx& c, bool block = true)
{
    const uint64_t done = c.fence->GetCompletedValue();
    size_t oldest = 0;
    for (size_t i = 0; i < c.allocs.size(); ++i) {
        if (c.allocs[i].fence <= done) return (int)i;
        if (c.allocs[i].fence < c.allocs[oldest].fence) oldest = i;
    }
    if (c.allocs.size() < kMaxFixAllocators) {
        ID3D12CommandAllocator* a = nullptr;
        if (SUCCEEDED(c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&a))) && a) {
            c.allocs.push_back({ a, 0 });
            return (int)c.allocs.size() - 1;
        }
    }
    if (!block) return -1;
    const uint64_t want = c.allocs[oldest].fence;
    if (SUCCEEDED(c.fence->SetEventOnCompletion(want, c.ev)) && c.fence->GetCompletedValue() < want)
        WaitForSingleObject(c.ev, 5000);
    return c.fence->GetCompletedValue() >= want ? (int)oldest : -1;
}

static void ShStartStream();

// Game CreateHeap(1) of an UPLOAD heap on the fix's device: returns true when *ppv is the vheap.
static bool TryShadowHeap(ID3D12Device* d, const D3D12_HEAP_DESC* hd, REFIID riid, void** ppv)
{
    if (!g_cfg.shadowHeaps || t_inFix || !hd || !ppv || hd->Properties.Type != D3D12_HEAP_TYPE_UPLOAD || !ShDevice(d))
        return false;
    if (hd->Flags & D3D12_HEAP_FLAG_DENY_BUFFERS) return false;
    const uint64_t size = hd->SizeInBytes;
    if (!size || (size & 0xffff)) {
        static bool warned = false;
        if (!warned) { warned = true; Log("shadow: UPLOAD heap of %llu bytes (not a multiple of 64 KB) stays a normal heap", size); }
        return false;
    }
    std::lock_guard<std::mutex> lk(g_shTableMutex);
    const ShTable* cur = g_shTable.load(std::memory_order_relaxed);
    if (cur && cur->n >= kMaxShHeaps) {
        static bool warned = false;
        if (!warned) { warned = true; Log("shadow: %d heaps, no more shadows", kMaxShHeaps); }
        return false;
    }
    if (!ShEnsureCtx(d)) return false;

    ShHeap* h = new ShHeap;
    h->size = size;
    ID3D12Device3* d3 = nullptr;
    HRESULT hr = d->QueryInterface(IID_PPV_ARGS(&d3));
    const char* step = "QueryInterface(Device3)";
    if (SUCCEEDED(hr)) {
        step = "VirtualAlloc";
        h->shadow = (uint8_t*)VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE);
        if (!h->shadow) hr = HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) { step = "OpenExistingHeapFromAddress"; hr = d3->OpenExistingHeapFromAddress(h->shadow, IID_PPV_ARGS(&h->sheap)); }
    D3D12_RESOURCE_DESC wd = BufferDesc(size);
    if (SUCCEEDED(hr)) {
        step = "placed shadow buffer";
        wd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        hr = PlaceRaw(d, h->sheap, 0, &wd, D3D12_RESOURCE_STATE_COMMON, &h->src);
    }
    D3D12_HEAP_DESC vd = *hd;
    vd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    vd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    vd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    if (SUCCEEDED(hr)) {
        step = "DEFAULT heap";
        hr = o_CH ? o_CH(d, &vd, IID_PPV_ARGS(&h->vheap)) : d->CreateHeap(&vd, IID_PPV_ARGS(&h->vheap));
    }
    if (SUCCEEDED(hr)) {
        step = "placed VRAM buffer";
        wd.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = PlaceRaw(d, h->vheap, 0, &wd, D3D12_RESOURCE_STATE_COMMON, &h->dst);
    }
    if (d3) d3->Release();
    bool published = false;
    if (SUCCEEDED(hr)) {
        h->vaLo = h->dst->GetGPUVirtualAddress();
        h->pages.resize(size / 4096);
        ResetWriteWatch(h->shadow, size);
        h->id = g_shHeaps.fetch_add(1, std::memory_order_relaxed) + 1;
        PublishShTable(h);
        published = true;
        // Self-test: our Map hook must answer for the vheap buffer with exactly the shadow pointer.
        void* mp = nullptr;
        const HRESULT mh = h->dst->Map(0, nullptr, &mp);
        const bool mok = SUCCEEDED(mh) && mp == h->shadow;
        if (SUCCEEDED(mh)) h->dst->Unmap(0, nullptr);
        if (!mok) {
            step = "Map self-test";
            hr = E_FAIL;
            static std::atomic<bool> loud{false};
            if (!loud.exchange(true))
                Log("shadow: !!! SELF-TEST FAILED: Map of a VRAM heap buffer returned 0x%08lx / %p instead of the shadow %p;"
                    " the Map hook is not active, stage 2e heaps are NOT used", mh, mp, (void*)h->shadow);
        }
    }
    if (SUCCEEDED(hr)) { step = "QueryInterface(heap)"; hr = h->vheap->QueryInterface(riid, ppv); }
    if (FAILED(hr)) {
        g_shFailed.fetch_add(1, std::memory_order_relaxed);
        Log("shadow: UPLOAD heap %.2f MB failed at '%s' (0x%08lx); it is created as without stage 2e", MB(size), step, hr);
        if (published) PublishShTable(nullptr, h); // h itself is leaked: a reader may still hold the old table
        if (h->dst) h->dst->Release();
        if (h->vheap) h->vheap->Release();
        if (h->src) h->src->Release();
        if (h->sheap) h->sheap->Release();
        if (!published) {
            if (h->shadow) VirtualFree(h->shadow, 0, MEM_RELEASE); // nothing was ever mapped into it
            delete h;
        }
        return false;
    }
    g_shLiveBytes.fetch_add(size, std::memory_order_relaxed);
    h->src->SetName(L"egpu_fix shadow source (whole heap)");
    h->dst->SetName(L"egpu_fix shadow target (whole heap)");
    ShStartStream();
    Log("shadow: UPLOAD heap #%llu %.2f MB (flags 0x%x, align %llu): shadow %p, VRAM heap VA 0x%llx (live %.0f MB)",
        h->id, MB(size), (unsigned)hd->Flags, hd->Alignment, (void*)h->shadow, h->vaLo, MB(g_shLiveBytes.load()));
    return true;
}

// Scans heap h's write-watch (reset = pages are handed over: they MUST be copied) and appends the dirty
// ranges, merged over gaps <= gap, to rgs. Returns the dirty page count, -1 if GetWriteWatch failed.
struct ShRg { ShHeap* h; uint64_t lo, hi; };
static long long ShScan(ShHeap* h, bool reset, uint64_t gap, std::vector<ShRg>* rgs, uint64_t* dirty)
{
    ULONG_PTR cnt = h->pages.size();
    DWORD gran = 0;
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, h->shadow, h->size, h->pages.data(), &cnt, &gran) != 0 || !gran) {
        static bool warned = false;
        if (!warned) { warned = true; Log("shadow: GetWriteWatch failed (error %lu); copying whole heaps", GetLastError()); }
        return -1;
    }
    *dirty += (uint64_t)cnt * gran;
    if (rgs) {
        const size_t first = rgs->size();
        for (ULONG_PTR k = 0; k < cnt; ++k) {
            const uint64_t off = (uint8_t*)h->pages[k] - h->shadow;
            if (rgs->size() > first && off <= rgs->back().hi + gap) rgs->back().hi = off + gran;
            else rgs->push_back({ h, off, off + gran });
        }
        if (rgs->size() > first && rgs->back().hi > h->size) rgs->back().hi = h->size;
    }
    return (long long)cnt;
}

static std::atomic<uint64_t> g_shBgFlushes{0}, g_shBgCopies{0}, g_shBgBytes{0}, g_shBgCpuTicks{0};
static std::atomic<uint64_t> g_shBgSkipAlloc{0}, g_shBgSkipFlight{0}, g_shBgSkipMin{0};
static std::atomic<bool> g_shStop{false};
static std::atomic<uint64_t> g_shEclHotBytes{0}, g_shEclColdBytes{0}; // ECL-scan dirty bytes in hot / cold heaps

// Copies what the CPU wrote since the last flush (caller holds g_shMutex). background: called by the
// stream thread: scans only heaps that had dirty pages at their last scan, never blocks, skips the round
// when the COPY queue is behind or too little is dirty. The ECL path (background false) scans every heap
// and may wait for an allocator. The allocator is taken BEFORE any write-watch reset: reset pages must be copied.
static void ShFlush(bool background)
{
    ShCtx& c = g_sh;
    const ShTable* t = g_shTable.load(std::memory_order_acquire);
    if (!t || !c.cq) return;
    const int64_t t0 = Qpc();
    const uint64_t gap = (uint64_t)g_cfg.shadowMergeKB << 10;
    std::atomic<uint64_t>& gwwTicks = background ? g_shBgCpuTicks : g_shGwwTicks;
    if (background) {
        g_shBgFlushes.fetch_add(1, std::memory_order_relaxed);
        if (g_shValue - c.fence->GetCompletedValue() >= (uint64_t)g_cfg.shadowStreamMaxInFlight) {
            g_shBgSkipFlight.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    const int ai = ShAcquireAlloc(c, !background);
    if (ai < 0) {
        if (background) g_shBgSkipAlloc.fetch_add(1, std::memory_order_relaxed);
        else g_shCopyFail.fetch_add(1, std::memory_order_relaxed); // pages stay dirty: copied at the next flush
        return;
    }
    static std::vector<ShRg> rgs; // g_shMutex
    rgs.clear();
    uint64_t dirty = 0;
    if (background) { // cheap check without reset: is it worth a round?
        uint64_t peek = 0;
        for (int i = 0; i < t->n; ++i) {
            ShHeap* h = t->h[i];
            if (h->released || (!g_cfg.shadowStreamAll && !h->hot && !h->needFull)) continue;
            const long long n = ShScan(h, false, gap, nullptr, &peek);
            if (n == 0) h->hot = false;
        }
        if (peek < ((uint64_t)g_cfg.shadowStreamMinKB << 10)) {
            g_shBgSkipMin.fetch_add(1, std::memory_order_relaxed);
            g_shBgCpuTicks.fetch_add((uint64_t)(Qpc() - t0), std::memory_order_relaxed);
            return;
        }
    }
    for (int i = 0; i < t->n; ++i) {
        ShHeap* h = t->h[i];
        if (background && (h->released || (!g_cfg.shadowStreamAll && !h->hot && !h->needFull))) continue;
        bool full = h->needFull;
        h->needFull = false;
        if (!full) {
            const uint64_t before = dirty;
            const long long n = ShScan(h, true, gap, &rgs, &dirty);
            if (n < 0) full = true;
            else {
                // Diagnostics: at the ECL, was the dirty data in heaps the stream thread was watching?
                if (!background) (h->hot ? g_shEclHotBytes : g_shEclColdBytes).fetch_add(dirty - before, std::memory_order_relaxed);
                h->hot = n > 0;
            }
        } else {
            ResetWriteWatch(h->shadow, h->size);
        }
        if (full) { rgs.push_back({ h, 0, h->size }); dirty += h->size; h->hot = true; }
    }
    if (!background) gwwTicks.fetch_add((uint64_t)(Qpc() - t0), std::memory_order_relaxed);
    if (g_cfg.shadowNoCopy) rgs.clear(); // negative test: VRAM stays stale
    if (!rgs.empty()) {
        bool ok = false;
        FixAlloc& fa = c.allocs[ai];
        ++t_inFix;
        HRESULT hr = fa.alloc->Reset();
        if (SUCCEEDED(hr)) hr = c.list->Reset(fa.alloc, nullptr);
        if (SUCCEEDED(hr)) {
            for (const ShRg& r : rgs) c.list->CopyBufferRegion(r.h->dst, r.lo, r.h->src, r.lo, r.hi - r.lo);
            D3D12_RESOURCE_BARRIER ab = {}; // our whole-heap dst aliases the game's placed buffers
            ab.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
            c.list->ResourceBarrier(1, &ab);
            hr = c.list->Close();
            if (SUCCEEDED(hr)) {
                ID3D12CommandList* cl = c.list;
                c.cq->ExecuteCommandLists(1, &cl);
                const uint64_t v = g_shValue + 1;
                hr = c.cq->Signal(c.fence, v);
                if (SUCCEEDED(hr)) { g_shValue = v; fa.fence = v; ok = true; }
            }
        }
        --t_inFix;
        if (FAILED(hr)) {
            static std::atomic<int> logged{0};
            if (logged.fetch_add(1, std::memory_order_relaxed) < 5) Log("shadow: copy submission failed (0x%08lx)", hr);
        }
        if (ok) {
            uint64_t cb = 0;
            for (const ShRg& r : rgs) cb += r.hi - r.lo;
            if (!background) g_shSyncsCopy.fetch_add(1, std::memory_order_relaxed);
            else { g_shBgCopies.fetch_add(1, std::memory_order_relaxed); g_shBgBytes.fetch_add(cb, std::memory_order_relaxed); }
            g_shRegions.fetch_add(rgs.size(), std::memory_order_relaxed);
            g_shDirtyBytes.fetch_add(dirty, std::memory_order_relaxed);
            g_shCopyBytes.fetch_add(cb, std::memory_order_relaxed);
        } else {
            g_shCopyFail.fetch_add(1, std::memory_order_relaxed);
            for (const ShRg& r : rgs) r.h->needFull = true; // try everything again at the next flush
        }
    }
    if (background) g_shBgCpuTicks.fetch_add((uint64_t)(Qpc() - t0), std::memory_order_relaxed);
}

// Game submission on any queue: copy what the CPU wrote since the last flush, then make q wait for it.
static void ShadowSync(ID3D12CommandQueue* q)
{
    if (!g_shTable.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lk(g_shMutex);
    ShCtx& c = g_sh;
    if (!c.cq) return;
    const uint64_t n = g_shSyncs.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n % 64 == 0) { // heaps the game let go of: nothing is freed (placed resources may outlive them)
        const ShTable* t = g_shTable.load(std::memory_order_acquire);
        for (int i = 0; t && i < t->n; ++i) {
            ShHeap* h = t->h[i];
            if (h->released) continue;
            h->vheap->AddRef();
            if (h->vheap->Release() != 1) continue;
            h->released = true;
            g_shReleased.fetch_add(1, std::memory_order_relaxed);
            Log("shadow: the game released UPLOAD heap #%llu (%.2f MB); kept, only the ECL sync scans it now", h->id, MB(h->size));
        }
    }
    ShFlush(false);
    if (c.fence->GetCompletedValue() < g_shValue) {
        const HRESULT hr = q->Wait(c.fence, g_shValue);
        g_shWaits.fetch_add(1, std::memory_order_relaxed);
        if (FAILED(hr) && g_shWaitFail.fetch_add(1, std::memory_order_relaxed) < 5)
            Log("shadow: Queue::Wait failed (0x%08lx): the game may read stale VRAM data", hr);
    }
}

// Background streaming thread (ShadowStreamUs > 0): flushes while the game is still writing the frame.
static DWORD WINAPI ShStreamThread(void*)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    if (auto setDesc = (HRESULT(WINAPI*)(HANDLE, PCWSTR))GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"))
        setDesc(GetCurrentThread(), L"egpu_fix shadow stream");
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x2 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    const bool hires = timer != nullptr;
    Log("shadow: stream thread running, period %d us, min %d KB, max in flight %d (%s timer)", g_cfg.shadowStreamUs,
        g_cfg.shadowStreamMinKB, g_cfg.shadowStreamMaxInFlight, hires ? "waitable" : "Sleep(1)");
    while (!g_shStop.load(std::memory_order_relaxed)) {
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)g_cfg.shadowStreamUs * 10; // 100 ns units, relative
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
            else Sleep(1);
        } else {
            Sleep(1);
        }
        if (g_shStop.load(std::memory_order_relaxed)) break;
        std::lock_guard<std::mutex> lk(g_shMutex);
        ShFlush(true);
    }
    return 0;
}
static void ShStartStream()
{
    static std::atomic<bool> started{false};
    if (g_cfg.shadowStreamUs <= 0 || started.exchange(true)) return;
    HANDLE th = CreateThread(nullptr, 0, ShStreamThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    else Log("shadow: cannot start the stream thread (error %lu)", GetLastError());
}

// ---- VB / IB views --------------------------------------------------------------
// Returns the views to pass on: v itself, or tmp with views into mirrored heaps redirected.
static const D3D12_VERTEX_BUFFER_VIEW* HybVB(ID3D12GraphicsCommandList* cl, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v,
                                             D3D12_VERTEX_BUFFER_VIEW* tmp)
{
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    if (!t || t_inFix) return v;
    ListState* ls = nullptr;
    for (UINT k = 0; k < n; ++k) {
        const uint64_t va = v[k].BufferLocation;
        HybHeap* h = va ? FindHyb(t, va) : nullptr;
        if (!h) continue;
        if (!ls) {
            ls = FindListState(cl);
            if (!ls || !ls->open || !ls->direct || !g_hybRedirect.load(std::memory_order_relaxed)) {
                Bump(TS()->misc[M_HYB_SKIP]);
                return v;
            }
            memcpy(tmp, v, n * sizeof(*v));
        }
        const uint64_t lo = va - h->srcVA;
        uint64_t hi = lo + v[k].SizeInBytes;
        if (hi > h->size) hi = h->size;
        if (hi > lo) ls->AddH(h, lo, hi);
        tmp[k].BufferLocation = h->dstVA + lo;
        Bump(TS()->misc[M_HYB_VB]);
    }
    return ls ? tmp : v;
}

// Returns the IB view to pass on (v or *tmp).
static const D3D12_INDEX_BUFFER_VIEW* HybIB(ID3D12GraphicsCommandList* cl, const D3D12_INDEX_BUFFER_VIEW* v,
                                            D3D12_INDEX_BUFFER_VIEW* tmp)
{
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    if (!t || t_inFix || !v->BufferLocation) return v;
    HybHeap* h = FindHyb(t, v->BufferLocation);
    if (!h) return v;
    ListState* ls = FindListState(cl);
    if (!ls || !ls->open || !ls->direct || !g_hybRedirect.load(std::memory_order_relaxed)) {
        Bump(TS()->misc[M_HYB_SKIP]);
        return v;
    }
    const uint64_t lo = v->BufferLocation - h->srcVA;
    uint64_t hi = lo + v->SizeInBytes;
    if (hi > h->size) hi = h->size;
    if (hi > lo) ls->AddH(h, lo, hi);
    *tmp = *v;
    tmp->BufferLocation = h->dstVA + lo;
    Bump(TS()->misc[M_HYB_IB]);
    return tmp;
}

// ---- SRV descriptors ([Fix] HybridSrv) ----------------------------------------------
// {6a0d3c55-91e2-4f7b-a3c8-5b14e9d2f086}
static const GUID kHybTwinGuid = { 0x6a0d3c55, 0x91e2, 0x4f7b, { 0xa3, 0xc8, 0x5b, 0x14, 0xe9, 0xd2, 0xf0, 0x86 } };

// Twins of dead game buffers, pushed lock-free from inside D3D12's destruction of
// the buffer (no locks there); the next submission defers their release.
struct RetiredTwin { ID3D12Resource* twin; RetiredTwin* next; };
static std::atomic<RetiredTwin*> g_hybDeadTwins{nullptr};

static void HybTakeDeadTwins() // caller holds g_fixMutex
{
    RetiredTwin* p = g_hybDeadTwins.exchange(nullptr, std::memory_order_acquire);
    while (p) {
        RetiredTwin* next = p->next;
        DeferRelease(p->twin, 0, nullptr);
        delete p;
        p = next;
    }
}

// Holds the twin of a game buffer (private data of the buffer): released, after
// the GPU is done, when the buffer dies.
struct HybTwin final : public IUnknown {
    std::atomic<ULONG> ref{1};
    ID3D12Resource* twin;
    uint64_t width;
    HybTwin(ID3D12Resource* t, uint64_t w) : twin(t), width(w) { if (t) g_hybTwinsLive.fetch_add(1, std::memory_order_relaxed); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown)) { *ppv = this; AddRef(); return S_OK; }
        *ppv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG r = --ref;
        if (r == 0) {
            if (twin) { // nullptr: marks a buffer whose twin could not be created
                RetiredTwin* e = new RetiredTwin{ twin, g_hybDeadTwins.load(std::memory_order_relaxed) };
                while (!g_hybDeadTwins.compare_exchange_weak(e->next, e, std::memory_order_release, std::memory_order_relaxed)) {}
                g_hybTwinsLive.fetch_sub(1, std::memory_order_relaxed);
            }
            delete this;
        }
        return r;
    }
};
static std::mutex g_hybTwinMutex; // get-or-create of twins

// Twin of a game buffer at the same offset in the mirror (AddRef'd), nullptr on failure.
static ID3D12Resource* HybGetTwin(ID3D12Device* d, ID3D12Resource* res, HybHeap* h, uint64_t off, uint64_t* width)
{
    std::lock_guard<std::mutex> lk(g_hybTwinMutex);
    IUnknown* u = nullptr;
    UINT sz = sizeof(u);
    if (SUCCEEDED(res->GetPrivateData(kHybTwinGuid, &sz, &u)) && u) {
        HybTwin* ht = static_cast<HybTwin*>(u);
        ID3D12Resource* t = ht->twin;
        if (t) t->AddRef();
        *width = ht->width;
        u->Release();
        return t;
    }
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    // GetDesc reports the real alignment (128 for a small tight-aligned buffer), which
    // CreatePlacedResource rejects (E_INVALIDARG); 0 = default for the flags works.
    rd.Alignment = 0;
    ID3D12Resource* t = nullptr;
    const HRESULT hr = PlaceRaw(d, h->mirror, off, &rd, D3D12_RESOURCE_STATE_COMMON, &t);
    if (FAILED(hr) || !t) {
        if (g_hybTwinFail.fetch_add(1, std::memory_order_relaxed) < 10)
            Log("hybrid: twin for a %llu-byte buffer (flags 0x%x) at heap #%llu +0x%llx failed: 0x%08lx; its SRVs stay on sysmem",
                rd.Width, (unsigned)rd.Flags, h->id, off, hr);
        HybTwin* none = new HybTwin(nullptr, rd.Width); // don't try again for this buffer
        res->SetPrivateDataInterface(kHybTwinGuid, none);
        none->Release();
        return nullptr;
    }
    HybTwin* ht = new HybTwin(t, rd.Width);
    const HRESULT ph = res->SetPrivateDataInterface(kHybTwinGuid, ht);
    ht->Release(); // owned by the buffer now (or released right here, which defers t's release)
    if (FAILED(ph)) return nullptr;
    g_hybTwinsTotal.fetch_add(1, std::memory_order_relaxed);
    t->AddRef();
    *width = rd.Width;
    return t;
}

// Exact element size of a typed buffer view format, 0 = unknown.
static uint32_t ExactFormatBytes(DXGI_FORMAT f)
{
    const int v = (int)f;
    if (v >= 1 && v <= 4) return 16;   // R32G32B32A32
    if (v >= 5 && v <= 8) return 12;   // R32G32B32
    if (v >= 9 && v <= 22) return 8;   // R16G16B16A16, R32G32, R32G8X24
    if (v >= 23 && v <= 47) return 4;  // R10G10B10A2 .. R24G8
    if (v >= 48 && v <= 59) return 2;  // R8G8, R16
    if (v >= 60 && v <= 65) return 1;  // R8, A8
    if (v >= 87 && v <= 93) return 4;  // B8G8R8A8 / B8G8R8X8 family
    return 0;
}

// New SRV ranges, copied at the next HybridSrvCopies game submissions.
struct HybSrvPend { HybHeap* h; uint64_t lo, hi; int left; };
static std::mutex g_hybSrvMutex;
static std::vector<HybSrvPend> g_hybSrvPend;

// CreateShaderResourceView on a buffer in a mirrored heap: creates the redirected
// view and returns true; false = caller creates the original view.
static bool HybSrv(ID3D12Device* d, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                   D3D12_CPU_DESCRIPTOR_HANDLE hnd)
{
    const HybTable* t = g_hybTable.load(std::memory_order_acquire);
    if (!t || !t->n) return false;
    const uint64_t va = res->GetGPUVirtualAddress(); // 0 for textures
    HybHeap* h = va ? FindHyb(t, va) : nullptr;
    if (!h) return false;
    if (!desc || desc->ViewDimension != D3D12_SRV_DIMENSION_BUFFER || !g_hybRedirect.load(std::memory_order_relaxed)) {
        Bump(TS()->misc[M_HYB_SRV_SKIP]);
        return false;
    }
    const D3D12_BUFFER_SRV& b = desc->Buffer;
    const uint64_t elem = b.StructureByteStride ? b.StructureByteStride
                        : (b.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : ExactFormatBytes(desc->Format);
    const uint64_t off = va - h->srcVA;
    D3D12_SHADER_RESOURCE_VIEW_DESC nd = *desc;
    ID3D12Resource* twin = nullptr;
    ID3D12Resource* target = h->dst;
    uint64_t lo = 0, hi = 0;
    if (elem && off % elem == 0) {
        nd.Buffer.FirstElement += off / elem;
        Bump(TS()->misc[M_HYB_SRV_ALIAS]);
    } else {
        uint64_t width = 0;
        twin = HybGetTwin(d, res, h, off, &width);
        if (!twin) { Bump(TS()->misc[M_HYB_SRV_SKIP]); return false; }
        target = twin;
        lo = off; hi = off + width; // element size unknown: the whole buffer
        Bump(TS()->misc[M_HYB_SRV_TWIN]);
    }
    if (elem) {
        lo = off + b.FirstElement * elem;
        hi = lo + (uint64_t)b.NumElements * elem;
    }
    if (hi > h->size) hi = h->size;
    if (hi > lo) {
        std::lock_guard<std::mutex> lk(g_hybSrvMutex);
        g_hybSrvPend.push_back({ h, lo, hi, g_cfg.hybridSrvCopies });
        g_hybSrvQueued.fetch_add(1, std::memory_order_relaxed);
        g_hybSrvQueuedBytes.fetch_add(hi - lo, std::memory_order_relaxed);
    }
    if (g_cfg.hybridCheck)
        if (ResRec* r = Lookup(va)) r->hcSrv.store(true, std::memory_order_relaxed);
    o_CSRV(d, target, &nd, hnd);
    if (twin) twin->Release();
    return true;
}

// Moves the pending SRV ranges into rs (caller holds g_fixMutex).
static void HybTakeSrvRanges(std::vector<HybRange>& rs)
{
    std::lock_guard<std::mutex> lk(g_hybSrvMutex);
    size_t o = 0;
    for (size_t i = 0; i < g_hybSrvPend.size(); ++i) {
        HybSrvPend& p = g_hybSrvPend[i];
        rs.push_back({ p.h, p.lo, p.hi });
        if (--p.left > 0) g_hybSrvPend[o++] = p;
    }
    g_hybSrvPend.resize(o);
}

// ---- HybridCheck ([Fix] HybridCheck=1, Measure=1 only) ---------------------------------
// Finds mirrored buffers whose system-memory data changed after their last copy
// although no submitted list referenced them again: exactly what a shader would
// read stale through a redirected SRV. At each copy a buffer gets a sampled hash;
// at every later game submission (and at its death) it is hashed again. Changes
// are counted (by age, SRV or not, size), logged (first ones) and repaired: the
// buffer is copied in this submission. CPU reads of write-combined memory are
// slow, so this costs frame time; it is a diagnostic.
static constexpr uint64_t kHcMaxAgeEcl = 1200; // stop watching a buffer this many submissions after its last copy
enum { HA_NEW, HA_PREV, HA_OLD, HA_COUNT };    // age at the change: created this frame / last frame / older
struct HcStats {
    uint64_t stale[HA_COUNT][2][4];            // [age][has SRV][size bucket]
    uint64_t hashed, copied, submits, deathChecked, deathChanged;
    uint64_t copiedBytes, redundant, redundantBytes; // buffers copied / copied again with unchanged data
    double ms;
};
static HcStats g_hc = {};                      // guarded by g_fixMutex; taken by the stats thread
static std::vector<ResRec*> g_hcList;          // watched buffers (g_fixMutex)
static std::vector<ResRec*> g_hcScratch;
static uint64_t g_hcEcl = 0;
static int g_hcLogged = 0;

static int SizeBucket(uint64_t s) { return s < 4096 ? 0 : s < 65536 ? 1 : s < (1u << 20) ? 2 : 3; }

// Sampled hash of write-combined memory: whole range up to 4 KB, else the first
// and last 64 bytes plus 16 bytes of every KB. Streaming loads (MOVNTDQA) are
// the fast way to read WC memory.
static uint64_t HashMem(const uint8_t* p, uint64_t n)
{
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    const uint64_t n16 = n & ~15ull;
    auto mix = [&](uint64_t o) {
        const __m128i v = _mm_stream_load_si128((__m128i*)(p + o));
        h = (h ^ (uint64_t)_mm_cvtsi128_si64(v)) * 0x100000001b3ull;
        h = (h ^ (uint64_t)_mm_extract_epi64(v, 1)) * 0x100000001b3ull;
        h ^= h >> 29;
    };
    if (n16 <= 4096) {
        for (uint64_t o = 0; o < n16; o += 16) mix(o);
    } else {
        for (uint64_t o = 0; o < 64; o += 16) mix(o);
        for (uint64_t o = 1024; o < n16 - 64; o += 1024) mix(o);
        for (uint64_t o = n16 - 64; o < n16; o += 16) mix(o);
    }
    return h;
}

static inline uint64_t HcHash(const ResRec* r)
{
    const HybHeap* h = r->hh;
    return HashMem(h->cpu + (r->va - h->srcVA), r->size);
}

// At a game submission, before the copies are recorded (caller holds g_fixMutex;
// rs merged). May append repair ranges.
static void HcOnSubmit(std::vector<HybRange>& rs)
{
    const int64_t t0 = Qpc();
    const uint64_t ecl = ++g_hcEcl, frame = g_frames.load(std::memory_order_relaxed);
    ++g_hc.submits;
    // 1) Buffers this submission copies: take a new snapshot.
    g_hcScratch.clear();
    AcquireSRWLockShared(&g_resLock);
    for (const HybRange& e : rs) {
        const HybHeap* h = e.k;
        if (h->dead || !h->cpu) continue;
        const uint64_t vlo = h->srcVA + e.lo, vhi = h->srcVA + e.hi;
        auto it = g_byVa.upper_bound(vlo);
        if (it != g_byVa.begin()) --it;
        for (; it != g_byVa.end() && it->first < vhi; ++it) {
            ResRec* r = it->second;
            if (r->hh == h && r->va + r->size > vlo) g_hcScratch.push_back(r);
        }
    }
    ReleaseSRWLockShared(&g_resLock);
    for (ResRec* r : g_hcScratch) {
        if (r->hcCopyEcl == ecl) continue; // in several ranges
        const uint64_t hs = HcHash(r);
        ++g_hc.hashed; ++g_hc.copied;
        g_hc.copiedBytes += r->size;
        // Copied before and unchanged since: this copy was not needed (dedup potential).
        if (r->hcTracked && hs == r->hcHash) { ++g_hc.redundant; g_hc.redundantBytes += r->size; }
        r->hcHash = hs;
        r->hcCopyEcl = ecl;
        if (!r->hcTracked) { r->hcTracked = true; g_hcList.push_back(r); }
    }
    // 2) Watched buffers not copied now: changed since their last copy?
    bool repaired = false;
    size_t o = 0;
    for (size_t i = 0; i < g_hcList.size(); ++i) {
        ResRec* r = g_hcList[i];
        if (!r->alive.load(std::memory_order_relaxed) || r->hh->dead || ecl - r->hcCopyEcl > kHcMaxAgeEcl) {
            r->hcTracked = false;
            continue;
        }
        g_hcList[o++] = r;
        if (r->hcCopyEcl == ecl) continue;
        const uint64_t hs = HcHash(r);
        ++g_hc.hashed;
        if (hs == r->hcHash) continue;
        const uint64_t age = frame - r->createdFrame, since = ecl - r->hcCopyEcl;
        const bool srv = r->hcSrv.load(std::memory_order_relaxed);
        ++g_hc.stale[age == 0 ? HA_NEW : age == 1 ? HA_PREV : HA_OLD][srv ? 1 : 0][SizeBucket(r->size)];
        ++r->hcChanges;
        if (g_hcLogged < 40) {
            ++g_hcLogged;
            Log("hybrid check: buffer #%llu (%.1f KB, heap #%llu +0x%llx, %s) changed %llu submission(s) after its last copy"
                " without a new reference: age %llu frames, change #%u -> repaired",
                r->id, r->size / 1024.0, r->hh->id, r->va - r->hh->srcVA, srv ? "has a redirected SRV" : "no SRV",
                since, age, r->hcChanges);
        }
        r->hcHash = hs;
        r->hcCopyEcl = ecl;
        rs.push_back({ r->hh, r->va - r->hh->srcVA, r->va - r->hh->srcVA + r->size });
        repaired = true;
    }
    g_hcList.resize(o);
    if (repaired) MergeRanges(rs, (uint64_t)g_cfg.hybridMergeKB << 10);
    g_hc.ms += QpcMs(Qpc() - t0);
}

// A watched buffer dies: did it change after its last copy? (Called inside
// D3D12's destruction of the buffer: never wait for g_fixMutex there.)
static void HcOnDestroyed(ResRec* r)
{
    std::unique_lock<std::mutex> lk(g_fixMutex, std::try_to_lock);
    if (!lk.owns_lock() || !r->hcTracked || r->hh->dead || !r->hh->cpu) return;
    ++g_hc.deathChecked;
    if (HcHash(r) != r->hcHash) ++g_hc.deathChanged;
    r->hcTracked = false; // leaves g_hcList at the next submission (no longer alive)
}

static inline void TimeCreate(const D3D12_HEAP_PROPERTIES* hp, const D3D12_RESOURCE_DESC* d, int64_t t0)
{
    const bool smallUpload = hp && hp->Type == D3D12_HEAP_TYPE_UPLOAD && d &&
                             d->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && d->Width < kConstPageSize;
    AddTime(g_apiTime[smallUpload ? AT_CCR_UPLOAD_SMALL : AT_CCR_OTHER], Qpc() - t0);
}

// Stage 2a: DEFAULT twin for a constant page. Created through the trampoline,
// so our own hook never sees it (no record, no stats).
static void CreateTwin(ID3D12Device* dev, ResRec* r)
{
    if (!o_CCR) return;
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d = {};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = r->size; d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* t = nullptr;
    const HRESULT hr = o_CCR(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
    if (FAILED(hr) || !t) {
        Log("fix: twin creation for UPLOAD #%llu failed: 0x%08lx (page stays on sysmem)", r->id, hr);
        return;
    }
    t->SetName(L"egpu_fix constant page twin");
    r->twin = t;
    r->twinVA = t->GetGPUVirtualAddress();
    g_fixTwinsLive.fetch_add(1, std::memory_order_relaxed);
    const uint64_t n = g_fixTwinsTotal.fetch_add(1, std::memory_order_relaxed) + 1;
    Log("fix: twin #%llu for UPLOAD #%llu va=0x%llx -> DEFAULT va=0x%llx", n, r->id, r->va, r->twinVA);
}

static HRESULT STDMETHODCALLTYPE Hook_CCR(ID3D12Device* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                          const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
                                          const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
{
    CreateGuard g;
    const int64_t t0 = Qpc();
    bool vid = false;
    HRESULT hr = S_OK;
    if (!(g.outer && TryVidBuffer(d, hp, hf, desc, st, cv, riid, ppv, &vid)))
        hr = o_CCR(d, hp, hf, desc, st, cv, riid, ppv);
    if (g.outer) TimeCreate(hp, desc, t0);
    if (g.outer && SUCCEEDED(hr) && ppv)
        OnResourceCreated(d, vid ? "committed(vidmem)" : "committed", hp, nullptr, 0, desc, hf, (uint32_t)st, *ppv, vid);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CCR1(ID3D12Device4* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                           const D3D12_RESOURCE_DESC* desc, D3D12_RESOURCE_STATES st,
                                           const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
                                           REFIID riid, void** ppv)
{
    CreateGuard g;
    const int64_t t0 = Qpc();
    bool vid = false;
    HRESULT hr = S_OK;
    if (!(g.outer && !ps && TryVidBuffer(d, hp, hf, desc, st, cv, riid, ppv, &vid)))
        hr = o_CCR1(d, hp, hf, desc, st, cv, ps, riid, ppv);
    if (g.outer) TimeCreate(hp, desc, t0);
    if (g.outer && SUCCEEDED(hr) && ppv)
        OnResourceCreated(d, vid ? "committed1(vidmem)" : "committed1", hp, nullptr, 0, desc, hf, (uint32_t)st, *ppv, vid);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CCR2(ID3D12Device8* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                           const D3D12_RESOURCE_DESC1* desc, D3D12_RESOURCE_STATES st,
                                           const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
                                           REFIID riid, void** ppv)
{
    CreateGuard g;
    const int64_t t0 = Qpc();
    // D3D12_RESOURCE_DESC1 starts with the exact layout of D3D12_RESOURCE_DESC
    // (the extra SamplerFeedbackMipRegion is unused for buffers).
    const D3D12_RESOURCE_DESC* d0 = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
    bool vid = false;
    HRESULT hr = S_OK;
    if (!(g.outer && !ps && TryVidBuffer(d, hp, hf, d0, st, cv, riid, ppv, &vid)))
        hr = o_CCR2(d, hp, hf, desc, st, cv, ps, riid, ppv);
    if (g.outer) TimeCreate(hp, d0, t0);
    if (g.outer && SUCCEEDED(hr) && ppv)
        OnResourceCreated(d, vid ? "committed2(vidmem)" : "committed2", hp, nullptr, 0, d0, hf, (uint32_t)st, *ppv, vid);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CCR3(ID3D12Device10* d, const D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS hf,
                                           const D3D12_RESOURCE_DESC1* desc, D3D12_BARRIER_LAYOUT layout,
                                           const D3D12_CLEAR_VALUE* cv, ID3D12ProtectedResourceSession* ps,
                                           UINT32 ncf, const DXGI_FORMAT* cf, REFIID riid, void** ppv)
{
    CreateGuard g;
    const int64_t t0 = Qpc();
    HRESULT hr = o_CCR3(d, hp, hf, desc, layout, cv, ps, ncf, cf, riid, ppv);
    if (g.outer) TimeCreate(hp, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), t0);
    if (g.outer && SUCCEEDED(hr) && ppv)
        OnResourceCreated(d, "committed3(layout)", hp, nullptr, 0, reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc), hf, (uint32_t)layout, *ppv);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CPR(ID3D12Device* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC* desc,
                                          D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
{
    CreateGuard g;
    HRESULT hr = o_CPR(d, heap, off, desc, st, cv, riid, ppv);
    if (g.outer && SUCCEEDED(hr) && ppv) {
        HybOnPlaced(heap, off, desc, *ppv);
        OnResourceCreated(nullptr, "placed", nullptr, heap, off, desc, D3D12_HEAP_FLAG_NONE, (uint32_t)st, *ppv);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CPR1(ID3D12Device8* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc,
                                           D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv, REFIID riid, void** ppv)
{
    CreateGuard g;
    HRESULT hr = o_CPR1(d, heap, off, desc, st, cv, riid, ppv);
    if (g.outer && SUCCEEDED(hr) && ppv) {
        const D3D12_RESOURCE_DESC* d0 = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
        HybOnPlaced(heap, off, d0, *ppv);
        OnResourceCreated(nullptr, "placed1", nullptr, heap, off, d0, D3D12_HEAP_FLAG_NONE, (uint32_t)st, *ppv);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CPR2(ID3D12Device10* d, ID3D12Heap* heap, UINT64 off, const D3D12_RESOURCE_DESC1* desc,
                                           D3D12_BARRIER_LAYOUT layout, const D3D12_CLEAR_VALUE* cv, UINT32 ncf,
                                           const DXGI_FORMAT* cf, REFIID riid, void** ppv)
{
    CreateGuard g;
    HRESULT hr = o_CPR2(d, heap, off, desc, layout, cv, ncf, cf, riid, ppv);
    if (g.outer && SUCCEEDED(hr) && ppv) {
        const D3D12_RESOURCE_DESC* d0 = reinterpret_cast<const D3D12_RESOURCE_DESC*>(desc);
        HybOnPlaced(heap, off, d0, *ppv);
        OnResourceCreated(nullptr, "placed2(layout)", nullptr, heap, off, d0, D3D12_HEAP_FLAG_NONE, (uint32_t)layout, *ppv);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CH(ID3D12Device* d, const D3D12_HEAP_DESC* hd, REFIID riid, void** ppv)
{
    CreateGuard g;
    bool vid = false, shadow = false;
    HRESULT hr = S_OK;
    if (g.outer && TryShadowHeap(d, hd, riid, ppv)) shadow = true; // stage 2e first
    else if (!(g.outer && TryVidHeap(d, hd, riid, ppv, &vid)))
        hr = o_CH(d, hd, riid, ppv);
    if (g.outer && SUCCEEDED(hr) && ppv && *ppv) {
        if (!vid && !shadow) HybOnHeapCreated(d, hd, *ppv);
        OnHeapCreated(vid ? "CreateHeap -> CPU-visible VRAM" : shadow ? "CreateHeap -> shadow (VRAM)" : "CreateHeap", hd, *ppv, vid);
    }
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CH1(ID3D12Device4* d, const D3D12_HEAP_DESC* hd, ID3D12ProtectedResourceSession* ps,
                                          REFIID riid, void** ppv)
{
    CreateGuard g;
    bool vid = false, shadow = false;
    HRESULT hr = S_OK;
    if (g.outer && !ps && TryShadowHeap(d, hd, riid, ppv)) shadow = true; // stage 2e first
    else if (!(g.outer && !ps && TryVidHeap(d, hd, riid, ppv, &vid)))
        hr = o_CH1(d, hd, ps, riid, ppv);
    if (g.outer && SUCCEEDED(hr) && ppv && *ppv) {
        if (!vid && !shadow && !ps) HybOnHeapCreated(d, hd, *ppv);
        OnHeapCreated(vid ? "CreateHeap1 -> CPU-visible VRAM" : shadow ? "CreateHeap1 -> shadow (VRAM)" : "CreateHeap1", hd, *ppv, vid);
    }
    return hr;
}
static void STDMETHODCALLTYPE Hook_CCBV(ID3D12Device* d, const D3D12_CONSTANT_BUFFER_VIEW_DESC* desc,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (desc) CountVA(U_CBV_DESC, desc->BufferLocation, desc->SizeInBytes);
    o_CCBV(d, desc, h);
}
static void STDMETHODCALLTYPE Hook_CSRV(ID3D12Device* d, ID3D12Resource* res, const D3D12_SHADER_RESOURCE_VIEW_DESC* desc,
                                        D3D12_CPU_DESCRIPTOR_HANDLE h)
{
    if (g_cfg.measure) {
        if (res && !desc) {
            CountRes(U_SRV_DESC, res, 0, kToEnd);
        } else if (res && desc->ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
            const D3D12_BUFFER_SRV& b = desc->Buffer;
            const uint64_t elem = b.StructureByteStride ? b.StructureByteStride
                                : (b.Flags & D3D12_BUFFER_SRV_FLAG_RAW) ? 4 : FormatBytes(desc->Format);
            CountRes(U_SRV_DESC, res, b.FirstElement * elem, (uint64_t)b.NumElements * elem);
        }
    }
    if (res && g_cfg.hybridSrv && HybSrv(d, res, desc, h)) return; // stage 2d: view created on the mirror
    o_CSRV(d, res, desc, h);
}

// ---- ID3D12Resource::Map -----------------------------------------------------------
using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*, void**);
static std::atomic<bool> g_resHooksInstalled{false};
static std::atomic<void*> g_mapTarget{nullptr};

using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D12Resource*, UINT, const D3D12_RANGE*);
using PFN_GetHeapProps = HRESULT(STDMETHODCALLTYPE*)(ID3D12Resource*, D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS*);
using PFN_HeapGetDesc = D3D12_HEAP_DESC*(STDMETHODCALLTYPE*)(ID3D12Heap*, D3D12_HEAP_DESC*);

// ID3D12Resource methods (one instance per distinct implementation, if the runtime has several).
// Map / Unmap: stage 2e answers them for buffers in a vheap (shadow memory), the rest is the
// Measure=1 Map accounting. GetHeapProperties: stage 2e resources look like UPLOAD ones.
template <int V> struct ResHooks {
    static inline PFN_Map oMap;
    static inline PFN_Unmap oUnmap;
    static inline PFN_GetHeapProps oGhp;
    static HRESULT STDMETHODCALLTYPE Map(ID3D12Resource* r, UINT sub, const D3D12_RANGE* rr, void** pp)
    {
        if (g_shTable.load(std::memory_order_relaxed)) {
            const uint64_t va = r->GetGPUVirtualAddress();
            if (ShHeap* h = FindSh(va)) {
                if (pp) *pp = h->shadow + (va - h->vaLo);
                return S_OK;
            }
        }
        if (!g_cfg.measure) return oMap(r, sub, rr, pp);
        const int64_t t0 = Qpc();
        HRESULT hr = oMap(r, sub, rr, pp);
        AddTime(g_apiTime[AT_MAP], Qpc() - t0);
        Bump(TS()->misc[M_MAP]);
        if (const uint64_t va = r->GetGPUVirtualAddress())
            if (ResRec* rec = Lookup(va)) rec->maps.fetch_add(1, std::memory_order_relaxed);
        return hr;
    }
    static void STDMETHODCALLTYPE Unmap(ID3D12Resource* r, UINT sub, const D3D12_RANGE* rr)
    {
        if (g_shTable.load(std::memory_order_relaxed) && FindSh(r->GetGPUVirtualAddress())) return;
        oUnmap(r, sub, rr);
    }
    static HRESULT STDMETHODCALLTYPE GetHeapProps(ID3D12Resource* r, D3D12_HEAP_PROPERTIES* hp, D3D12_HEAP_FLAGS* hf)
    {
        const HRESULT hr = oGhp(r, hp, hf);
        if (SUCCEEDED(hr) && hp && g_shTable.load(std::memory_order_relaxed) && FindSh(r->GetGPUVirtualAddress())) {
            hp->Type = D3D12_HEAP_TYPE_UPLOAD;
            hp->CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            hp->MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        }
        return hr;
    }
};
static PFN_HeapGetDesc o_HeapGetDesc;
static D3D12_HEAP_DESC* STDMETHODCALLTYPE Hook_HeapGetDesc(ID3D12Heap* h, D3D12_HEAP_DESC* ret)
{
    D3D12_HEAP_DESC* r = o_HeapGetDesc(h, ret);
    if (r && IsShVheap(h)) {
        r->Properties.Type = D3D12_HEAP_TYPE_UPLOAD;
        r->Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        r->Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    }
    return r;
}

// Hooks Map (and, for stage 2e, Unmap / GetHeapProperties) of the resource class with vtable vt;
// ref: vtable of variant 0 (slots with the same implementation are not hooked again).
template <int V> static bool InstallResVT(void** vt, void** ref)
{
    bool ok = true;
    if (!ref || vt[g_vtbl.Resource_Map] != ref[g_vtbl.Resource_Map])
        ok &= HookVT(vt, g_vtbl.Resource_Map, (void*)&ResHooks<V>::Map, (void**)&ResHooks<V>::oMap, "Resource::Map");
    if (!g_cfg.shadowHeaps) return ok;
    if (!ref || vt[g_vtbl.Resource_Unmap] != ref[g_vtbl.Resource_Unmap])
        ok &= HookVT(vt, g_vtbl.Resource_Unmap, (void*)&ResHooks<V>::Unmap, (void**)&ResHooks<V>::oUnmap, "Resource::Unmap");
    if (!ref || vt[g_vtbl.Resource_GetHeapProperties] != ref[g_vtbl.Resource_GetHeapProperties])
        HookVT(vt, g_vtbl.Resource_GetHeapProperties, (void*)&ResHooks<V>::GetHeapProps, (void**)&ResHooks<V>::oGhp,
               "Resource::GetHeapProperties");
    return ok;
}

// Stage 2e: hooks before the game's first Map, from temporary objects: an UPLOAD buffer, a placed
// DEFAULT buffer (may be another class) and a heap (caller holds g_installMutex; hooks are queued).
static void ShHooksFailed()
{
    g_shOn.store(false, std::memory_order_release);
    g_shOffReason = "Map/Unmap hooks could not be installed";
    Log("shadow: stage 2e OFF: the Map/Unmap hooks could not be installed");
}

static bool InstallShadowHooks(ID3D12Device* dev)
{
    bool ok = false;
    D3D12_RESOURCE_DESC rd = BufferDesc(65536);
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* up = nullptr; ID3D12Resource* def = nullptr; ID3D12Heap* heap = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&up));
    D3D12_HEAP_DESC hd = {};
    hd.SizeInBytes = 65536; hd.Alignment = 65536;
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
    if (SUCCEEDED(dev->CreateHeap(&hd, IID_PPV_ARGS(&heap))) && heap)
        dev->CreatePlacedResource(heap, 0, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&def));
    void** v0 = up ? *reinterpret_cast<void***>(up) : nullptr;
    void** v1 = def ? *reinterpret_cast<void***>(def) : nullptr;
    if (v0) ok = InstallResVT<0>(v0, nullptr);
    if (v1 && !InstallResVT<1>(v1, v0)) ok = false;
    if (v0 && !v1) Log("shadow: temporary DEFAULT buffer failed, only the UPLOAD buffer class is hooked");
    if (heap) HookVT(*reinterpret_cast<void***>(heap), g_vtbl.Heap_GetDesc, (void*)&Hook_HeapGetDesc, (void**)&o_HeapGetDesc, "Heap::GetDesc");
    if (v0) {
        g_mapTarget.store(v0[g_vtbl.Resource_Map], std::memory_order_relaxed);
        g_resHooksInstalled.store(true, std::memory_order_release);
    }
    if (up) up->Release();
    if (def) def->Release();
    if (heap) heap->Release();
    return ok;
}

static void InstallResourceHooks(ID3D12Resource* res)
{
    void* target = (*reinterpret_cast<void***>(res))[g_vtbl.Resource_Map];
    if (g_resHooksInstalled.load(std::memory_order_acquire)) {
        // Cheap sanity check: warn once if another resource class has a different Map().
        static std::atomic<bool> warned{false};
        if (target != g_mapTarget.load(std::memory_order_relaxed) && !warned.exchange(true))
            Log("NOTE: a buffer has a different ID3D12Resource::Map implementation (%s); its Map calls are not counted",
                DescribeAddr(target).c_str());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    if (g_resHooksInstalled.load()) return;
    HookTarget(target, (void*)&ResHooks<0>::Map, (void**)&ResHooks<0>::oMap, "Resource::Map");
    ApplyHooks();
    g_mapTarget.store(target, std::memory_order_relaxed);
    g_resHooksInstalled.store(true, std::memory_order_release);
}

// ============================================================================
// Command queues
// ============================================================================
struct QueueStats {
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC desc = {};
    std::atomic<uint64_t> ecl{0}, lists{0};         // game submissions
    std::atomic<uint64_t> eclDxgi{0}, listsDxgi{0}; // DXGI's own, inside Present
    uint64_t snapEcl = 0, snapLists = 0, snapEclDxgi = 0, snapListsDxgi = 0;
    int shDev = -1;           // stage 2e: 1 = queue of the shadow device, 0 = other device, -1 = not known yet
    FixQueue* fix = nullptr;  // stage 2a, guarded by g_fixMutex
    bool fixFailed = false;
};
static std::mutex g_qMutex;
static std::vector<QueueStats*> g_queues;

static QueueStats* OnExecuteCommandLists(ID3D12CommandQueue* q, UINT n)
{
    QueueStats* qs = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        for (QueueStats* s : g_queues) if (s->q == q) { qs = s; break; }
        if (!qs) {
            qs = new QueueStats;
            qs->q = q;
            qs->desc = q->GetDesc();
            g_queues.push_back(qs);
            Log("first ExecuteCommandLists on queue %p: type %s, priority %d, flags 0x%x",
                q, ListTypeName(qs->desc.Type), qs->desc.Priority, (unsigned)qs->desc.Flags);
        }
    }
    const bool dxgi = t_presentDepth > 0;
    if (!dxgi) CloseWindow();
    (dxgi ? qs->eclDxgi : qs->ecl).fetch_add(1, std::memory_order_relaxed);
    (dxgi ? qs->listsDxgi : qs->lists).fetch_add(n, std::memory_order_relaxed);
    return qs;
}

using PFN_ECL = void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// ---- Stage 2a copy + GPU timing injection ------------------------------------------
// GPU timing objects (measurement only; on failure timing is just switched off).
static void SetupTiming(FixQueue* fq, ID3D12Device* dev) // caller holds g_fixMutex
{
    D3D12_QUERY_HEAP_DESC qd = {};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = kTsBanks * kTsPerBank;
    HRESULT hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&fq->qheap));
    UINT64 freq = 0;
    if (SUCCEEDED(hr)) hr = fq->q->GetTimestampFrequency(&freq);
    if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d = {};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = (UINT64)kTsSlots * kTsPerBank * sizeof(uint64_t);
        d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // Through the trampoline: not recorded as a game buffer.
        hr = o_CCR ? o_CCR(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&fq->rb))
                   : E_FAIL;
    }
    if (SUCCEEDED(hr)) {
        void* p = nullptr;
        hr = fq->rb->Map(0, nullptr, &p);
        fq->rbPtr = (const uint64_t*)p;
    }
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&fq->tsAlloc));
    ++t_inFix;
    for (int b = 0; b < kTsBanks && SUCCEEDED(hr); ++b) {
        for (int k = 0; k < kTsPerBank && SUCCEEDED(hr); ++k) {
            ID3D12GraphicsCommandList* cl = nullptr;
            hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fq->tsAlloc, nullptr, IID_PPV_ARGS(&cl));
            if (SUCCEEDED(hr)) {
                cl->EndQuery(fq->qheap, D3D12_QUERY_TYPE_TIMESTAMP, (UINT)(b * kTsPerBank + k));
                hr = cl->Close();
                fq->tsList[b][k] = cl;
            }
        }
    }
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, fq->allocs[0].alloc, nullptr, IID_PPV_ARGS(&fq->post));
    if (SUCCEEDED(hr)) hr = fq->post->Close();
    --t_inFix;
    UINT64 g = 0, c = 0;
    if (SUCCEEDED(hr)) hr = fq->q->GetClockCalibration(&g, &c);
    if (FAILED(hr) || !freq) { Log("timing: setup failed (0x%08lx); GPU timing off", hr); return; } // partial objects leak
    fq->gpuToQpc = (double)g_qpf.QuadPart / (double)freq;
    fq->calGpu = (int64_t)g; fq->calCpu = (int64_t)c; fq->calAt = Qpc();
    fq->timing = true;
    Log("timing: GPU timestamps ready on queue %p (%llu Hz, %d x %d timestamp lists)", fq->q, freq, kTsBanks, kTsPerBank);
}

static FixQueue* CreateFixQueue(ID3D12CommandQueue* q) // caller holds g_fixMutex
{
    ID3D12Device* dev = nullptr;
    if (FAILED(q->GetDevice(IID_PPV_ARGS(&dev))) || !dev) { Log("fix: queue %p: GetDevice failed", q); return nullptr; }
    FixQueue* fq = new FixQueue;
    fq->q = q;
    ID3D12CommandAllocator* a = nullptr;
    HRESULT hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fq->fence));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, a, nullptr, IID_PPV_ARGS(&fq->list));
    if (SUCCEEDED(hr)) {
        ++t_inFix;
        hr = fq->list->Close();
        --t_inFix;
    }
    fq->ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || !fq->ev) {
        Log("fix: queue %p: creating fence/allocator/list failed: 0x%08lx", q, hr);
        if (fq->list) fq->list->Release();
        if (a) a->Release();
        if (fq->fence) fq->fence->Release();
        if (fq->ev) CloseHandle(fq->ev);
        delete fq;
        dev->Release();
        return nullptr;
    }
    fq->list->SetName(L"egpu_fix copy list");
    fq->allocs.push_back({ a, 0 });
    if (g_cfg.gpuTiming) SetupTiming(fq, dev);
    if (g_cfg.hybridCopyQueue) {
        // Stage 2d copies on our own COPY queue; on failure they stay on the game's queue.
        D3D12_COMMAND_QUEUE_DESC cd = {};
        cd.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ID3D12CommandAllocator* ca = nullptr;
        HRESULT ch = dev->CreateCommandQueue(&cd, IID_PPV_ARGS(&fq->cq));
        if (SUCCEEDED(ch)) ch = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fq->cfence));
        if (SUCCEEDED(ch)) ch = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&ca));
        if (SUCCEEDED(ch)) ch = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, ca, nullptr, IID_PPV_ARGS(&fq->clist));
        if (SUCCEEDED(ch)) {
            ++t_inFix;
            ch = fq->clist->Close();
            --t_inFix;
        }
        if (SUCCEEDED(ch)) {
            fq->cq->SetName(L"egpu_fix mirror copy queue");
            fq->clist->SetName(L"egpu_fix mirror copy list (COPY queue)");
            fq->callocs.push_back({ ca, 0 });
            Log("hybrid: mirror copies run on our COPY queue %p; game submissions wait for its fence", fq->cq);
        } else {
            Log("hybrid: creating the COPY queue failed (0x%08lx); mirror copies stay on the game's queue", ch);
            if (fq->clist) fq->clist->Release();
            if (ca) ca->Release();
            if (fq->cfence) fq->cfence->Release();
            if (fq->cq) fq->cq->Release();
            fq->clist = nullptr; fq->cfence = nullptr; fq->cq = nullptr;
        }
    }
    dev->Release();
    g_fixQueues.push_back(fq);
    Log("fix: injection context ready for DIRECT queue %p (list %p)", q, fq->list);
    return fq;
}

// Index of an allocator in pool the GPU is done with (creates or waits if needed); -1 on failure.
static int AcquireAlloc(FixQueue* fq, std::vector<FixAlloc>& pool, ID3D12Fence* fence, D3D12_COMMAND_LIST_TYPE type)
{
    const uint64_t done = fence->GetCompletedValue();
    size_t oldest = 0;
    for (size_t i = 0; i < pool.size(); ++i) {
        if (pool[i].fence <= done) return (int)i;
        if (pool[i].fence < pool[oldest].fence) oldest = i;
    }
    if (pool.size() < kMaxFixAllocators) {
        ID3D12Device* dev = nullptr;
        ID3D12CommandAllocator* a = nullptr;
        if (SUCCEEDED(fq->q->GetDevice(IID_PPV_ARGS(&dev))) && dev) {
            dev->CreateCommandAllocator(type, IID_PPV_ARGS(&a));
            dev->Release();
        }
        if (a) {
            pool.push_back({ a, 0 });
            if (pool.size() == kMaxFixAllocators) Log("fix: %s allocator pool reached %zu", ListTypeName(type), kMaxFixAllocators);
            return (int)pool.size() - 1;
        }
    }
    const uint64_t want = pool[oldest].fence;
    if (SUCCEEDED(fence->SetEventOnCompletion(want, fq->ev)) && fence->GetCompletedValue() < want)
        WaitForSingleObject(fq->ev, 5000);
    return fence->GetCompletedValue() >= want ? (int)oldest : -1;
}
static int AcquireFixAllocator(FixQueue* fq) { return AcquireAlloc(fq, fq->allocs, fq->fence, D3D12_COMMAND_LIST_TYPE_DIRECT); }

static D3D12_RESOURCE_BARRIER CopyDoneBarrier(ID3D12Resource* r, D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter = after;
    return b;
}

// Stage 2d heap -> mirror copies for hranges (merged). barriers == nullptr: a
// COPY queue list, where the mirrors just decay to COMMON after the submission.
static void RecordHybCopies(ID3D12GraphicsCommandList* cl, const std::vector<HybRange>& hranges,
                            std::vector<D3D12_RESOURCE_BARRIER>* barriers, uint64_t& hregions, uint64_t& hbytes)
{
    bool copied = false;
    for (size_t i = 0; i < hranges.size(); ++i) {
        HybHeap* h = hranges[i].k;
        if (h->dead) continue; // the game released the heap: nothing can read it any more
        const uint64_t lo = hranges[i].lo & ~255ull;
        uint64_t hi = (hranges[i].hi + 255) & ~255ull;
        if (hi > h->size) hi = h->size;
        if (hi > lo) {
            cl->CopyBufferRegion(h->dst, lo, h->src, lo, hi - lo);
            ++hregions; hbytes += hi - lo;
            copied = true;
        }
        if (copied && (i + 1 == hranges.size() || hranges[i + 1].k != h)) {
            // GENERIC_READ covers VB/IB, shader resource (SRVs on the whole-heap buffer) and copy source.
            if (barriers) barriers->push_back(CopyDoneBarrier(h->dst, D3D12_RESOURCE_STATE_GENERIC_READ));
            copied = false;
        }
    }
}

// Records our copy list (caller set t_inFix): stage 2a page -> twin copies for
// fq->scratch, stage 2d heap -> mirror copies for fq->hscratch unless they go to
// our COPY queue (hyb false), then one barrier per target (they start in COMMON
// and are promoted to COPY_DEST).
static HRESULT RecordCopies(FixQueue* fq, ID3D12CommandAllocator* alloc, bool hyb, uint64_t& regions, uint64_t& bytes,
                            uint64_t& hregions, uint64_t& hbytes)
{
    std::vector<ListRange>& ranges = fq->scratch;
    MergeRanges(ranges);
    ID3D12GraphicsCommandList* cl = fq->list;
    HRESULT hr = cl->Reset(alloc, nullptr);
    if (FAILED(hr)) return hr;
    fq->barriers.clear();
    for (size_t i = 0; i < ranges.size(); ++i) {
        ResRec* r = ranges[i].k;
        // A dead page here would be a game bug (executing a list that references
        // a released buffer); skip it rather than touch a released twin.
        if (!r->twin || r->twinDead) continue;
        const uint64_t lo = ranges[i].lo & ~255ull;
        uint64_t hi = (ranges[i].hi + 255) & ~255ull;
        if (hi > r->size) hi = r->size;
        cl->CopyBufferRegion(r->twin, lo, r->res, lo, hi - lo);
        ++regions; bytes += hi - lo;
        if (i + 1 == ranges.size() || ranges[i + 1].k != r)
            fq->barriers.push_back(CopyDoneBarrier(r->twin, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER));
    }
    if (hyb) RecordHybCopies(cl, fq->hscratch, &fq->barriers, hregions, hbytes);
    if (!fq->barriers.empty()) cl->ResourceBarrier((UINT)fq->barriers.size(), fq->barriers.data());
    return cl->Close();
}

static void AddTimedSegment(GpuAgg& g, const TsSeg& sg, double ms, uint32_t ecl, uint32_t pos)
{
    int k;
    if (sg.copy) k = LK_FIXCOPY;
    else if (!sg.known) k = LK_OTHER;
    else if (sg.c.draws) k = sg.c.vbSys ? LK_DRAW_SYSVB : LK_DRAW;
    else if (sg.c.dispatches) k = LK_COMPUTE;
    else if (sg.c.execInd) k = LK_INDIRECT;
    else k = LK_OTHER;
    KindAgg& a = g.kind[k];
    ++a.lists; a.ms += ms;
    a.draws += sg.c.draws; a.vbSys += sg.c.vbSys; a.vbAll += sg.c.vbAll; a.dispatches += sg.c.dispatches;
    const SlowList e = { ms, ecl, pos, sg };
    if (g.nSlow < kSlowN) { g.slow[g.nSlow++] = e; return; }
    int mi = 0;
    for (int i = 1; i < kSlowN; ++i) if (g.slow[i].ms < g.slow[mi].ms) mi = i;
    if (ms > g.slow[mi].ms) g.slow[mi] = e;
}

// Reads the timestamps of finished submissions, oldest first (caller holds g_fixMutex).
static void ProcessTimingSlots(FixQueue* fq)
{
    if (Qpc() - fq->calAt > g_qpf.QuadPart) { // recalibrate once a second
        UINT64 g = 0, c = 0;
        if (SUCCEEDED(fq->q->GetClockCalibration(&g, &c))) { fq->calGpu = (int64_t)g; fq->calCpu = (int64_t)c; }
        fq->calAt = Qpc();
    }
    const uint64_t done = fq->fence->GetCompletedValue();
    auto toQpc = [fq](uint64_t t) {
        return fq->calCpu + (int64_t)((double)(int64_t)(t - (uint64_t)fq->calGpu) * fq->gpuToQpc);
    };
    GpuAgg& g = g_gpuAgg;
    for (uint32_t i = 0; i < kTsSlots; ++i) {
        TsSlot& s = fq->slots[(fq->nextSlot + i) % kTsSlots];
        if (!s.pending) continue;
        if (s.fence > done) break; // the GPU finishes submissions in order
        s.pending = false;
        const uint64_t* ts = fq->rbPtr + (size_t)(&s - fq->slots) * kTsPerBank;
        const size_t m = s.segs.size();
        bool ok = ts[0] != 0;
        for (size_t k = 0; k < m && ok; ++k) ok = ts[k + 1] >= ts[k];
        if (!ok) { ++g.anomalies; continue; }
        const int64_t start = toQpc(ts[0]), end = toQpc(ts[m]);
        ++g.timed;
        g.busyMs += QpcMs(end - start);
        // GPU idle time before this submission started: before or after the CPU submitted it.
        if (fq->prevEnd && start > fq->prevEnd) {
            const int64_t gap = start - fq->prevEnd;
            int64_t starved = s.submitQpc - fq->prevEnd;
            if (starved < 0) starved = 0;
            if (starved > gap) starved = gap;
            g.starvedMs += QpcMs(starved);
            g.stalledMs += QpcMs(gap - starved);
        }
        if (end > fq->prevEnd) fq->prevEnd = end;
        const double lat = QpcMs(start - s.submitQpc);
        if (lat > 0) { g.latSumMs += lat; if (lat > g.latMaxMs) g.latMaxMs = lat; }
        for (size_t k = 0; k < m; ++k)
            AddTimedSegment(g, s.segs[k], QpcMs(toQpc(ts[k + 1]) - toQpc(ts[k])), s.ecl, (uint32_t)k);
    }
}

// Called for game submissions on DIRECT queues. Returns true if it submitted the
// lists itself (with our copy list in front and/or timestamp lists in between);
// false = caller submits unchanged.
static bool InjectExecute(QueueStats* qs, ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l, PFN_ECL oECL)
{
    if (!n || !l) return false;
    const int64_t submitQpc = Qpc();
    std::lock_guard<std::mutex> lk(g_fixMutex);
    const bool hyb = g_hybTable.load(std::memory_order_acquire) != nullptr;
    if (hyb) HybTakeDeadTwins();
    if (!g_pendingRel.empty()) ReleasePending();
    if (qs->fixFailed) return false;

    // Ranges of all submitted lists (the game closed them before this call).
    FixQueue* fq = qs->fix;
    std::vector<ListRange> local;
    std::vector<HybRange> hlocal;
    std::vector<ListRange>& rs = fq ? fq->scratch : local;
    std::vector<HybRange>& hrs = fq ? fq->hscratch : hlocal;
    rs.clear();
    hrs.clear();
    if (g_cfg.constPages || hyb) {
        AcquireSRWLockShared(&g_listLock);
        for (UINT i = 0; i < n; ++i) {
            auto it = g_lists.find(l[i]);
            if (it == g_lists.end()) continue;
            const ListState* ls = it->second;
            if (g_cfg.constPages) rs.insert(rs.end(), ls->ranges.begin(), ls->ranges.end());
            if (hyb) hrs.insert(hrs.end(), ls->hranges.begin(), ls->hranges.end());
        }
        ReleaseSRWLockShared(&g_listLock);
    }
    if (hyb) {
        HybTakeSrvRanges(hrs);
        HybPollRetire();
        // Stats: exact bytes referenced, and how much of it earlier submissions of this frame referenced too.
        g_hybUnionBytes.fetch_add(MergeRanges(hrs, 0), std::memory_order_relaxed);
        if (fq) {
            const uint64_t frame = g_frames.load(std::memory_order_relaxed);
            if (frame != fq->hframeNo) { fq->hframe.clear(); fq->hframeNo = frame; }
            g_hybRecopyBytes.fetch_add(OverlapBytes(hrs, fq->hframe), std::memory_order_relaxed);
            fq->hframe.insert(fq->hframe.end(), hrs.begin(), hrs.end());
            MergeRanges(fq->hframe, 0);
        }
        MergeRanges(hrs, (uint64_t)g_cfg.hybridMergeKB << 10);
        if (g_cfg.hybridCheck) HcOnSubmit(hrs);
        if (g_cfg.hybridNoCopy) hrs.clear(); // negative test: the mirrors stay empty / stale
    }
    const bool wantCopy = !rs.empty() || !hrs.empty();
    if (!wantCopy && !g_cfg.gpuTiming) return false;

    auto fail = [&](const char* what, HRESULT hr) {
        // Lists recorded so far still point at twins/mirrors we cannot fill: stop redirecting.
        Log("fix: %s failed (0x%08lx) on queue %p; disabling redirects", what, hr, q);
        g_fixRedirect.store(false);
        g_hybRedirect.store(false);
        g_fixFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (!fq) {
        fq = qs->fix = CreateFixQueue(q);
        if (!fq) { qs->fixFailed = true; return wantCopy ? fail("copy context creation", E_FAIL) : false; }
        fq->scratch.swap(local);
        fq->hscratch.swap(hlocal);
    }

    // Stage 2d copies on our COPY queue ([Fix] HybridCopyQueue): they overlap the game's
    // earlier work still running on its queue; the game's lists wait for them.
    uint64_t regions = 0, bytes = 0, hregions = 0, hbytes = 0;
    const bool async = !fq->hscratch.empty() && fq->cq;
    if (async) {
        const int ci = AcquireAlloc(fq, fq->callocs, fq->cfence, D3D12_COMMAND_LIST_TYPE_COPY);
        if (ci < 0) return fail("COPY queue allocator wait", E_FAIL);
        ID3D12CommandAllocator* ca = fq->callocs[ci].alloc;
        ++t_inFix;
        HRESULT ch = ca->Reset();
        if (SUCCEEDED(ch)) ch = fq->clist->Reset(ca, nullptr);
        if (SUCCEEDED(ch)) {
            RecordHybCopies(fq->clist, fq->hscratch, nullptr, hregions, hbytes);
            ch = fq->clist->Close();
        }
        if (SUCCEEDED(ch)) {
            ID3D12CommandList* cl = fq->clist;
            fq->cq->ExecuteCommandLists(1, &cl);
        }
        --t_inFix;
        if (FAILED(ch)) return fail("COPY queue list recording", ch);
        fq->cq->Signal(fq->cfence, ++fq->cSignaled);
        fq->callocs[ci].fence = fq->cSignaled;
        q->Wait(fq->cfence, fq->cSignaled);
        g_hybAsync.fetch_add(1, std::memory_order_relaxed);
    }
    const bool directCopy = !fq->scratch.empty() || (!fq->hscratch.empty() && !async);

    bool timing = fq->timing;
    if (timing) ProcessTimingSlots(fq);
    const uint32_t segs = (directCopy ? 1u : 0u) + n;
    TsSlot* slot = &fq->slots[fq->nextSlot];
    if (timing && (segs + 1 > (uint32_t)kTsPerBank || slot->pending)) { timing = false; ++g_gpuAgg.skipped; }
    if (!directCopy && !timing) {
        if (!async) return false;
        // Only the wait was needed. Submit the game's lists here so our fence covers them
        // (and with them the COPY queue work they waited for): deferred releases rely on it.
        oECL(q, n, l);
        q->Signal(fq->fence, ++fq->lastSignaled);
        if (g_hybInj.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("hybrid: first mirror copy (COPY queue) for queue %p: %u game lists -> %llu copies, %.2f MB", q, n, hregions, MB(hbytes));
        g_hybRegions.fetch_add(hregions, std::memory_order_relaxed);
        g_hybBytes.fetch_add(hbytes, std::memory_order_relaxed);
        return true;
    }

    const int ai = AcquireFixAllocator(fq);
    if (ai < 0) {
        if (directCopy || async) return fail("allocator wait", E_FAIL); // async: the wait is queued, lists must follow
        return false;
    }
    ID3D12CommandAllocator* alloc = fq->allocs[ai].alloc;

    ++t_inFix;
    HRESULT hr = alloc->Reset();
    if (SUCCEEDED(hr) && directCopy) hr = RecordCopies(fq, alloc, !async, regions, bytes, hregions, hbytes);
    if (FAILED(hr)) { --t_inFix; return (directCopy || async) ? fail("copy list recording", hr) : false; }
    const uint32_t bank = fq->nextBank;
    if (timing) {
        HRESULT th = fq->post->Reset(alloc, nullptr);
        if (SUCCEEDED(th)) {
            const UINT base = bank * kTsPerBank;
            fq->post->EndQuery(fq->qheap, D3D12_QUERY_TYPE_TIMESTAMP, base + segs);
            fq->post->ResolveQueryData(fq->qheap, D3D12_QUERY_TYPE_TIMESTAMP, base, segs + 1, fq->rb,
                                       (UINT64)(slot - fq->slots) * kTsPerBank * sizeof(uint64_t));
            th = fq->post->Close();
        }
        if (FAILED(th)) {
            Log("timing: post list recording failed (0x%08lx); GPU timing off", th);
            fq->timing = timing = false;
        }
    }
    --t_inFix;
    if (!directCopy && !timing && !async) return false;

    if (timing) { // what each game list recorded, for the breakdown
        slot->segs.clear();
        if (directCopy) slot->segs.push_back({ {}, true, true });
        AcquireSRWLockShared(&g_listLock);
        for (UINT i = 0; i < n; ++i) {
            TsSeg sg = {};
            auto it = g_lists.find(l[i]);
            if (it != g_lists.end() && it->second->known) { sg.c = it->second->cnt; sg.known = true; }
            slot->segs.push_back(sg);
        }
        ReleaseSRWLockShared(&g_listLock);
    }

    fq->submit.clear();
    if (timing) {
        for (uint32_t k = 0; k < segs; ++k) {
            fq->submit.push_back(fq->tsList[bank][k]);
            fq->submit.push_back(directCopy ? (k ? l[k - 1] : (ID3D12CommandList*)fq->list) : l[k]);
        }
        fq->submit.push_back(fq->post);
    } else {
        if (directCopy) fq->submit.push_back(fq->list);
        fq->submit.insert(fq->submit.end(), l, l + n);
    }
    oECL(q, (UINT)fq->submit.size(), fq->submit.data());
    q->Signal(fq->fence, ++fq->lastSignaled);
    fq->allocs[ai].fence = fq->lastSignaled;

    if (timing) {
        const uint64_t frame = g_frames.load(std::memory_order_relaxed);
        if (frame != fq->lastFrame) { fq->lastFrame = frame; fq->eclInFrame = 0; }
        slot->pending = true;
        slot->fence = fq->lastSignaled;
        slot->submitQpc = submitQpc;
        slot->bank = bank;
        slot->ecl = fq->eclInFrame++;
        fq->nextSlot = (fq->nextSlot + 1) % kTsSlots;
        fq->nextBank = (bank + 1) % kTsBanks;
    }
    if (!fq->scratch.empty()) {
        if (g_fixInjections.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("fix: first copy injection on queue %p: %u game lists, %zu ranges -> %llu copies, %.2f MB",
                q, n, fq->scratch.size(), regions, MB(bytes));
        g_fixCopyRegions.fetch_add(regions, std::memory_order_relaxed);
        g_fixCopyBytes.fetch_add(bytes, std::memory_order_relaxed);
    }
    if (!fq->hscratch.empty()) {
        if (g_hybInj.fetch_add(1, std::memory_order_relaxed) == 0)
            Log("hybrid: first mirror copy on queue %p: %u game lists -> %llu copies, %.2f MB", q, n, hregions, MB(hbytes));
        g_hybRegions.fetch_add(hregions, std::memory_order_relaxed);
        g_hybBytes.fetch_add(hbytes, std::memory_order_relaxed);
    }
    return true;
}

// One template instance per queue type in case the runtime uses different
// implementations (each MinHook target needs its own trampoline).
template <int V> struct QueueHooks {
    static inline PFN_ECL oECL = nullptr;
    static void STDMETHODCALLTYPE ECL(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* l)
    {
        if (t_inFix) { oECL(q, n, l); return; } // our own COPY queue submission
        QueueStats* qs = OnExecuteCommandLists(q, n);
        const bool game = t_presentDepth == 0;
        if (game && g_shTable.load(std::memory_order_relaxed)) { // stage 2e: before anything else
            if (qs->shDev < 0) {
                ID3D12Device* sd = g_shDev.load(std::memory_order_acquire);
                ID3D12Device* qd = nullptr;
                if (sd && SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&qd))) && qd) { qs->shDev = qd == sd; qd->Release(); }
            }
            if (qs->shDev > 0) ShadowSync(q);
        }
        const int64_t t0 = Qpc();
        const bool fix = g_cfg.constPages || g_cfg.gpuTiming || g_hybTable.load(std::memory_order_relaxed);
        if (!(game && fix && qs->desc.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && InjectExecute(qs, q, n, l, oECL)))
            oECL(q, n, l);
        if (game) AddTime(g_apiTime[AT_ECL], Qpc() - t0);
    }
    static void Install(void** vt, const char* typeName)
    {
        char name[96];
        sprintf_s(name, "Queue(%s)::ExecuteCommandLists", typeName);
        HookVT(vt, g_vtbl.Queue_ExecuteCommandLists, (void*)&ECL, (void**)&oECL, name);
    }
};

// ============================================================================
// Command lists
// ============================================================================
using PFN_RootVA   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_IASetVB  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
using PFN_IASetIB  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
using PFN_Draw     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
using PFN_DrawIdx  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
using PFN_Dispatch = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_ExecInd  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT,
                                              ID3D12Resource*, UINT64, ID3D12Resource*, UINT64);
using PFN_CopyBR   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64,
                                              ID3D12Resource*, UINT64, UINT64);
using PFN_CopyRes  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
using PFN_Close    = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Reset    = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);

// The game's command lists may use vtables (runtime classes) other than the
// ones of our temporary lists; Reset/Close detect and hook them on first sight.
static void CheckListVtable(ID3D12GraphicsCommandList* cl);

template <int V> struct ListHooks {
    static inline PFN_RootVA oGfxCbv, oCmpCbv, oGfxSrv, oCmpSrv;
    static inline PFN_IASetVB oVB;
    static inline PFN_IASetIB oIB;
    static inline PFN_Draw oDraw;
    static inline PFN_DrawIdx oDrawIdx;
    static inline PFN_Dispatch oDispatch;
    static inline PFN_ExecInd oExecInd;
    static inline PFN_CopyBR oCopyBR;
    static inline PFN_CopyRes oCopyRes;
    static inline PFN_Close oClose;
    static inline PFN_Reset oReset;

    using CL = ID3D12GraphicsCommandList;
    static void STDMETHODCALLTYPE GfxCbv(CL* s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        ResRec* r = CountVA(U_RCBV_G, va);
        if (ListCounters* c = Counters(s)) ++c->rootCbv;
        if (r && r->twinVA) va = RedirectCbv(s, r, va);
        oGfxCbv(s, i, va);
    }
    static void STDMETHODCALLTYPE CmpCbv(CL* s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va)
    {
        ResRec* r = CountVA(U_RCBV_C, va);
        if (ListCounters* c = Counters(s)) ++c->rootCbv;
        if (r && r->twinVA) va = RedirectCbv(s, r, va);
        oCmpCbv(s, i, va);
    }
    static void STDMETHODCALLTYPE GfxSrv(CL* s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { CountVA(U_RSRV_G, va); oGfxSrv(s, i, va); }
    static void STDMETHODCALLTYPE CmpSrv(CL* s, UINT i, D3D12_GPU_VIRTUAL_ADDRESS va) { CountVA(U_RSRV_C, va); oCmpSrv(s, i, va); }
    static void STDMETHODCALLTYPE VB(CL* s, UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW* v)
    {
        D3D12_VERTEX_BUFFER_VIEW tmp[D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT];
        const D3D12_VERTEX_BUFFER_VIEW* use = v;
        if (v && n && n <= D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT && g_hybTable.load(std::memory_order_relaxed))
            use = HybVB(s, n, v, tmp); // stage 2d
        if (v && g_cfg.measure) {
            ListCounters* c = Counters(s);
            for (UINT k = 0; k < n; ++k) {
                const ResRec* r = CountVA(U_VB, v[k].BufferLocation, v[k].SizeInBytes);
                if (c && v[k].BufferLocation) {
                    ++c->vbAll;
                    if (r && IsSysmem(r->cat) && use[k].BufferLocation == v[k].BufferLocation) ++c->vbSys;
                }
            }
        }
        oVB(s, start, n, use);
    }
    static void STDMETHODCALLTYPE IB(CL* s, const D3D12_INDEX_BUFFER_VIEW* v)
    {
        D3D12_INDEX_BUFFER_VIEW tmp;
        const D3D12_INDEX_BUFFER_VIEW* use = v;
        if (v && g_hybTable.load(std::memory_order_relaxed)) use = HybIB(s, v, &tmp); // stage 2d
        if (v && g_cfg.measure) CountVA(U_IB, v->BufferLocation, v->SizeInBytes);
        oIB(s, use);
    }
    static void STDMETHODCALLTYPE Draw(CL* s, UINT a, UINT b, UINT c, UINT d)
    {
        Bump(TS()->misc[M_DRAW]);
        if (ListCounters* lc = Counters(s)) ++lc->draws;
        oDraw(s, a, b, c, d);
    }
    static void STDMETHODCALLTYPE DrawIdx(CL* s, UINT a, UINT b, UINT c, INT d, UINT e)
    {
        Bump(TS()->misc[M_DRAW_IDX]);
        if (ListCounters* lc = Counters(s)) ++lc->draws;
        oDrawIdx(s, a, b, c, d, e);
    }
    static void STDMETHODCALLTYPE Dispatch(CL* s, UINT x, UINT y, UINT z)
    {
        Bump(TS()->misc[M_DISPATCH]);
        if (ListCounters* lc = Counters(s)) ++lc->dispatches;
        oDispatch(s, x, y, z);
    }
    static void STDMETHODCALLTYPE ExecInd(CL* s, ID3D12CommandSignature* sig, UINT max, ID3D12Resource* arg,
                                          UINT64 argOff, ID3D12Resource* cnt, UINT64 cntOff)
    {
        Bump(TS()->misc[M_EXEC_INDIRECT]);
        if (ListCounters* lc = Counters(s)) ++lc->execInd;
        oExecInd(s, sig, max, arg, argOff, cnt, cntOff);
    }
    static void STDMETHODCALLTYPE CopyBR(CL* s, ID3D12Resource* dst, UINT64 dstOff, ID3D12Resource* src,
                                         UINT64 srcOff, UINT64 bytes)
    {
        if (!t_inFix) {
            CountRes(U_COPY_SRC, src, srcOff);
            CountRes(U_COPY_DST, dst, dstOff);
        }
        oCopyBR(s, dst, dstOff, src, srcOff, bytes);
    }
    static void STDMETHODCALLTYPE CopyRes(CL* s, ID3D12Resource* dst, ID3D12Resource* src)
    {
        CountRes(U_COPY_SRC, src, 0);
        CountRes(U_COPY_DST, dst, 0);
        oCopyRes(s, dst, src);
    }
    static HRESULT STDMETHODCALLTYPE Close(CL* s)
    {
        if (t_inFix) return oClose(s); // our own copy list
        Bump(TS()->misc[t_presentDepth ? M_CL_CLOSE_DXGI : M_CL_CLOSE]);
        if (g_cfg.measure) FlushPending();
        CheckListVtable(s);
        if (g_cfg.constPages || g_cfg.gpuTiming || g_cfg.hybridHeaps) OnListClose(s);
        return oClose(s);
    }
    static HRESULT STDMETHODCALLTYPE Reset(CL* s, ID3D12CommandAllocator* a, ID3D12PipelineState* p)
    {
        if (t_inFix) return oReset(s, a, p); // our own copy list
        Bump(TS()->misc[t_presentDepth ? M_CL_RESET_DXGI : M_CL_RESET]);
        CheckListVtable(s);
        const HRESULT hr = oReset(s, a, p);
        if (g_cfg.constPages || g_cfg.gpuTiming || g_cfg.hybridHeaps) OnListReset(s, SUCCEEDED(hr));
        return hr;
    }

    // Fix-only mode ([General] Measure=0) hooks only what stage 2d needs ("lean").
    static void Install(void** vt, const char* typeName)
    {
        struct Item { int idx; void* detour; void** orig; const char* name; bool lean; };
        const Item items[] = {
            { g_vtbl.CL_SetGraphicsRootConstantBufferView,  (void*)&GfxCbv,   (void**)&oGfxCbv,   "SetGraphicsRootConstantBufferView", false },
            { g_vtbl.CL_SetComputeRootConstantBufferView,   (void*)&CmpCbv,   (void**)&oCmpCbv,   "SetComputeRootConstantBufferView", false },
            { g_vtbl.CL_SetGraphicsRootShaderResourceView,  (void*)&GfxSrv,   (void**)&oGfxSrv,   "SetGraphicsRootShaderResourceView", false },
            { g_vtbl.CL_SetComputeRootShaderResourceView,   (void*)&CmpSrv,   (void**)&oCmpSrv,   "SetComputeRootShaderResourceView", false },
            { g_vtbl.CL_IASetVertexBuffers,                 (void*)&VB,       (void**)&oVB,       "IASetVertexBuffers", true },
            { g_vtbl.CL_IASetIndexBuffer,                   (void*)&IB,       (void**)&oIB,       "IASetIndexBuffer", true },
            { g_vtbl.CL_DrawInstanced,                      (void*)&Draw,     (void**)&oDraw,     "DrawInstanced", false },
            { g_vtbl.CL_DrawIndexedInstanced,               (void*)&DrawIdx,  (void**)&oDrawIdx,  "DrawIndexedInstanced", false },
            { g_vtbl.CL_Dispatch,                           (void*)&Dispatch, (void**)&oDispatch, "Dispatch", false },
            { g_vtbl.CL_ExecuteIndirect,                    (void*)&ExecInd,  (void**)&oExecInd,  "ExecuteIndirect", false },
            { g_vtbl.CL_CopyBufferRegion,                   (void*)&CopyBR,   (void**)&oCopyBR,   "CopyBufferRegion", false },
            { g_vtbl.CL_CopyResource,                       (void*)&CopyRes,  (void**)&oCopyRes,  "CopyResource", false },
            { g_vtbl.CL_Close,                              (void*)&Close,    (void**)&oClose,    "Close", true },
            { g_vtbl.CL_Reset,                              (void*)&Reset,    (void**)&oReset,    "Reset", true },
        };
        for (const Item& it : items) {
            if (!g_cfg.measure && !it.lean) continue;
            char name[128];
            sprintf_s(name, "CL(%s)::%s", typeName, it.name);
            HookVT(vt, it.idx, it.detour, it.orig, name);
        }
    }
};

// ============================================================================
// Frames (DXGI Present)
// ============================================================================
static std::atomic<int64_t> g_lastPresentQpc{0};
static std::atomic<uint64_t> g_ftSumUs{0}, g_ftCount{0}, g_ftMaxUs{0}, g_ftOver33{0}, g_ftOver50{0};

static void OnPresent(UINT flags)
{
    if (flags & DXGI_PRESENT_TEST) return;
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const int64_t prev = g_lastPresentQpc.exchange(now.QuadPart);
    if (prev) {
        const uint64_t us = (uint64_t)((now.QuadPart - prev) * 1000000 / g_qpf.QuadPart);
        g_ftSumUs.fetch_add(us, std::memory_order_relaxed);
        g_ftCount.fetch_add(1, std::memory_order_relaxed);
        uint64_t m = g_ftMaxUs.load(std::memory_order_relaxed);
        while (us > m && !g_ftMaxUs.compare_exchange_weak(m, us)) {}
        if (us > 33333) g_ftOver33.fetch_add(1, std::memory_order_relaxed);
        if (us > 50000) g_ftOver50.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_frames.fetch_add(1, std::memory_order_relaxed) == 0) Log("first Present");
}

using PFN_Present  = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
static PFN_Present o_Present;
static PFN_Present1 o_Present1;

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    const bool outer = t_presentDepth++ == 0;
    if (outer) OnPresent(flags);
    const int64_t t0 = Qpc();
    HRESULT hr = o_Present(sc, sync, flags);
    if (outer) AddTime(g_apiTime[AT_PRESENT], Qpc() - t0);
    --t_presentDepth;
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_Present1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp)
{
    const bool outer = t_presentDepth++ == 0;
    if (outer) OnPresent(flags);
    const int64_t t0 = Qpc();
    HRESULT hr = o_Present1(sc, sync, flags, pp);
    if (outer) AddTime(g_apiTime[AT_PRESENT], Qpc() - t0);
    --t_presentDepth;
    return hr;
}

static std::atomic<bool> g_swapHooked{false};

static void OnSwapChainCreated(IUnknown* unk)
{
    IDXGISwapChain1* sc = nullptr;
    if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&sc))) || !sc) return;
    DXGI_SWAP_CHAIN_DESC1 d = {};
    sc->GetDesc1(&d);
    Log("swapchain %p: %ux%u format %d buffers %u swapEffect %d flags 0x%x scaling %d alpha %d",
        sc, d.Width, d.Height, (int)d.Format, d.BufferCount, (int)d.SwapEffect, d.Flags, (int)d.Scaling, (int)d.AlphaMode);
    if (!g_swapHooked.exchange(true)) {
        void** vt = *reinterpret_cast<void***>(sc);
        HookVT(vt, g_vtbl.SwapChain_Present, (void*)&Hook_Present, (void**)&o_Present, "SwapChain::Present");
        HookVT(vt, g_vtbl.SwapChain1_Present1, (void*)&Hook_Present1, (void**)&o_Present1, "SwapChain1::Present1");
        ApplyHooks();
    }
    sc->Release();
}

using PFN_CreateSwapChain = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using PFN_CreateSwapChainForHwnd = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_CreateSwapChain o_CreateSwapChain;
static PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd;

static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** pp)
{
    HRESULT hr = o_CreateSwapChain(f, dev, desc, pp);
    Log("CreateSwapChain -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && pp && *pp) OnSwapChainCreated(*pp);
    return hr;
}
static HRESULT STDMETHODCALLTYPE Hook_CreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND hwnd,
                                                             const DXGI_SWAP_CHAIN_DESC1* desc,
                                                             const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                             IDXGIOutput* out, IDXGISwapChain1** pp)
{
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, hwnd, desc, fs, out, pp);
    Log("CreateSwapChainForHwnd(windowed=%d) -> 0x%08lx", fs ? (int)fs->Windowed : -1, hr);
    if (SUCCEEDED(hr) && pp && *pp) OnSwapChainCreated(*pp);
    return hr;
}

static std::atomic<bool> g_factoryHooked{false};

static void OnFactoryCreated(IUnknown* unk)
{
    if (g_factoryHooked.load()) return;
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    if (g_factoryHooked.load()) return;
    IDXGIFactory2* f = nullptr;
    if (FAILED(unk->QueryInterface(IID_PPV_ARGS(&f))) || !f) return;
    void** vt = *reinterpret_cast<void***>(f);
    HookVT(vt, g_vtbl.Factory_CreateSwapChain, (void*)&Hook_CreateSwapChain, (void**)&o_CreateSwapChain, "Factory::CreateSwapChain");
    HookVT(vt, g_vtbl.Factory2_CreateSwapChainForHwnd, (void*)&Hook_CreateSwapChainForHwnd, (void**)&o_CreateSwapChainForHwnd, "Factory2::CreateSwapChainForHwnd");
    ApplyHooks();
    f->Release();
    g_factoryHooked.store(true);
}

using PFN_CreateDXGIFactory  = HRESULT(WINAPI*)(REFIID, void**);
using PFN_CreateDXGIFactory2 = HRESULT(WINAPI*)(UINT, REFIID, void**);
static PFN_CreateDXGIFactory o_CreateDXGIFactory, o_CreateDXGIFactory1;
static PFN_CreateDXGIFactory2 o_CreateDXGIFactory2;

static HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void** pp)
{
    HRESULT hr = o_CreateDXGIFactory(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) OnFactoryCreated(static_cast<IUnknown*>(*pp));
    return hr;
}
static HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void** pp)
{
    HRESULT hr = o_CreateDXGIFactory1(riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) OnFactoryCreated(static_cast<IUnknown*>(*pp));
    return hr;
}
static HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void** pp)
{
    HRESULT hr = o_CreateDXGIFactory2(flags, riid, pp);
    if (SUCCEEDED(hr) && pp && *pp) OnFactoryCreated(static_cast<IUnknown*>(*pp));
    return hr;
}

// ============================================================================
// Stats thread
// ============================================================================
struct StatsState {
    LARGE_INTEGER prevT = {};
    uint64_t prevFrames = 0;
    uint64_t prevUse[U_COUNT][HC_COUNT] = {};
    uint64_t prevMisc[M_COUNT] = {};
    uint64_t prevCreated[HC_COUNT] = {}, prevCreatedBytes[HC_COUNT] = {};
    uint64_t prevDestroyed[HC_COUNT] = {}, prevHeaps[HC_COUNT] = {};
    uint64_t prevWinBytes[WC_COUNT] = {}, prevWinBufs[WC_COUNT] = {}, prevWinCount = 0;
    size_t prevRecIndex = 0; // g_allRecs entries already classified as "new"
    uint64_t prevFixInj = 0, prevFixRegions = 0, prevFixBytes = 0;
    uint64_t prevHybInj = 0, prevHybRegions = 0, prevHybBytes = 0, prevHybSrvQ = 0, prevHybSrvQB = 0;
    uint64_t prevHybUnion = 0, prevHybRecopy = 0, prevHybAsync = 0;
    uint64_t prevShSyncs = 0, prevShCopy = 0, prevShDirty = 0, prevShRegions = 0, prevShGww = 0, prevShWaits = 0, prevShBytes = 0;
    uint64_t prevShBgFl = 0, prevShBgCp = 0, prevShBgBytes = 0, prevShBgCpu = 0, prevShSkA = 0, prevShSkF = 0, prevShSkM = 0;
    uint64_t prevShEclHot = 0, prevShEclCold = 0;
    std::map<DWORD, uint64_t> prevThreadCpu; // thread id -> user+kernel time (100 ns)
};

// Stage 2e state, both modes.
static void AppendShadowStats(std::string& s, StatsState& st, double div, const char* per)
{
    if (!g_cfg.shadowHeaps) return;
    if (!g_shOn.load()) { Appendf(s, "  fix Shadow: OFF (%s)\n", g_shOffReason); return; }
    const uint64_t sy = g_shSyncs.load(), cp = g_shSyncsCopy.load(), di = g_shDirtyBytes.load(), rg = g_shRegions.load();
    const uint64_t gw = g_shGwwTicks.load(), wa = g_shWaits.load(), cb = g_shCopyBytes.load();
    const uint64_t dCb = cb - st.prevShBytes;
    const double gwMs = (gw - st.prevShGww) * 1000.0 / (double)g_qpf.QuadPart;
    Appendf(s, "  fix Shadow: ON | heaps %llu (%.0f MB) | syncs%s %.1f (with copies %.1f), dirty %.2f MB (copied %.2f MB),"
               " regions %.1f, GetWriteWatch %.3f ms, queue waits %.1f | released by game %llu (kept)",
            g_shHeaps.load(), MB(g_shLiveBytes.load()), per,
            (sy - st.prevShSyncs) / div, (cp - st.prevShCopy) / div, MB(di - st.prevShDirty) / div, MB(cb - st.prevShBytes) / div,
            (rg - st.prevShRegions) / div, gwMs / div, (wa - st.prevShWaits) / div, g_shReleased.load());
    st.prevShSyncs = sy; st.prevShCopy = cp; st.prevShDirty = di; st.prevShRegions = rg; st.prevShGww = gw;
    st.prevShWaits = wa; st.prevShBytes = cb;
    if (g_cfg.shadowStreamUs > 0) {
        const uint64_t bf = g_shBgFlushes.load(), bc = g_shBgCopies.load(), bb = g_shBgBytes.load(), bt = g_shBgCpuTicks.load();
        const uint64_t sa = g_shBgSkipAlloc.load(), sf = g_shBgSkipFlight.load(), sm = g_shBgSkipMin.load();
        const double bgMB = MB(bb - st.prevShBgBytes) / div;
        const double eclMB = MB(dCb - (bb - st.prevShBgBytes)) / div;
        Appendf(s, " | stream %d us%s: flushes %.1f (with copies %.1f), background %.2f MB vs ECL %.2f MB, skipped: no allocator %.1f,"
                   " in-flight limit %.1f, below %d KB %.1f, stream thread CPU %.3f ms | ECL dirty in hot heaps %.2f MB, cold %.2f MB%s",
                g_cfg.shadowStreamUs, per, (bf - st.prevShBgFl) / div, (bc - st.prevShBgCp) / div, bgMB, eclMB,
                (sa - st.prevShSkA) / div, (sf - st.prevShSkF) / div, g_cfg.shadowStreamMinKB, (sm - st.prevShSkM) / div,
                (bt - st.prevShBgCpu) * 1000.0 / (double)g_qpf.QuadPart / div,
                MB(g_shEclHotBytes.load() - st.prevShEclHot) / div, MB(g_shEclColdBytes.load() - st.prevShEclCold) / div,
                g_cfg.shadowStreamAll ? " (all heaps scanned)" : "");
        st.prevShEclHot = g_shEclHotBytes.load(); st.prevShEclCold = g_shEclColdBytes.load();
        st.prevShBgFl = bf; st.prevShBgCp = bc; st.prevShBgBytes = bb; st.prevShBgCpu = bt;
        st.prevShSkA = sa; st.prevShSkF = sf; st.prevShSkM = sm;
    }
    if (g_shFailed.load()) Appendf(s, " | heap creation failed %llu", g_shFailed.load());
    if (g_shWaitFail.load()) Appendf(s, " | QUEUE WAIT FAILURES %llu", g_shWaitFail.load());
    if (g_shCopyFail.load()) Appendf(s, " | COPY SUBMISSION FAILURES %llu", g_shCopyFail.load());
    if (g_cfg.shadowNoCopy) Appendf(s, " | TEST: COPIES DISABLED");
    Appendf(s, "\n");
}

// Stage 2d state, both modes.
static void AppendHybStats(std::string& s, StatsState& st, const uint64_t* dMisc, double div, const char* per)
{
    if (!g_cfg.hybridHeaps) return;
    if (!g_hybOn.load()) { Appendf(s, "  fix Hybrid: OFF (%s)\n", g_hybOffReason); return; }
    const uint64_t inj = g_hybInj.load(), reg = g_hybRegions.load(), by = g_hybBytes.load();
    const uint64_t sq = g_hybSrvQueued.load(), sqb = g_hybSrvQueuedBytes.load();
    const uint64_t un = g_hybUnionBytes.load(), rc = g_hybRecopyBytes.load(), as = g_hybAsync.load();
    const uint64_t dInj = inj - st.prevHybInj, dReg = reg - st.prevHybRegions, dBy = by - st.prevHybBytes;
    const uint64_t dSq = sq - st.prevHybSrvQ, dSqb = sqb - st.prevHybSrvQB;
    const uint64_t dUn = un - st.prevHybUnion, dRc = rc - st.prevHybRecopy, dAs = as - st.prevHybAsync;
    st.prevHybInj = inj; st.prevHybRegions = reg; st.prevHybBytes = by; st.prevHybSrvQ = sq; st.prevHybSrvQB = sqb;
    st.prevHybUnion = un; st.prevHybRecopy = rc; st.prevHybAsync = as;
    Appendf(s, "  fix Hybrid: ON | mirrors %llu (%.0f MB), retired %llu | binds%s: VB %.1f, IB %.1f, left on sysmem %.1f"
               " | SRV: alias %.1f, twin %.1f, left %.1f, new ranges %.1f (%.2f MB) | copies%s: submissions %.2f%s,"
               " regions %.1f, %.2f MB (referenced %.2f MB, of it already this frame %.2f MB) | twins live %llu (made %llu)",
            g_hybHeaps.load() - g_hybRetired.load(), MB(g_hybLiveBytes.load()), g_hybRetired.load(), per,
            dMisc[M_HYB_VB] / div, dMisc[M_HYB_IB] / div, dMisc[M_HYB_SKIP] / div,
            dMisc[M_HYB_SRV_ALIAS] / div, dMisc[M_HYB_SRV_TWIN] / div, dMisc[M_HYB_SRV_SKIP] / div,
            dSq / div, MB(dSqb) / div, per, dInj / div, dAs ? " (COPY queue)" : "", dReg / div, MB(dBy) / div,
            MB(dUn) / div, MB(dRc) / div, g_hybTwinsLive.load(), g_hybTwinsTotal.load());
    if (!g_hybRedirect.load()) Appendf(s, " | REDIRECTS DISABLED (copy failure, see log)");
    if (g_hybBadLayout.load()) Appendf(s, " | VA layout check FAILED for %llu heap(s)", g_hybBadLayout.load());
    if (g_hybFailed.load()) Appendf(s, " | mirror creation failed %llu", g_hybFailed.load());
    if (g_hybTwinFail.load()) Appendf(s, " | twin creation failed %llu", g_hybTwinFail.load());
    if (g_cfg.hybridNoCopy) Appendf(s, " | TEST: COPIES DISABLED");
    Appendf(s, "\n");
    if (!g_cfg.hybridCheck) return;
    HcStats h;
    size_t watched;
    {
        std::lock_guard<std::mutex> lk(g_fixMutex);
        h = g_hc;
        g_hc = HcStats();
        watched = g_hcList.size();
    }
    uint64_t age[HA_COUNT][2] = {}, srvSize[4] = {}, total = 0;
    for (int a = 0; a < HA_COUNT; ++a)
        for (int v = 0; v < 2; ++v)
            for (int z = 0; z < 4; ++z) {
                age[a][v] += h.stale[a][v][z];
                if (v) srvSize[z] += h.stale[a][v][z];
                total += h.stale[a][v][z];
            }
    Appendf(s, "  hybrid check%s: watched %zu, hashed %.1f (%.2f ms), copied %.1f (%.2f MB), of them unchanged since their"
               " last copy %.1f (%.2f MB) | STALE (changed after last copy, no new"
               " reference; repaired) %.2f: with SRV new/last frame/older %.2f/%.2f/%.2f, without SRV %.2f/%.2f/%.2f |"
               " SRV ones by size <4K %.2f, <64K %.2f, <1M %.2f, >=1M %.2f | at death: changed %llu of %llu\n",
            per, watched, h.hashed / div, h.ms / div, h.copied / div, MB(h.copiedBytes) / div, h.redundant / div,
            MB(h.redundantBytes) / div, total / div,
            age[HA_NEW][1] / div, age[HA_PREV][1] / div, age[HA_OLD][1] / div,
            age[HA_NEW][0] / div, age[HA_PREV][0] / div, age[HA_OLD][0] / div,
            srvSize[0] / div, srvSize[1] / div, srvSize[2] / div, srvSize[3] / div, h.deathChanged, h.deathChecked);
}

// CPU time per thread of this process since the last block (user + kernel),
// top threads by time. GetThreadTimes is tick-based, fine over a 5 s window.
static void AppendThreadCpu(std::string& s, StatsState& st, double div, const char* per)
{
    using PFN_GTD = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static PFN_GTD gtd = (PFN_GTD)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription");
    struct T { DWORD tid; double ms; std::string name; };
    std::vector<T> ts;
    std::map<DWORD, uint64_t> now;
    double total = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(h, &c, &e, &k, &u)) {
            const uint64_t t = ((uint64_t)k.dwHighDateTime << 32 | k.dwLowDateTime) +
                               ((uint64_t)u.dwHighDateTime << 32 | u.dwLowDateTime);
            now[te.th32ThreadID] = t;
            auto it = st.prevThreadCpu.find(te.th32ThreadID);
            const uint64_t prev = it != st.prevThreadCpu.end() ? it->second : 0;
            const double ms = t > prev ? (t - prev) / 10000.0 : 0.0;
            total += ms;
            if (ms > 0) {
                T x = { te.th32ThreadID, ms, "" };
                PWSTR w = nullptr;
                if (gtd && SUCCEEDED(gtd(h, &w)) && w) {
                    char nm[64];
                    WideCharToMultiByte(CP_UTF8, 0, w, -1, nm, sizeof(nm), nullptr, nullptr);
                    nm[sizeof(nm) - 1] = 0;
                    x.name = nm;
                    LocalFree(w);
                }
                ts.push_back(std::move(x));
            }
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    st.prevThreadCpu.swap(now);
    std::sort(ts.begin(), ts.end(), [](const T& a, const T& b) { return a.ms > b.ms; });
    Appendf(s, "  CPU threads%s (ms, user+kernel): total %.1f, %zu threads busy; top:", per, total / div, ts.size());
    for (size_t i = 0; i < ts.size() && i < 10; ++i)
        Appendf(s, "%s %s#%lu %.1f", i % 5 ? " |" : "\n   ", ts[i].name.empty() ? "" : ts[i].name.c_str(),
                ts[i].tid, ts[i].ms / div);
    Appendf(s, "\n");
}

static void DumpStats(StatsState& st)
{
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    const double dt = (double)(now.QuadPart - st.prevT.QuadPart) / g_qpf.QuadPart;
    st.prevT = now;
    const uint64_t frames = g_frames.load();
    const uint64_t df = frames - st.prevFrames;
    st.prevFrames = frames;
    const double div = df ? (double)df : (dt > 0 ? dt : 1.0);
    const char* per = df ? "/frame" : "/s";

    uint64_t use[U_COUNT][HC_COUNT] = {}, misc[M_COUNT] = {};
    {
        std::lock_guard<std::mutex> lk(g_tsMutex);
        for (ThreadStats* t : g_threadStats) {
            for (int u = 0; u < U_COUNT; ++u)
                for (int c = 0; c < HC_COUNT; ++c) use[u][c] += t->use[u][c].load(std::memory_order_relaxed);
            for (int m = 0; m < M_COUNT; ++m) misc[m] += t->misc[m].load(std::memory_order_relaxed);
        }
    }
    uint64_t dUse[U_COUNT][HC_COUNT], dMisc[M_COUNT];
    for (int u = 0; u < U_COUNT; ++u)
        for (int c = 0; c < HC_COUNT; ++c) { dUse[u][c] = use[u][c] - st.prevUse[u][c]; st.prevUse[u][c] = use[u][c]; }
    for (int m = 0; m < M_COUNT; ++m) { dMisc[m] = misc[m] - st.prevMisc[m]; st.prevMisc[m] = misc[m]; }

    const uint64_t ftSum = g_ftSumUs.exchange(0), ftCnt = g_ftCount.exchange(0), ftMax = g_ftMaxUs.exchange(0);
    const uint64_t o33 = g_ftOver33.exchange(0), o50 = g_ftOver50.exchange(0);

    std::string s;
    SYSTEMTIME t; GetLocalTime(&t);
    Appendf(s, "[%02d:%02d:%02d.%03d] ==== stats %.1fs | frames %llu | %.1f fps | frametime avg %.2f ms, max %.2f ms, >33ms %llu, >50ms %llu ====\n",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, dt, df, df / (dt > 0 ? dt : 1.0),
            ftCnt ? ftSum / 1000.0 / ftCnt : 0.0, ftMax / 1000.0, o33, o50);
    if (!g_cfg.measure) { // fix-only mode: FPS, stage 2c state, CPU per thread
        if (!g_vidOn.load())
            Appendf(s, "  fix Vidmem: OFF (%s)\n", g_vidOffReason);
        else
            Appendf(s, "  fix Vidmem: ON | live in CPU-visible VRAM %.1f MB | heaps %llu, buffers %llu | left in sysmem: no room %llu,"
                       " NVAPI failed %llu, window full %llu\n",
                    MB(g_vidLiveBytes.load()), g_vidHeaps.load(), g_vidBufs.load(),
                    g_vidNoRoom.load(), g_vidFailed.load(), g_vidIgnored.load());
        AppendHybStats(s, st, dMisc, div, per);
        AppendShadowStats(s, st, div, per);
        AppendThreadCpu(s, st, div, per);
        LogRaw(s.c_str());
        return;
    }
    Appendf(s, "  calls%s: draw %.2f, drawIndexed %.2f, dispatch %.2f, executeIndirect %.2f, CL reset %.2f, CL close %.2f, Map %.2f"
               " | DXGI-internal CL reset %.2f, close %.2f\n",
            per, dMisc[M_DRAW] / div, dMisc[M_DRAW_IDX] / div, dMisc[M_DISPATCH] / div, dMisc[M_EXEC_INDIRECT] / div,
            dMisc[M_CL_RESET] / div, dMisc[M_CL_CLOSE] / div, dMisc[M_MAP] / div,
            dMisc[M_CL_RESET_DXGI] / div, dMisc[M_CL_CLOSE_DXGI] / div);

    if (g_cfg.constPages) {
        const uint64_t inj = g_fixInjections.load(), reg = g_fixCopyRegions.load(), by = g_fixCopyBytes.load();
        const uint64_t dInj = inj - st.prevFixInj, dReg = reg - st.prevFixRegions, dBy = by - st.prevFixBytes;
        st.prevFixInj = inj; st.prevFixRegions = reg; st.prevFixBytes = by;
        Appendf(s, "  fix ConstPages%s: rootCBV redirected %.2f, left on sysmem %.2f | copy injections %.2f, regions %.2f, %.2f MB"
                   " | twins live %llu (created %llu)%s%s\n",
                per, dMisc[M_FIX_REDIRECT] / div, dMisc[M_FIX_SKIP] / div, dInj / div, dReg / div, MB(dBy) / div,
                g_fixTwinsLive.load(), g_fixTwinsTotal.load(),
                g_fixRedirect.load() ? "" : " | REDIRECTS DISABLED",
                g_fixFailures.load() ? " | FAILURES (see log)" : "");
    }

    if (!g_vidOn.load())
        Appendf(s, "  fix Vidmem: OFF (%s)\n", g_vidOffReason);
    else
        Appendf(s, "  fix Vidmem: ON | live in CPU-visible VRAM %.1f MB | created so far: heaps %llu (%.1f MB), committed buffers %llu (%.1f MB),"
                   " unverified %llu | left in sysmem: no room %llu, NVAPI failed %llu, window full %llu\n",
                MB(g_vidLiveBytes.load()), g_vidHeaps.load(), MB(g_vidHeapBytes.load()),
                g_vidBufs.load(), MB(g_vidBufBytes.load()), g_vidUnverified.load(), g_vidNoRoom.load(), g_vidFailed.load(),
                g_vidIgnored.load());
    AppendHybStats(s, st, dMisc, div, per);
    AppendShadowStats(s, st, div, per);

    if (g_cfg.gpuTiming) {
        GpuAgg g;
        {
            std::lock_guard<std::mutex> lk(g_fixMutex);
            g = g_gpuAgg;
            g_gpuAgg = GpuAgg();
        }
        Appendf(s, "  GPU%s: busy %.2f ms | idle, CPU had not submitted yet %.2f ms | idle, work already submitted %.2f ms"
                   " | submit->start avg %.2f ms, max %.2f | timed submissions %.2f (skipped %llu, bad %llu)\n",
                per, g.busyMs / div, g.starvedMs / div, g.stalledMs / div, g.timed ? g.latSumMs / g.timed : 0.0,
                g.latMaxMs, g.timed / div, g.skipped, g.anomalies);
        for (int k = 0; k < LK_COUNT; ++k) {
            const KindAgg& a = g.kind[k];
            if (!a.lists) continue;
            Appendf(s, "    %-14s lists %6.2f  GPU %6.2f ms", kListKindName[k], a.lists / div, a.ms / div);
            if (a.draws) Appendf(s, "  draws %7.1f (%.1f us/draw)", a.draws / div, a.ms * 1000.0 / a.draws);
            if (a.vbAll) Appendf(s, "  VB views %.1f, in sysmem %.1f", a.vbAll / div, a.vbSys / div);
            if (a.dispatches) Appendf(s, "  dispatches %.1f", a.dispatches / div);
            Appendf(s, "\n");
        }
        std::sort(g.slow, g.slow + g.nSlow, [](const SlowList& a, const SlowList& b) { return a.ms > b.ms; });
        if (g.nSlow) Appendf(s, "    slowest lists (submission#.position in frame):\n");
        for (int i = 0; i < g.nSlow; ++i) {
            const SlowList& e = g.slow[i];
            if (e.seg.copy) { Appendf(s, "      %6.2f ms  %u.%u our copy list\n", e.ms, e.ecl, e.pos); continue; }
            Appendf(s, "      %6.2f ms  %u.%u %s draws %u, VB views %u (sysmem %u), rootCBV %u, dispatches %u, execIndirect %u\n",
                    e.ms, e.ecl, e.pos, e.seg.known ? "" : "(not tracked)", e.seg.c.draws, e.seg.c.vbAll, e.seg.c.vbSys,
                    e.seg.c.rootCbv, e.seg.c.dispatches, e.seg.c.execInd);
        }
    }

    {
        std::string line;
        for (int k = 0; k < AT_COUNT; ++k) {
            const TimeDelta d = TakeDelta(g_apiTime[k]);
            if (d.n) Appendf(line, " | %s %.1f (%.2f ms, max %.2f)", kApiTimeName[k], d.n / div, d.ms / div, d.maxMs);
        }
        if (!line.empty()) Appendf(s, "  CPU in calls%s:%s\n", per, line.c_str() + 2);
    }
    AppendThreadCpu(s, st, div, per);
    if (g_cfg.kmtStats) {
        struct K { const char* name; TimeDelta d; };
        std::vector<K> ks;
        for (int k = 0; k < kKmtCount; ++k) {
            const TimeDelta d = TakeDelta(g_kmt[k]);
            if (d.n) ks.push_back({ kKmtNames[k], d });
        }
        std::sort(ks.begin(), ks.end(), [](const K& a, const K& b) { return a.d.ms > b.d.ms; });
        if (!ks.empty()) Appendf(s, "  kernel (D3DKMT) calls%s, by CPU time:\n", per);
        for (size_t i = 0; i < ks.size(); i += 3) {
            Appendf(s, "   ");
            for (size_t j = i; j < ks.size() && j < i + 3; ++j)
                Appendf(s, " %-34s %7.1f %7.2f ms (max %.2f)", ks[j].name, ks[j].d.n / div, ks[j].d.ms / div, ks[j].d.maxMs);
            Appendf(s, "\n");
        }
    }

    {
        std::lock_guard<std::mutex> lk(g_qMutex);
        for (QueueStats* q : g_queues) {
            const uint64_t e = q->ecl.load(), l = q->lists.load();
            const uint64_t ex = q->eclDxgi.load(), lx = q->listsDxgi.load();
            const uint64_t de = e - q->snapEcl, dl = l - q->snapLists;
            const uint64_t dex = ex - q->snapEclDxgi, dlx = lx - q->snapListsDxgi;
            q->snapEcl = e; q->snapLists = l; q->snapEclDxgi = ex; q->snapListsDxgi = lx;
            if (de || dex)
                Appendf(s, "  queue %p %-7s prio %3d: ExecuteCommandLists %.2f%s, lists %.2f%s | DXGI-internal ECL %.2f, lists %.2f\n",
                        q->q, ListTypeName(q->desc.Type), q->desc.Priority, de / div, per, dl / div, per, dex / div, dlx / div);
        }
    }

    // Bindings table: rows = use kinds, columns = heap categories that appeared.
    bool col[HC_COUNT] = {};
    for (int u = 0; u < U_COUNT; ++u) for (int c = 0; c < HC_COUNT; ++c) if (dUse[u][c]) col[c] = true;
    bool anyCol = false;
    for (int c = 0; c < HC_COUNT; ++c) anyCol |= col[c];
    if (anyCol) {
        Appendf(s, "  %-12s", per[1] == 'f' ? "uses/frame" : "uses/s");
        for (int c = 0; c < HC_COUNT; ++c) if (col[c]) Appendf(s, " %11s", kCatName[c]);
        Appendf(s, " %8s\n", "sysmem%");
    }
    for (int u = 0; u < U_COUNT; ++u) {
        uint64_t tot = 0, sys = 0;
        for (int c = 0; c < HC_COUNT; ++c) { tot += dUse[u][c]; if (IsSysmem(c)) sys += dUse[u][c]; }
        if (!tot) continue;
        Appendf(s, "  %-12s", kUseName[u]);
        for (int c = 0; c < HC_COUNT; ++c) if (col[c]) Appendf(s, " %11.2f", dUse[u][c] / div);
        Appendf(s, " %7.1f%%\n", 100.0 * sys / tot);
    }

    // Buffer population.
    std::string pop, created;
    for (int c = 0; c < HC_COUNT; ++c) {
        const uint64_t cr = g_bufCreated[c].load(), crb = g_bufCreatedBytes[c].load();
        const uint64_t de = g_bufDestroyed[c].load(), deb = g_bufDestroyedBytes[c].load();
        const uint64_t hp = g_heapCreated[c].load();
        if (cr - de) Appendf(pop, " %s %llu (%.1f MB)", kCatName[c], cr - de, MB(crb - deb));
        if (cr != st.prevCreated[c] || de != st.prevDestroyed[c] || hp != st.prevHeaps[c])
            Appendf(created, " %s +%llu (%.2f MB) -%llu heaps+%llu", kCatName[c], cr - st.prevCreated[c],
                    MB(crb - st.prevCreatedBytes[c]), de - st.prevDestroyed[c], hp - st.prevHeaps[c]);
        st.prevCreated[c] = cr; st.prevCreatedBytes[c] = crb; st.prevDestroyed[c] = de; st.prevHeaps[c] = hp;
    }
    Appendf(s, "  live buffers:%s\n", pop.empty() ? " none" : pop.c_str());
    if (!created.empty()) Appendf(s, "  buffer churn this interval:%s\n", created.c_str());

    // Sysmem bytes referenced by GPU-read bindings per submission window
    // (between game ExecuteCommandLists calls) = what stage 2 would copy.
    {
        uint64_t wb[WC_COUNT], wn[WC_COUNT];
        for (int c = 0; c < WC_COUNT; ++c) {
            const uint64_t b = g_winBytes[c].load(), n = g_winBufs[c].load();
            wb[c] = b - st.prevWinBytes[c]; wn[c] = n - st.prevWinBufs[c];
            st.prevWinBytes[c] = b; st.prevWinBufs[c] = n;
        }
        const uint64_t w = g_winCount.load(), dw = w - st.prevWinCount;
        st.prevWinCount = w;
        Appendf(s, "  sysmem GPU-read ranges%s: big(>=1MB) %.1f bufs %.2f MB | small %.1f bufs %.2f MB | total %.2f MB (submissions %.2f%s)\n",
                per, wn[WC_BIG] / div, MB(wb[WC_BIG]) / div, wn[WC_SMALL] / div, MB(wb[WC_SMALL]) / div,
                MB(wb[WC_BIG] + wb[WC_SMALL]) / div, dw / div, per);
    }

    // Sysmem buffers created this interval, by what the GPU did with them.
    {
        enum { NB_GPU, NB_COPY, NB_UNUSED, NB_COUNT };
        uint64_t cnt[NB_COUNT] = {}, bytes[NB_COUNT] = {};
        uint64_t byUse[U_COUNT] = {};       // GPU-read buffers having this use at least once
        uint64_t hist[4] = {}, histB[4] = {}; // GPU-read sizes: <4K, <64K, <1M, >=1M
        AcquireSRWLockShared(&g_resLock);
        const size_t n = g_allRecs.size();
        for (size_t i = st.prevRecIndex; i < n; ++i) {
            const ResRec* r = g_allRecs[i];
            if (!IsSysmem(r->cat)) continue;
            uint64_t gpu = 0, cp = 0;
            for (int u = 0; u < U_COUNT; ++u) {
                const uint64_t v = r->use[u].load(std::memory_order_relaxed);
                if (IsGpuRead(u)) { gpu += v; if (v) ++byUse[u]; } else cp += v;
            }
            const int cls = gpu ? NB_GPU : cp ? NB_COPY : NB_UNUSED;
            ++cnt[cls]; bytes[cls] += r->size;
            if (cls == NB_GPU) {
                const int h = r->size < 4096 ? 0 : r->size < 65536 ? 1 : r->size < (1u << 20) ? 2 : 3;
                ++hist[h]; histB[h] += r->size;
            }
        }
        st.prevRecIndex = n;
        ReleaseSRWLockShared(&g_resLock);
        if (cnt[NB_GPU] + cnt[NB_COPY] + cnt[NB_UNUSED]) {
            Appendf(s, "  new sysmem buffers%s: GPU-read %.1f (%.2f MB) | copy-only %.1f (%.2f MB) | unused %.1f (%.2f MB)\n",
                    per, cnt[NB_GPU] / div, MB(bytes[NB_GPU]) / div, cnt[NB_COPY] / div, MB(bytes[NB_COPY]) / div,
                    cnt[NB_UNUSED] / div, MB(bytes[NB_UNUSED]) / div);
            if (cnt[NB_GPU]) {
                Appendf(s, "    GPU-read ones used as:");
                for (int u = 0; u < U_COUNT; ++u) if (IsGpuRead(u) && byUse[u]) Appendf(s, " %s %.1f", kUseName[u], byUse[u] / div);
                Appendf(s, " | sizes <4K %.1f (%.2f MB), <64K %.1f (%.2f MB), <1M %.1f (%.2f MB), >=1M %.1f (%.2f MB)\n",
                        hist[0] / div, MB(histB[0]) / div, hist[1] / div, MB(histB[1]) / div,
                        hist[2] / div, MB(histB[2]) / div, hist[3] / div, MB(histB[3]) / div);
            }
        }
    }

    // Top non-DEFAULT buffers by GPU-side uses this interval.
    struct Top { uint64_t total; ResRec* r; uint64_t d[U_COUNT]; uint64_t dMaps; };
    std::vector<Top> tops;
    AcquireSRWLockShared(&g_resLock);
    for (ResRec* r : g_allRecs) {
        if (r->cat == HC_DEFAULT) continue;
        Top tp = {}; tp.r = r;
        for (int u = 0; u < U_COUNT; ++u) {
            const uint64_t v = r->use[u].load(std::memory_order_relaxed);
            tp.d[u] = v - r->snap[u]; r->snap[u] = v;
            if (u != U_COPY_DST) tp.total += tp.d[u];
        }
        const uint64_t m = r->maps.load(std::memory_order_relaxed);
        tp.dMaps = m - r->snapMaps; r->snapMaps = m;
        if (tp.total || tp.dMaps) tops.push_back(tp);
    }
    ReleaseSRWLockShared(&g_resLock);
    std::sort(tops.begin(), tops.end(), [](const Top& a, const Top& b) { return a.total > b.total; });
    if (!tops.empty()) {
        Appendf(s, "  top non-DEFAULT buffers (%zu used this interval):\n", tops.size());
        for (size_t i = 0; i < tops.size() && (int)i < g_cfg.topN; ++i) {
            const Top& tp = tops[i];
            Appendf(s, "    #%llu %s %s %.2f MB va 0x%llx%s:", tp.r->id, kCatName[tp.r->cat], tp.r->how,
                    MB(tp.r->size), tp.r->va, tp.r->alive.load() ? "" : " (dead)");
            for (int u = 0; u < U_COUNT; ++u) if (tp.d[u]) Appendf(s, " %s %.2f", kUseName[u], tp.d[u] / div);
            if (tp.dMaps) Appendf(s, " Map %.2f", tp.dMaps / div);
            Appendf(s, " (%s)\n", per);
        }
    }
    LogRaw(s.c_str());
}

static DWORD WINAPI StatsThread(LPVOID)
{
    LogRaw("  legend: 'uses' = GPU VA bindings counted at record time; sysmem% = share of UPLOAD+READBACK+CUSTOM_SYS;\n"
           "          'untracked' = VA not inside any buffer created after the hooks were installed.\n"
           "          Pending per-buffer counts are flushed on CommandList::Close, so the per-buffer list may lag slightly.\n");
    StatsState st;
    QueryPerformanceCounter(&st.prevT);
    st.prevFrames = g_frames.load();
    for (;;) {
        Sleep((DWORD)g_cfg.statsIntervalSec * 1000);
        DumpStats(st);
    }
}

// ============================================================================
// Device creation
// ============================================================================
static std::atomic<bool> g_deviceHooksInstalled{false};
static std::atomic<bool> g_statsStarted{false};

static void LogAdapter(const char* prefix, IUnknown* adapterUnk)
{
    IDXGIAdapter1* a = nullptr;
    if (!adapterUnk || FAILED(adapterUnk->QueryInterface(IID_PPV_ARGS(&a))) || !a) {
        Log("%s adapter: %s", prefix, adapterUnk ? "(not an IDXGIAdapter1)" : "(default)");
        return;
    }
    DXGI_ADAPTER_DESC1 d = {};
    a->GetDesc1(&d);
    Log("%s adapter: %ls (vendor 0x%04x device 0x%04x) dedicated VRAM %.0f MB, shared %.0f MB, LUID %08lx:%08lx",
        prefix, d.Description, d.VendorId, d.DeviceId, MB(d.DedicatedVideoMemory), MB(d.SharedSystemMemory),
        (unsigned long)d.AdapterLuid.HighPart, d.AdapterLuid.LowPart);
    a->Release();
}

static void LogDeviceInfo(ID3D12Device* dev)
{
    const LUID luid = dev->GetAdapterLuid();

    if (PFN_CreateDXGIFactory f1 = (PFN_CreateDXGIFactory)GetProcAddress(GetModuleHandleW(L"dxgi.dll"), "CreateDXGIFactory1")) {
        IDXGIFactory4* f = nullptr;
        if (SUCCEEDED(f1(IID_PPV_ARGS(&f))) && f) {
            IDXGIAdapter3* a = nullptr;
            if (SUCCEEDED(f->EnumAdapterByLuid(luid, IID_PPV_ARGS(&a))) && a) {
                LogAdapter("  device", a);
                DXGI_QUERY_VIDEO_MEMORY_INFO loc = {}, non = {};
                a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &loc);
                a->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &non);
                Log("  memory budget: local %.0f MB (used %.0f MB), non-local %.0f MB (used %.0f MB)",
                    MB(loc.Budget), MB(loc.CurrentUsage), MB(non.Budget), MB(non.CurrentUsage));
                a->Release();
            }
            f->Release();
        }
    }

    D3D12_FEATURE_DATA_ARCHITECTURE1 arch = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE1, &arch, sizeof(arch))))
        Log("  architecture: UMA=%d CacheCoherentUMA=%d IsolatedMMU=%d TileBasedRenderer=%d",
            arch.UMA, arch.CacheCoherentUMA, arch.IsolatedMMU, arch.TileBasedRenderer);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))))
        Log("  ResourceBindingTier=%d ResourceHeapTier=%d", (int)o.ResourceBindingTier, (int)o.ResourceHeapTier);
    D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))))
        Log("  EnhancedBarriersSupported=%d", o12.EnhancedBarriersSupported);
    D3D12_FEATURE_DATA_D3D12_OPTIONS16 o16 = {};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS16, &o16, sizeof(o16))))
        Log("  GPUUploadHeapSupported=%d (needs ReBAR)", o16.GPUUploadHeapSupported);
    else
        Log("  OPTIONS16 not supported by runtime");

    if (HMODULE core = GetModuleHandleW(L"D3D12Core.dll")) {
        // Game folder or System32 only (users attach this log publicly: no user folders).
        wchar_t path[MAX_PATH], exeDir[MAX_PATH], sys[MAX_PATH];
        GetModuleFileNameW(core, path, MAX_PATH);
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        if (wchar_t* sl = wcsrchr(exeDir, L'\\')) sl[1] = 0;
        GetSystemDirectoryW(sys, MAX_PATH);
        const size_t n = wcslen(exeDir), ns = wcslen(sys);
        if (n && _wcsnicmp(path, exeDir, n) == 0)      Log("  D3D12Core: <game folder>\\%ls", path + n);
        else if (ns && _wcsnicmp(path, sys, ns) == 0)  Log("  D3D12Core: %ls", path);
        else                                           Log("  D3D12Core: <other folder>\\D3D12Core.dll");
    }
    if (const UINT* sdk = (const UINT*)GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion"))
        Log("  exe D3D12SDKVersion export: %u", *sdk);
}

template <template <int> class H>
static void InstallForType(int v, void** vt, const char* typeName)
{
    switch (v) {
    case 0: H<0>::Install(vt, typeName); break;
    case 1: H<1>::Install(vt, typeName); break;
    case 2: H<2>::Install(vt, typeName); break;
    case 3: H<3>::Install(vt, typeName); break;
    case 4: H<4>::Install(vt, typeName); break;
    case 5: H<5>::Install(vt, typeName); break;
    case 6: H<6>::Install(vt, typeName); break;
    case 7: H<7>::Install(vt, typeName); break;
    case 8: H<8>::Install(vt, typeName); break;
    case 9: H<9>::Install(vt, typeName); break;
    }
}
static constexpr int kMaxListVariants = 10;

// ---- Command list vtable discovery ------------------------------------------------
// The Agility runtime gives every command list its own heap copy of the vtable,
// with hot methods pointing straight into the driver (nvwgf2umx.dll). So we key
// on the slot *targets*, not on the vtable address: a vtable needs hooking only
// if one of its slots points to a function we have not hooked yet.
static constexpr int kListSlots = 14;
static constexpr int kMaxTargetsPerSlot = 8;
static std::atomic<void*> g_slotTargets[kListSlots][kMaxTargetsPerSlot];
static std::atomic<int> g_slotTargetCount[kListSlots];
static int g_listVariantsUsed = 0; // guarded by g_installMutex

static int ListSlotIndex(int i)
{
    const int idx[kListSlots] = {
        g_vtbl.CL_SetGraphicsRootConstantBufferView, g_vtbl.CL_SetComputeRootConstantBufferView,
        g_vtbl.CL_SetGraphicsRootShaderResourceView, g_vtbl.CL_SetComputeRootShaderResourceView,
        g_vtbl.CL_IASetVertexBuffers, g_vtbl.CL_IASetIndexBuffer, g_vtbl.CL_DrawInstanced,
        g_vtbl.CL_DrawIndexedInstanced, g_vtbl.CL_Dispatch, g_vtbl.CL_ExecuteIndirect,
        g_vtbl.CL_CopyBufferRegion, g_vtbl.CL_CopyResource, g_vtbl.CL_Close, g_vtbl.CL_Reset };
    return idx[i];
}

static bool AllListTargetsKnown(void** vt)
{
    for (int i = 0; i < kListSlots; ++i) {
        void* t = vt[ListSlotIndex(i)];
        const int n = g_slotTargetCount[i].load(std::memory_order_acquire);
        bool found = false;
        for (int k = 0; k < n && !found; ++k) found = g_slotTargets[i][k].load(std::memory_order_relaxed) == t;
        if (!found) return false;
    }
    return true;
}

static void RememberListTargets(void** vt) // caller holds g_installMutex
{
    for (int i = 0; i < kListSlots; ++i) {
        void* t = vt[ListSlotIndex(i)];
        const int n = g_slotTargetCount[i].load(std::memory_order_relaxed);
        bool found = false;
        for (int k = 0; k < n && !found; ++k) found = g_slotTargets[i][k].load(std::memory_order_relaxed) == t;
        if (!found && n < kMaxTargetsPerSlot) {
            g_slotTargets[i][n].store(t, std::memory_order_relaxed);
            g_slotTargetCount[i].store(n + 1, std::memory_order_release);
        }
    }
}

static void CheckListVtable(ID3D12GraphicsCommandList* cl)
{
    void** vt = *reinterpret_cast<void***>(cl);
    if (AllListTargetsKnown(vt)) return;
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    if (AllListTargetsKnown(vt)) return;
    if (g_listVariantsUsed >= kMaxListVariants) {
        static bool warned = false;
        if (!warned) { warned = true; Log("command list vtable with unhooked targets, but no free hook variants left"); }
        RememberListTargets(vt); // don't come back here for these targets
        return;
    }
    const D3D12_COMMAND_LIST_TYPE type = cl->GetType();
    Log("command list vtable %s with new targets (list %p, type %s, seen in Reset/Close)",
        DescribeAddr(vt).c_str(), cl, ListTypeName(type));
    char name[48];
    sprintf_s(name, "%s/vt#%d", ListTypeName(type), g_listVariantsUsed);
    InstallForType<ListHooks>(g_listVariantsUsed++, vt, name);
    RememberListTargets(vt);
    ApplyHooks();
}

// Temporary queues and command lists, only to reach their vtables and hook
// ExecuteCommandLists and the command list methods (caller holds g_installMutex;
// hooks are queued, not enabled yet, so these creations are not recorded).
static void InstallQueueAndListHooks(ID3D12Device* dev, bool lists = true)
{
    static const D3D12_COMMAND_LIST_TYPE kQueueTypes[] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY };
    for (int i = 0; i < 3; ++i) {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = kQueueTypes[i];
        ID3D12CommandQueue* q = nullptr;
        HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
        if (FAILED(hr) || !q) { Log("  temp %s queue failed: 0x%08lx", ListTypeName(qd.Type), hr); continue; }
        InstallForType<QueueHooks>(i, *reinterpret_cast<void***>(q), ListTypeName(qd.Type));
        q->Release();
    }
    if (!lists) return;
    static const D3D12_COMMAND_LIST_TYPE kListTypes[] = {
        D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE, D3D12_COMMAND_LIST_TYPE_COPY,
        D3D12_COMMAND_LIST_TYPE_BUNDLE };
    for (int i = 0; i < 4; ++i) {
        ID3D12CommandAllocator* alloc = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        HRESULT hr = dev->CreateCommandAllocator(kListTypes[i], IID_PPV_ARGS(&alloc));
        if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, kListTypes[i], alloc, nullptr, IID_PPV_ARGS(&cl));
        if (FAILED(hr) || !cl) {
            Log("  temp %s command list failed: 0x%08lx", ListTypeName(kListTypes[i]), hr);
        } else {
            void** lvt = *reinterpret_cast<void***>(cl);
            Log("  temp %s command list vtable %s", ListTypeName(kListTypes[i]), DescribeAddr(lvt).c_str());
            InstallForType<ListHooks>(i, lvt, ListTypeName(kListTypes[i]));
            RememberListTargets(lvt);
            g_listVariantsUsed = i + 1;
            cl->Close();
            cl->Release();
        }
        if (alloc) alloc->Release();
    }
}

static void InstallDeviceHooks(ID3D12Device* dev)
{
    std::lock_guard<std::recursive_mutex> lk(g_installMutex);
    void** vt = *reinterpret_cast<void***>(dev);
    if (!g_cfg.measure) {
        // Fix-only mode ([General] Measure=0): just the calls stage 2c / 2d need.
        Log("installing device hooks (fix-only mode: no measurement%s)",
            g_cfg.hybridHeaps ? "; stage 2d: placed resources, SRVs, VB/IB, list Reset/Close, ExecuteCommandLists"
            : g_cfg.shadowHeaps ? "; stage 2e: Map/Unmap, GetHeapProperties, Heap::GetDesc, ExecuteCommandLists"
                                : ": no binding/draw/submit hooks");
        HookVT(vt, g_vtbl.Device_CreateCommittedResource, (void*)&Hook_CCR, (void**)&o_CCR, "Device::CreateCommittedResource");
        HookVT(vt, g_vtbl.Device_CreateHeap, (void*)&Hook_CH, (void**)&o_CH, "Device::CreateHeap");
        if (g_cfg.hybridHeaps) {
            HookVT(vt, g_vtbl.Device_CreatePlacedResource, (void*)&Hook_CPR, (void**)&o_CPR, "Device::CreatePlacedResource");
            if (g_cfg.hybridSrv)
                HookVT(vt, g_vtbl.Device_CreateShaderResourceView, (void*)&Hook_CSRV, (void**)&o_CSRV, "Device::CreateShaderResourceView");
        }
        ID3D12Device4* m4 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&m4))) && m4) {
            void** v4 = *reinterpret_cast<void***>(m4);
            HookVT(v4, g_vtbl.Device4_CreateCommittedResource1, (void*)&Hook_CCR1, (void**)&o_CCR1, "Device4::CreateCommittedResource1");
            HookVT(v4, g_vtbl.Device4_CreateHeap1, (void*)&Hook_CH1, (void**)&o_CH1, "Device4::CreateHeap1");
            m4->Release();
        }
        ID3D12Device8* m8 = nullptr;
        if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&m8))) && m8) {
            void** v8 = *reinterpret_cast<void***>(m8);
            HookVT(v8, g_vtbl.Device8_CreateCommittedResource2, (void*)&Hook_CCR2, (void**)&o_CCR2, "Device8::CreateCommittedResource2");
            if (g_cfg.hybridHeaps)
                HookVT(v8, g_vtbl.Device8_CreatePlacedResource1, (void*)&Hook_CPR1, (void**)&o_CPR1, "Device8::CreatePlacedResource1");
            m8->Release();
        }
        if (g_cfg.hybridHeaps) {
            ID3D12Device10* m10 = nullptr;
            if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&m10))) && m10) {
                void** v10 = *reinterpret_cast<void***>(m10);
                HookVT(v10, g_vtbl.Device10_CreatePlacedResource2, (void*)&Hook_CPR2, (void**)&o_CPR2, "Device10::CreatePlacedResource2");
                m10->Release();
            }
            InstallQueueAndListHooks(dev);
        } else if (g_cfg.shadowHeaps) {
            InstallQueueAndListHooks(dev, false); // stage 2e needs ExecuteCommandLists only
        }
        if (g_cfg.shadowHeaps && !InstallShadowHooks(dev)) ShHooksFailed();
        ApplyHooks();
        Log("device hooks installed");
        return;
    }
    Log("installing device hooks");
    HookVT(vt, g_vtbl.Device_CreateCommittedResource, (void*)&Hook_CCR, (void**)&o_CCR, "Device::CreateCommittedResource");
    HookVT(vt, g_vtbl.Device_CreatePlacedResource, (void*)&Hook_CPR, (void**)&o_CPR, "Device::CreatePlacedResource");
    HookVT(vt, g_vtbl.Device_CreateHeap, (void*)&Hook_CH, (void**)&o_CH, "Device::CreateHeap");
    HookVT(vt, g_vtbl.Device_CreateConstantBufferView, (void*)&Hook_CCBV, (void**)&o_CCBV, "Device::CreateConstantBufferView");
    HookVT(vt, g_vtbl.Device_CreateShaderResourceView, (void*)&Hook_CSRV, (void**)&o_CSRV, "Device::CreateShaderResourceView");

    ID3D12Device4* d4 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d4))) && d4) {
        void** v4 = *reinterpret_cast<void***>(d4);
        HookVT(v4, g_vtbl.Device4_CreateCommittedResource1, (void*)&Hook_CCR1, (void**)&o_CCR1, "Device4::CreateCommittedResource1");
        HookVT(v4, g_vtbl.Device4_CreateHeap1, (void*)&Hook_CH1, (void**)&o_CH1, "Device4::CreateHeap1");
        d4->Release();
    }
    ID3D12Device8* d8 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d8))) && d8) {
        void** v8 = *reinterpret_cast<void***>(d8);
        HookVT(v8, g_vtbl.Device8_CreateCommittedResource2, (void*)&Hook_CCR2, (void**)&o_CCR2, "Device8::CreateCommittedResource2");
        HookVT(v8, g_vtbl.Device8_CreatePlacedResource1, (void*)&Hook_CPR1, (void**)&o_CPR1, "Device8::CreatePlacedResource1");
        d8->Release();
    }
    ID3D12Device10* d10 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d10))) && d10) {
        void** v10 = *reinterpret_cast<void***>(d10);
        HookVT(v10, g_vtbl.Device10_CreateCommittedResource3, (void*)&Hook_CCR3, (void**)&o_CCR3, "Device10::CreateCommittedResource3");
        HookVT(v10, g_vtbl.Device10_CreatePlacedResource2, (void*)&Hook_CPR2, (void**)&o_CPR2, "Device10::CreatePlacedResource2");
        d10->Release();
    }
    InstallQueueAndListHooks(dev);
    if (g_cfg.shadowHeaps && !InstallShadowHooks(dev)) ShHooksFailed();
    ApplyHooks();
    Log("device hooks installed");
}

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
static PFN_D3D12CreateDevice o_D3D12CreateDevice;

static HRESULT WINAPI Hook_D3D12CreateDevice(IUnknown* adapter, D3D_FEATURE_LEVEL fl, REFIID riid, void** ppDevice)
{
    HRESULT hr = o_D3D12CreateDevice(adapter, fl, riid, ppDevice);
    Log("D3D12CreateDevice(fl=0x%x, %s) -> 0x%08lx", (unsigned)fl, ppDevice ? "create" : "probe only", hr);
    LogAdapter("  requested", adapter);
    if (FAILED(hr) || !ppDevice || !*ppDevice) return hr;

    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(static_cast<IUnknown*>(*ppDevice)->QueryInterface(IID_PPV_ARGS(&dev))) && dev) {
        Log("  device %p", dev);
        LogDeviceInfo(dev);
        {
            std::lock_guard<std::recursive_mutex> lk(g_installMutex);
            InitVidmem(dev); // for the first device: before its hooks go live
        }
        if (!g_deviceHooksInstalled.exchange(true)) InstallDeviceHooks(dev);
        if (!g_statsStarted.exchange(true)) {
            HANDLE th = CreateThread(nullptr, 0, StatsThread, nullptr, 0, nullptr);
            if (th) CloseHandle(th);
        }
        dev->Release();
    }
    return hr;
}

// ============================================================================
// Entry point from DllMain
// ============================================================================
bool InstallInitialHooks()
{
    QueryPerformanceFrequency(&g_qpf);
    g_fixRedirect.store(g_cfg.constPages);
    if (g_cfg.constPages)
        Log("fix: ConstPages ON: DEFAULT twins for %llu-byte UPLOAD committed buffers, root CBV pad %d bytes",
            kConstPageSize, g_cfg.cbvPad);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK) { Log("MH_Initialize failed: %s", MH_StatusToString(s)); return false; }

    if (g_cfg.kmtStats) {
        HMODULE w32u = GetModuleHandleW(L"win32u.dll");
        if (!w32u) w32u = LoadLibraryW(L"win32u.dll");
        if (w32u) InstallKmtHooks(w32u, std::make_index_sequence<kKmtCount>{});
        else Log("kmt: win32u.dll not found");
    }

    Log("vtable slots: CCR=%d CPR=%d CCBV=%d ECL=%d SetGfxRootCBV=%d IASetVB=%d Present=%d Present1=%d",
        g_vtbl.Device_CreateCommittedResource, g_vtbl.Device_CreatePlacedResource, g_vtbl.Device_CreateConstantBufferView,
        g_vtbl.Queue_ExecuteCommandLists, g_vtbl.CL_SetGraphicsRootConstantBufferView, g_vtbl.CL_IASetVertexBuffers,
        g_vtbl.SwapChain_Present, g_vtbl.SwapChain1_Present1);

    wchar_t sys[MAX_PATH], path[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);

    swprintf_s(path, L"%s\\d3d12.dll", sys);
    HMODULE d3d12 = LoadLibraryW(path);
    swprintf_s(path, L"%s\\dxgi.dll", sys);
    HMODULE dxgi = LoadLibraryW(path);
    if (!d3d12 || !dxgi) { Log("failed to load system d3d12.dll/dxgi.dll"); return false; }

    HookTarget((void*)GetProcAddress(d3d12, "D3D12CreateDevice"), (void*)&Hook_D3D12CreateDevice,
               (void**)&o_D3D12CreateDevice, "D3D12CreateDevice");
    HookTarget((void*)GetProcAddress(dxgi, "CreateDXGIFactory"), (void*)&Hook_CreateDXGIFactory,
               (void**)&o_CreateDXGIFactory, "CreateDXGIFactory");
    HookTarget((void*)GetProcAddress(dxgi, "CreateDXGIFactory1"), (void*)&Hook_CreateDXGIFactory1,
               (void**)&o_CreateDXGIFactory1, "CreateDXGIFactory1");
    HookTarget((void*)GetProcAddress(dxgi, "CreateDXGIFactory2"), (void*)&Hook_CreateDXGIFactory2,
               (void**)&o_CreateDXGIFactory2, "CreateDXGIFactory2");
    ApplyHooks();
    Log("initial hooks installed");
    return true;
}
