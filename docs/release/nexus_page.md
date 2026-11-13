# Nexus Mods page: fields to fill in

The game section is https://www.nexusmods.com/norestforthewicked.
GitHub links are filled in. Use the GitHub Releases link until the Nexus page exists.

## Name

No Rest for the Wicked eGPU Fix

## Version

0.2.0

## Category

Performance? Use "Miscellaneous" if the game has no Performance category.

## Brief overview (short summary, max ~350 characters)

Up to 3x FPS on NVIDIA eGPUs without Resizable BAR (Thunderbolt/USB4). A version.dll proxy that moves the game's
per-frame upload heaps into VRAM and copies only the changed data over the link. Tested on one system so far.
Switches itself off on ReBAR, AMD and Intel GPUs.

## File to upload

`No-Rest-for-the-Wicked-eGPU-Fix-v0.2.0.zip` (from `dist\`). Main file, version 0.2.0.
File description: "Copy version.dll and egpu_fix.ini into the game folder."

## Tags and checkboxes to review

- **Generative AI tag.** Nexus requires one when AI was used: "AI-Generated Content" if AI produced most of the
  code, "AI Assisted" if the work was developer-led with limited AI involvement. Pick the one that is true.
  Undisclosed AI use can lead to moderation.
- Permissions: see the "Permissions and credits" section below.
- Requirements: no other mods are needed. Do not list MelonLoader.

## Upload form fields (plain text)

Paste each block into the field with the same name. If a field accepts formatting, the BBCode description further
below can be used instead.

### Description

```
Makes No Rest for the Wicked much faster on NVIDIA eGPUs without Resizable BAR (Thunderbolt / USB4 enclosures).

Over Thunderbolt without ReBAR the GPU waits for most of every frame: the game makes it read per-frame vertex and shader data directly from system memory, across the slow link. This mod is a small version.dll proxy. It puts that data in VRAM and keeps a copy in system memory for the game to write to. Only the changed pages are copied to VRAM before the GPU uses them, so the GPU reads everything locally. Game files are not modified.

Results on the test system (i7-12700H laptop, Thunderbolt 4, RTX 5070 Ti eGPU without ReBAR, driver 617.42, Windows 11 25H2), same session, same spots:
- Heaviest spot: about 11-13 FPS -> about 40-46 FPS (version 0.1.0 gave 31-34 FPS, and 16-26 FPS once its VRAM window filled up in a long session)
Your results will differ.

Tested on one system so far, including a 1-hour play session. Known limitations:
- System RAM use rises by about 300-350 MB.
- At the heaviest spot the frame rate is limited by the copy bandwidth over Thunderbolt.
- Known technical risk: the copy target overlaps the game's own buffers in VRAM (written on another GPU queue). No problems seen so far.
- Heaps the main fix cannot take use the version 0.1.0 method (CPU-visible VRAM window), then normal memory. ShadowHeaps=0 in egpu_fix.ini gives the 0.1.0 behaviour.
- Online co-op and sessions longer than an hour are not tested yet. The game has no anti-cheat (Steam build 22928553), and the mod does not touch game logic, saves or network traffic. Still, use it online at your own risk.
- If the game crashes, remove the mod and retry before reporting the crash to Moon Studios.

To report a problem, attach egpu_proxy.log from the game folder (it contains no user names or paths) and list your GPU, enclosure, link and CPU. For performance problems, set Measure=1 in egpu_fix.ini first.

Source code (MIT) and technical details: https://github.com/rfadeew/nrftw-egpu-fix
```

### Installation instructions

```
1. Open the game folder: in Steam, right-click the game -> Manage -> Browse local files (the folder with NoRestForTheWicked.exe).
2. Copy version.dll and egpu_fix.ini from the archive into that folder.
3. Start the game normally. No launcher or loader is needed.
4. Optional check: egpu_proxy.log appears in the game folder and contains "shadow: stage 2e ON" and "shadow: UPLOAD heap" lines.

Using MelonLoader? It also installs a version.dll. Do not overwrite it: rename MelonLoader's version.dll to winhttp.dll first (MelonLoader supports that name), then copy this mod's files. This combination is not tested yet.

Uninstall: delete version.dll and egpu_fix.ini (and egpu_proxy.log, egpu_proxy.prev.log) from the game folder.
Update: overwrite both files with the new version.
```

### Main features

```
- Moves the game's per-frame upload heaps into VRAM, with a write-watched system-memory copy and a per-frame copy of only the changed pages. On the test system: heaviest spot about 12 -> 40-46 FPS.
- Turns itself off on GPUs with Resizable BAR, on AMD and Intel GPUs, and when NVAPI is missing. The DLL then only passes calls through.
- Falls back to the older CPU-visible VRAM window method (v0.1.0), then to the game's normal heaps, for heaps it cannot take.
- Does not change game files, shaders, saves or network traffic. The rendered image is the same.
- Writes egpu_proxy.log with FPS and fix status. Measure=1 adds detailed GPU timing for bug reports. The log contains no user names or folder paths.
- Five settings in a commented egpu_fix.ini. To uninstall, delete two files.
- Open source (MIT).
```

### Requirements

```
- NVIDIA GPU without Resizable BAR, typically an eGPU over Thunderbolt 3/4/5 or USB4. On other GPUs the mod does nothing.
- Windows 10/11 64-bit with a current NVIDIA driver (tested: Windows 11 25H2, driver 617.42).
- Steam version of the game (tested: build 22928553).
- No other mods needed. MelonLoader is NOT required. If you use it, rename its version.dll to winhttp.dll (see Installation).
```

### Describe how AI was used

Check this text and correct anything that does not match what you did yourself.

```
This mod was developed with Claude Code (Anthropic's AI coding assistant).

The AI wrote most of the C++ code: the version.dll proxy, the D3D12 hooks, the NVAPI integration, the logging and statistics, and the standalone D3D12 test program. It also helped analyse the logs and captures, and drafted the README and this page.

My part: I found and reproduced the problem on my own eGPU setup. I ran the diagnostics (nvidia-smi, PIX timing and GPU captures) and tested every build in the game. I decided what to try next and checked the results. Every performance number on this page comes from real runs on my hardware, measured by the mod's own logging, and I keep those logs.

Before a build goes into the game, an automated test renders with it and checks the results by reading them back, including a negative test that must fail. The source code is public (MIT), so anyone can review it: https://github.com/rfadeew/nrftw-egpu-fix
```

## Description (BBCode)

```bbcode
[size=5][b]No Rest for the Wicked eGPU Fix[/b][/size]
Performance fix for [b]NVIDIA eGPUs without Resizable BAR[/b] (Thunderbolt / USB4 enclosures). It has been tested on one system so far. Please read the known issues and report your results.

[size=4][b]What it does[/b][/size]
No Rest for the Wicked reads its per-frame vertex, shader-resource and constant data from D3D12 "upload" heaps in system memory. Over Thunderbolt without ReBAR every one of those GPU reads crosses a slow link, and the GPU spends most of the frame waiting. On the test system an RTX 5070 Ti eGPU was slower than the laptop's internal RTX 3060.

This mod is a small [b]version.dll[/b] proxy. It creates those heaps in VRAM and keeps a copy in system memory (a "shadow") for the game's CPU writes. Before each frame's GPU work, only the pages the CPU changed are copied from the shadow to VRAM on the mod's own copy queue. The GPU then reads that data from its own VRAM. Game files, shaders and rendering are unchanged. Version 0.1.0 used the 256 MB CPU-visible VRAM window (BAR1) through NVAPI instead; that method is still the fallback.

[size=4][b]Results (test system)[/b][/size]
[list]
[*]Laptop with an i7-12700H and a Thunderbolt 4 port. eGPU: RTX 5070 Ti 16 GB, no ReBAR.
[*]NVIDIA driver 617.42, Windows 11 25H2, game Steam build 22928553.
[/list]
[list]
[*]Heaviest spot: [b]about 11-13 FPS -> about 40-46 FPS[/b]. Version 0.1.0 gave 31-34 FPS there, and 16-26 FPS once its VRAM window filled up in a long session.
[*]Per frame at that spot: about 12 ms of GPU work plus about 7.6 ms of the GPU waiting for the copy over Thunderbolt.
[/list]
Measured in the same session at the same spot, with the mod's own logging. Your results will differ. Full measurements and technical details: [url=https://github.com/rfadeew/nrftw-egpu-fix/blob/main/docs/how-it-works.md]How it works[/url].

[size=4][b]Who should use it[/b][/size]
[list]
[*][b]NVIDIA GPU in an eGPU enclosure, no ReBAR:[/b] the case it was made for.
[*][b]NVIDIA GPU, no ReBAR, other link (OCuLink, internal):[/b] it switches on, but this is untested.
[*][b]NVIDIA GPU with ReBAR:[/b] it switches itself off. Nothing changes.
[*][b]AMD or Intel GPU:[/b] it switches itself off. Nothing changes.
[/list]
To check ReBAR: NVIDIA Control Panel -> System Information -> Resizable BAR.

[size=4][b]Installation[/b][/size]
[list=1]
[*]Steam: right-click the game -> Manage -> Browse local files. This is the folder with NoRestForTheWicked.exe.
[*]Copy [b]version.dll[/b] and [b]egpu_fix.ini[/b] from the archive into that folder.
[*]Start the game normally.
[/list]
To check that it works, open [b]egpu_proxy.log[/b] in the game folder. It should contain "shadow: stage 2e ON" and "shadow: UPLOAD heap" lines.

[b]Using MelonLoader?[/b] It also installs a version.dll. Do not overwrite it: rename MelonLoader's version.dll to winhttp.dll first (MelonLoader supports that name), then copy this mod's files. This combination is not tested yet.

[size=4][b]Uninstallation[/b][/size]
Delete [b]version.dll[/b] and [b]egpu_fix.ini[/b] (and egpu_proxy*.log) from the game folder.

[size=4][b]Settings[/b][/size]
egpu_fix.ini has five settings, all with comments:
[list]
[*]ShadowHeaps: 1 = the main fix (default), 0 = off. ShadowHeaps=0 gives the version 0.1.0 behaviour.
[*]VidmemHeaps: 1 = fallback (the version 0.1.0 method, default), 0 = off. Both 0: the DLL only logs FPS.
[*]VidmemCapMB: upper limit for the fallback's use of the VRAM window (default 192). The fallback also stops by itself when the window is full.
[*]Measure: 1 = detailed logging for bug reports. Costs FPS; set it back to 0 afterwards.
[*]IntervalSec: seconds between the stats lines in the log.
[/list]

[size=4][b]Known issues and limitations[/b][/size]
[list]
[*][b]More system RAM:[/b] about 300-350 MB (copies of the game's upload heaps). They are not freed while the game runs.
[*][b]Copy bandwidth:[/b] changed data (about 21 MB per frame at the heaviest spot) crosses Thunderbolt every frame, and that limits the frame rate there.
[*][b]Known technical risk:[/b] the copy target is one buffer placed over a whole VRAM heap, so it overlaps the game's own buffers. It is written on the mod's copy queue while the game may read other parts of the heap on another queue. No problems seen so far.
[*][b]Fallback only (the 0.1.0 method):[/b] CPU writes into the VRAM window are slow, CPU reads are extremely slow, and only 160-192 MB of the 256 MB window is usable. When it is full, the mod writes a "SYSMEM" line to the log and uses normal heaps, exactly as without the mod. That is normal.
[*][b]Sessions longer than about an hour are not tested yet.[/b] The game's heap pool only grows.
[*][b]Online co-op has not been tested with the mod.[/b] The game ships no anti-cheat (as of Steam build 22928553), and the mod does not touch game logic, saves or network traffic. Still, use it online at your own risk. If the game adds anti-cheat later, remove the mod.
[*]If the game crashes, remove the mod and retry before reporting the crash to Moon Studios.
[*]Game updates may change how the game uses GPU memory. The mod could then stop helping, or stop being needed.
[/list]

[size=4][b]Reporting problems[/b][/size]
Post in the Bugs tab or on [url=https://github.com/rfadeew/nrftw-egpu-fix/issues]GitHub[/url], and include:
[list]
[*][b]egpu_proxy.log[/b] from the game folder (previous run: egpu_proxy.prev.log). It contains no user names or folder paths.
[*]Your GPU, enclosure, link (TB3/TB4/TB5/USB4/OCuLink), CPU, driver version and graphics settings.
[*]For performance problems, set Measure=1 in egpu_fix.ini, play 1-2 minutes at the problem spot, then attach that log.
[/list]

[size=4][b]Source code[/b][/size]
Open source (MIT): [url=https://github.com/rfadeew/nrftw-egpu-fix]https://github.com/rfadeew/nrftw-egpu-fix[/url]

[size=4][b]Credits[/b][/size]
[list]
[*][url=https://github.com/TsudaKageyu/minhook]MinHook[/url] by Tsuda Kageyu (BSD 2-clause).
[*]NVAPI function IDs and structures from NVIDIA's public [url=https://github.com/NVIDIA/nvapi]NVAPI SDK[/url] (MIT). No NVIDIA files are included.
[/list]
Not affiliated with Moon Studios, Private Division or NVIDIA.
```

## Permissions and credits (Nexus form)

- Upload permission: You are allowed to upload this file to other sites but you must credit me as the creator
  of the file.
- Modification permission: You are allowed to modify my files and release bug fixes or improve on the features
  so long as you credit me as the original creator. (The MIT license allows this anyway.)
- Conversion permission: not applicable (Windows/PC only).
- Asset use permission: You are allowed to use the assets in this file without permission as long as you credit
  me.
- Asset use permission in mods/files that are being sold: allowed. The MIT license already permits commercial use,
  so a stricter Nexus setting would contradict the license.
- Credits: "MinHook by Tsuda Kageyu (BSD 2-clause). NVAPI IDs/structures from NVIDIA's NVAPI SDK (MIT)."
- Donation points: your choice.
