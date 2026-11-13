#pragma once
#include <windows.h>
#include <stdint.h>
#include "egpu_version.h"

// ---- Logging (version_proxy.cpp) --------------------------------------------
// Log() prefixes "[hh:mm:ss.mmm][tid] " and appends '\n'. LogRaw() writes text as is.
void Log(const char* fmt, ...);
void LogRaw(const char* text);

// ---- Settings from egpu_fix.ini next to the DLL -------------------------------
// The defaults below are the release behaviour (no ini / missing key).
struct Config {
    bool loadPix          = false; // [General] LoadPix: load the PIX GPU capturer (developer tool)
    wchar_t pixDll[MAX_PATH] = {}; // [General] PixDll: capturer path; empty = newest under %ProgramFiles%\Microsoft PIX
    bool hooks            = true;  // [General] Hooks
    bool measure          = false; // [General] Measure: 0 = fix-only mode (only the hooks the fixes need, FPS stats)
    int  statsIntervalSec = 5;     // [Stats] IntervalSec
    int  logCreateLimit   = 300;   // [Stats] LogCreateLimit: individual "create" lines for sysmem buffers
    int  topN             = 8;     // [Stats] TopN: sysmem buffers listed per stats block
    bool gpuTiming        = true;  // [Stats] GpuTiming: GPU timestamps around every game command list (Measure=1 only)
    bool kmtStats         = false; // [Stats] KmtStats: count/time D3DKMT kernel thunks (win32u, Measure=1 only)
    bool constPages       = false; // [Fix] ConstPages: DEFAULT twins for the 4 MB UPLOAD constant pages (stage 2a)
    int  cbvPad           = 65536; // [Fix] CbvPad: bytes assumed readable after a root CBV address (CB size is unknown)
    bool vidmemHeaps      = true;  // [Fix] VidmemHeaps: UPLOAD heaps -> CPU-visible VRAM via NVAPI (stage 2c)
    bool vidmemBuffers    = false; // [Fix] VidmemBuffers: committed UPLOAD buffers -> CPU-visible VRAM via NVAPI
    int  vidmemBufMaxKB   = 4096;  // [Fix] VidmemBufMaxKB: only committed UPLOAD buffers up to this size
    int  vidmemReserveMB  = 32;    // [Fix] VidmemReserveMB: CPU-visible VRAM always left free (if the driver reports it)
    int  vidmemCapMB      = 192;   // [Fix] VidmemCapMB: max of our live CPU-visible VRAM (window ~192 MB free on this system)
    bool vidmemWithReBAR  = false; // [Fix] VidmemWithReBAR: also run stage 2c when the GPU has ReBAR (untested)
    bool hybridHeaps      = false; // [Fix] HybridHeaps: UPLOAD heaps not placed in VRAM by 2c get a DEFAULT mirror (stage 2d)
    bool hybridSrv        = false; // [Fix] HybridSrv: also redirect buffer SRV descriptors into the mirror
    int  hybridSrvCopies  = 2;     // [Fix] HybridSrvCopies: game submissions at which a new SRV's range is copied
    int  hybridMergeKB    = 16;    // [Fix] HybridMergeKB: copy ranges closer than this are merged into one copy
    bool hybridCopyQueue  = false; // [Fix] HybridCopyQueue: copies on our own COPY queue, overlapping the game's rendering
    bool hybridCheck      = false; // [Fix] HybridCheck: hash mirrored buffers, count/repair stale mirror data (Measure=1 only)
    bool hybridNoCopy     = false; // [Fix] HybridTestNoCopy: redirect but never copy (negative test, must break rendering)
};
extern Config g_cfg;

// ---- hooks.cpp ---------------------------------------------------------------
bool InstallInitialHooks(); // called from DllMain: D3D12CreateDevice + CreateDXGIFactory*
