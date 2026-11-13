# Runs build\smoke.exe over the configuration matrix (Measure 0/1 x fix modes) plus the negative tests.
# Each run gets build\egpu_fix.ini = tests\egpu_fix.dev.ini with some keys overridden; its log is kept
# as build\matrix\<name>.log. Exit code 0 = every run ended as expected (PASS, or FAIL for negative tests).
# Usage: powershell -ExecutionPolicy Bypass -File tests\smoke_matrix.ps1 [-Frames "30 30 30 40"] [-Only name,...]
param([string]$Frames = "30 30 30 40", [string[]]$Only = @())

$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "build"
$outDir = Join-Path $build "matrix"
New-Item -ItemType Directory -Force $outDir | Out-Null
$base = Get-Content (Join-Path $PSScriptRoot "egpu_fix.dev.ini")

# name, expected (pass/fail), extra smoke args, ini overrides
$runs = @(
    @{ n = "m0_release";      e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 1; HybridHeaps = 0 } },
    @{ n = "m0_nofix";        e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 0 } },
    @{ n = "m0_hyb_vbib";     e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 0 } },
    @{ n = "m0_hyb_srv";      e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1 } },
    @{ n = "m0_vid_hyb";      e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 1; HybridHeaps = 1; HybridSrv = 1 } },
    @{ n = "m0_hyb_srv_cq";   e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridCopyQueue = 1 } },
    @{ n = "m0_hyb_srv_gap0"; e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridMergeKB = 0 } },
    @{ n = "m1_release";      e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 1; HybridHeaps = 0 } },
    @{ n = "m1_hyb_vbib";     e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 0 } },
    @{ n = "m1_hyb_srv";      e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1 } },
    @{ n = "m1_hyb_check";    e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridCheck = 1 } },
    @{ n = "m1_hyb_check_cq"; e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridCheck = 1; HybridCopyQueue = 1 } },
    @{ n = "m1_consts";       e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; ConstPages = 1 } },
    @{ n = "m0_shadow";       e = "pass"; a = "";      o = @{ Measure = 0; VidmemHeaps = 1; HybridHeaps = 1; HybridSrv = 1; ShadowHeaps = 1 } },
    @{ n = "m1_shadow";       e = "pass"; a = "";      o = @{ Measure = 1; VidmemHeaps = 1; HybridHeaps = 1; HybridSrv = 1; ShadowHeaps = 1 } },
    @{ n = "m0_shadow_gap0";  e = "pass"; a = "";      o = @{ Measure = 0; ShadowHeaps = 1; ShadowMergeKB = 0 } },
    @{ n = "m0_shadow_stream"; e = "pass"; a = "";     o = @{ Measure = 0; ShadowHeaps = 1; ShadowStreamUs = 500 } },
    @{ n = "m1_shadow_stream"; e = "pass"; a = "";     o = @{ Measure = 1; ShadowHeaps = 1; ShadowStreamUs = 500 } },
    @{ n = "neg_break";       e = "fail"; a = "break"; o = @{ Measure = 0; VidmemHeaps = 1; HybridHeaps = 0 } },
    @{ n = "neg_hyb_nocopy";  e = "fail"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridTestNoCopy = 1 } },
    @{ n = "neg_srv_1copy";   e = "fail"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridSrvCopies = 1 } },
    @{ n = "neg_shadow_break"; e = "fail"; a = "break"; o = @{ Measure = 0; ShadowHeaps = 1 } },
    @{ n = "neg_shadow_nocopy"; e = "fail"; a = "";     o = @{ Measure = 0; ShadowHeaps = 1; ShadowTestNoCopy = 1 } },
    @{ n = "neg_shadow_stream_nocopy"; e = "fail"; a = ""; o = @{ Measure = 0; ShadowHeaps = 1; ShadowStreamUs = 500; ShadowTestNoCopy = 1 } },
    @{ n = "neg_cq_1copy";    e = "fail"; a = "";      o = @{ Measure = 0; VidmemHeaps = 0; HybridHeaps = 1; HybridSrv = 1; HybridSrvCopies = 1; HybridCopyQueue = 1 } }
)

$Only = @($Only | ForEach-Object { $_ -split "," } | Where-Object { $_ })
$bad = 0
foreach ($r in $runs) {
    if ($Only.Count -and -not ($Only -contains $r.n)) { continue }
    $ini = $base | ForEach-Object {
        $line = $_
        foreach ($k in $r.o.Keys) { if ($line -match "^$k=") { $line = "$k=$($r.o[$k])" } }
        $line
    }
    Set-Content -Path (Join-Path $build "egpu_fix.ini") -Value $ini -Encoding ascii
    $smokeArgs = "$Frames $($r.a)".Trim().Split(" ")
    $out = & (Join-Path $build "smoke.exe") @smokeArgs 2>&1
    $code = $LASTEXITCODE
    Copy-Item (Join-Path $build "egpu_proxy.log") (Join-Path $outDir "$($r.n).log") -Force
    $out | Set-Content (Join-Path $outDir "$($r.n).out.txt")
    $got = if ($code -eq 0) { "pass" } else { "fail" }
    $ok = $got -eq $r.e
    if (-not $ok) { $bad++ }
    $summary = ($out | Where-Object { $_ -match "^phase [1-4]:|SMOKE|DEVICE REMOVED" }) -join " | "
    "{0,-16} expected {1}, got {2} (exit {3}) {4}`n    {5}" -f $r.n, $r.e, $got, $code, ($(if ($ok) { "OK" } else { "<<< UNEXPECTED" })), $summary
}
# Leave the plain dev ini in place.
Copy-Item (Join-Path $PSScriptRoot "egpu_fix.dev.ini") (Join-Path $build "egpu_fix.ini") -Force
if ($bad) { "MATRIX: $bad unexpected result(s)"; exit 1 } else { "MATRIX: all as expected"; exit 0 }
