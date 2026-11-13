// version.dll proxy for No Rest for the Wicked (NRFTW eGPU Fix).
//
//   1. Every version.dll export is forwarded unchanged to the real
//      C:\Windows\System32\version.dll (/export pragmas below).
//   2. Reads egpu_fix.ini next to itself (every key is optional; see Config in common.h):
//        [General] Hooks=1     -> install the D3D12/DXGI hooks (hooks.cpp)
//        [General] Measure=0/1 -> fix only (default) / full measurement for bug reports
//        [General] LoadPix=1   -> load WinPixGpuCapturer.dll before any D3D12 call
//                                 (developer tool: lets PIX attach to the Steam-launched game)
//        [Fix] VidmemHeaps=1   -> stage 2c: UPLOAD heaps in CPU-visible VRAM (NVAPI, default on)
//        [Fix] ConstPages=1    -> stage 2a: DEFAULT-heap twins for the constant pages (experimental)
//   3. Writes egpu_proxy.log next to itself (previous run -> egpu_proxy.prev.log).
//
// Remove: delete version.dll, egpu_fix.ini and the logs from the game folder.

#include "common.h"
#include <stdio.h>
#include <stdarg.h>
#include <share.h>

// ---- Forward all real version.dll exports to the system copy ---------------
#define FWD(name) __pragma(comment(linker, "/export:" #name "=C:\\Windows\\System32\\version." #name))
FWD(GetFileVersionInfoA)
FWD(GetFileVersionInfoByHandle)
FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW)
FWD(GetFileVersionInfoSizeA)
FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW)
FWD(GetFileVersionInfoSizeW)
FWD(GetFileVersionInfoW)
FWD(VerFindFileA)
FWD(VerFindFileW)
FWD(VerInstallFileA)
FWD(VerInstallFileW)
FWD(VerLanguageNameA)
FWD(VerLanguageNameW)
FWD(VerQueryValueA)
FWD(VerQueryValueW)

Config g_cfg;

// ---- Logging ----------------------------------------------------------------
static CRITICAL_SECTION g_logCs;
static FILE* g_log = nullptr;

void LogRaw(const char* text)
{
    if (!g_log) return;
    EnterCriticalSection(&g_logCs);
    fputs(text, g_log);
    fflush(g_log);
    LeaveCriticalSection(&g_logCs);
}

void Log(const char* fmt, ...)
{
    char buf[4096];
    SYSTEMTIME t; GetLocalTime(&t);
    _snprintf_s(buf, sizeof(buf), _TRUNCATE, "[%02d:%02d:%02d.%03d][%5lu] ",
                t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId());
    size_t n = strlen(buf);
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf + n, sizeof(buf) - n - 1, _TRUNCATE, fmt, ap);
    va_end(ap);
    strcat_s(buf, "\n");
    LogRaw(buf);
}

// ---- Helpers ----------------------------------------------------------------
static bool EndsWithI(const wchar_t* s, const wchar_t* suffix)
{
    size_t ls = wcslen(s), lx = wcslen(suffix);
    return ls >= lx && _wcsicmp(s + ls - lx, suffix) == 0;
}

// Bool/int keys: a missing key keeps the code default from Config.
static bool IniBool(const wchar_t* sec, const wchar_t* key, bool def, const wchar_t* ini)
{
    return GetPrivateProfileIntW(sec, key, def ? 1 : 0, ini) != 0;
}
static int IniInt(const wchar_t* sec, const wchar_t* key, int def, const wchar_t* ini)
{
    return (int)GetPrivateProfileIntW(sec, key, def, ini);
}

static void ReadConfig(const wchar_t* ini)
{
    const Config d; // code defaults
    g_cfg.loadPix          = IniBool(L"General", L"LoadPix", d.loadPix, ini);
    GetPrivateProfileStringW(L"General", L"PixDll", L"", g_cfg.pixDll, MAX_PATH, ini);
    g_cfg.hooks            = IniBool(L"General", L"Hooks", d.hooks, ini);
    g_cfg.measure          = IniBool(L"General", L"Measure", d.measure, ini);
    g_cfg.statsIntervalSec = IniInt(L"Stats", L"IntervalSec", d.statsIntervalSec, ini);
    g_cfg.logCreateLimit   = IniInt(L"Stats", L"LogCreateLimit", d.logCreateLimit, ini);
    g_cfg.topN             = IniInt(L"Stats", L"TopN", d.topN, ini);
    g_cfg.gpuTiming        = IniBool(L"Stats", L"GpuTiming", d.gpuTiming, ini);
    g_cfg.kmtStats         = IniBool(L"Stats", L"KmtStats", d.kmtStats, ini);
    g_cfg.constPages       = IniBool(L"Fix", L"ConstPages", d.constPages, ini);
    g_cfg.cbvPad           = IniInt(L"Fix", L"CbvPad", d.cbvPad, ini);
    g_cfg.vidmemHeaps      = IniBool(L"Fix", L"VidmemHeaps", d.vidmemHeaps, ini);
    g_cfg.vidmemBuffers    = IniBool(L"Fix", L"VidmemBuffers", d.vidmemBuffers, ini);
    g_cfg.vidmemBufMaxKB   = IniInt(L"Fix", L"VidmemBufMaxKB", d.vidmemBufMaxKB, ini);
    g_cfg.vidmemReserveMB  = IniInt(L"Fix", L"VidmemReserveMB", d.vidmemReserveMB, ini);
    g_cfg.vidmemCapMB      = IniInt(L"Fix", L"VidmemCapMB", d.vidmemCapMB, ini);
    g_cfg.vidmemWithReBAR  = IniBool(L"Fix", L"VidmemWithReBAR", d.vidmemWithReBAR, ini);
    if (g_cfg.vidmemReserveMB < 0) g_cfg.vidmemReserveMB = 0;
    if (g_cfg.vidmemCapMB < 0) g_cfg.vidmemCapMB = 0;
    if (!g_cfg.measure) {
        // Fix-only mode: these need the binding/submit hooks, which are not installed.
        g_cfg.gpuTiming = g_cfg.kmtStats = g_cfg.constPages = false;
    }
    if (g_cfg.statsIntervalSec < 1) g_cfg.statsIntervalSec = 1;
    // Root CBVs are 256-byte aligned; keep the pad a multiple of that, 256 B .. 4 MB.
    g_cfg.cbvPad = (g_cfg.cbvPad + 255) & ~255;
    if (g_cfg.cbvPad < 256) g_cfg.cbvPad = 256;
    if (g_cfg.cbvPad > (4 << 20)) g_cfg.cbvPad = 4 << 20;
}

