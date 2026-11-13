No Rest for the Wicked eGPU Fix - v0.1.0-beta (PUBLIC BETA)
=============================================================

Speeds up No Rest for the Wicked on NVIDIA eGPUs without Resizable BAR
(Thunderbolt / USB4 enclosures). Game files are not modified.
On GPUs with ReBAR, and on AMD / Intel GPUs, it switches itself off.

INSTALL
  1. Steam: right-click the game -> Manage -> Browse local files.
     (The folder that contains NoRestForTheWicked.exe.)
  2. Copy version.dll and egpu_fix.ini into that folder.
  3. Start the game normally.
  Check: egpu_proxy.log in the game folder should contain
  "vidmem: stage 2c ON" and "-> VRAM" lines.
  MelonLoader users: MelonLoader also uses version.dll. Do not overwrite
  it; rename MelonLoader's version.dll to winhttp.dll first (untested).

UNINSTALL
  Delete version.dll and egpu_fix.ini (and egpu_proxy*.log) from the game folder.

SETTINGS
  See the comments in egpu_fix.ini. VidmemHeaps=0 turns the fix off.

REPORTING PROBLEMS
  Attach egpu_proxy.log (the previous run is egpu_proxy.prev.log) and your GPU,
  enclosure, link, CPU and driver version. For performance problems, set
  Measure=1 in egpu_fix.ini, play 1-2 minutes at the problem spot and attach
  that log. The log contains no user names or folder paths.

KNOWN LIMITATIONS
  - Beta, tested on one system (i7-12700H laptop, TB4, RTX 5070 Ti eGPU,
    driver 617.42, Windows 11 25H2).
  - CPU writes to the moved buffers are slower; heavy scenes can become
    CPU-limited instead of GPU-limited.
  - Only 160-192 MB of the 256 MB VRAM window is usable. When it is full
    (a "SYSMEM" line in the log), further heaps stay in normal system memory,
    as without the fix: correct, just not faster. No action needed.
  - Tested in one 1-hour session; longer sessions and online co-op are not
    tested yet. Use online at your own risk; the game has no anti-cheat as of
    Steam build 22928553.
  - If the game crashes, remove the fix and retry before reporting the crash
    to Moon Studios.

Full README, source code and issue tracker:
  https://github.com/rfadeew/nrftw-egpu-fix

MIT License (see LICENSE). Uses MinHook (BSD 2-clause), see THIRD_PARTY_NOTICES.md.
Not affiliated with Moon Studios, Private Division or NVIDIA.