// [General] LoadPix=1 without PixDll: the newest installed PIX
// (%ProgramFiles%\Microsoft PIX\<version>\WinPixGpuCapturer.dll, highest version folder name).
static bool FindPixCapturer(wchar_t* out, size_t outCount)
{
    wchar_t root[MAX_PATH], pattern[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"ProgramFiles", root, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    swprintf_s(pattern, L"%s\\Microsoft PIX\\*", root);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    wchar_t best[MAX_PATH] = L"";
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
        if (best[0] && wcscmp(fd.cFileName, best) <= 0) continue;
        wchar_t cand[MAX_PATH];
        swprintf_s(cand, L"%s\\Microsoft PIX\\%s\\WinPixGpuCapturer.dll", root, fd.cFileName);
        if (GetFileAttributesW(cand) != INVALID_FILE_ATTRIBUTES) wcscpy_s(best, fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (!best[0]) return false;
    swprintf_s(out, outCount, L"%s\\Microsoft PIX\\%s\\WinPixGpuCapturer.dll", root, best);
    return true;
}

static void LoadPixCapturer()
{
    if (GetModuleHandleW(L"d3d12.dll"))
        Log("WARNING: d3d12.dll is already loaded, PIX GPU capture may not work");
    wchar_t path[MAX_PATH];
    if (g_cfg.pixDll[0]) wcscpy_s(path, g_cfg.pixDll);
    else if (!FindPixCapturer(path, MAX_PATH)) { Log("LoadPix: no PIX installation found (set [General] PixDll)"); return; }
    if (LoadLibraryW(path)) Log("LoadPix: WinPixGpuCapturer.dll loaded OK");
    else                    Log("LoadPix: FAILED to load WinPixGpuCapturer.dll, error %lu", GetLastError());
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID)
{
    // No logging on detach: at process exit other threads are already gone and
    // could have died holding the log lock.
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(hinst);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // Unity's crash handler lives in the same folder; leave it (and our log) alone.
    if (EndsWithI(exe, L"UnityCrashHandler64.exe") || EndsWithI(exe, L"UnityCrashHandler32.exe"))
        return TRUE;

    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(hinst, dir, MAX_PATH);
    wchar_t* slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;

    wchar_t logPath[MAX_PATH], prevPath[MAX_PATH], iniPath[MAX_PATH];
    swprintf_s(logPath, L"%segpu_proxy.log", dir);
    swprintf_s(prevPath, L"%segpu_proxy.prev.log", dir);
    swprintf_s(iniPath, L"%segpu_fix.ini", dir);

    InitializeCriticalSection(&g_logCs);
    MoveFileExW(logPath, prevPath, MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen(logPath, L"w", _SH_DENYNO);

    // Only the exe name: users attach this log to public bug reports.
    const wchar_t* exeName = wcsrchr(exe, L'\\');
    exeName = exeName ? exeName + 1 : exe;
    OSVERSIONINFOW os = { sizeof(os) };
    if (auto rtlGetVersion = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))
        rtlGetVersion(&os);
    Log("egpu_fix v%s loaded into %ls (pid %lu), Windows %lu.%lu.%lu",
        EGPU_FIX_VERSION_STR, exeName, GetCurrentProcessId(), os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);

    ReadConfig(iniPath);
    Log("config: Hooks=%d Measure=%d IntervalSec=%d VidmemHeaps=%d VidmemBuffers=%d VidmemBufMaxKB=%d VidmemReserveMB=%d"
        " VidmemCapMB=%d VidmemWithReBAR=%d | GpuTiming=%d KmtStats=%d LogCreateLimit=%d TopN=%d ConstPages=%d CbvPad=%d"
        " LoadPix=%d (ini %s)",
        g_cfg.hooks, g_cfg.measure, g_cfg.statsIntervalSec, g_cfg.vidmemHeaps, g_cfg.vidmemBuffers, g_cfg.vidmemBufMaxKB,
        g_cfg.vidmemReserveMB, g_cfg.vidmemCapMB, g_cfg.vidmemWithReBAR, g_cfg.gpuTiming, g_cfg.kmtStats,
        g_cfg.logCreateLimit, g_cfg.topN, g_cfg.constPages, g_cfg.cbvPad, g_cfg.loadPix,
        GetFileAttributesW(iniPath) != INVALID_FILE_ATTRIBUTES ? "found" : "missing, defaults");

    if (g_cfg.loadPix)
        LoadPixCapturer();

    if (g_cfg.hooks)
        InstallInitialHooks();

    return TRUE;
}
