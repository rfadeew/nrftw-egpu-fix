# Steam community forum post (No Rest for the Wicked → General Discussions)

Steam markup. Replace `NEXUS_URL` before you post (or use the GitHub Releases link).

## Title

[Mod] eGPU without ReBAR: about 12 → 40-46 FPS with a small fix (NVIDIA, Thunderbolt) - v0.2.0

## Body

```
If you play on an NVIDIA eGPU over Thunderbolt / USB4, you may have noticed that the game runs much worse than the GPU should allow. Sometimes it is even slower than a weaker internal GPU. I found the cause and made a small fix. It has been tested on one system so far, and I am looking for more eGPU testers.

[h2]The cause[/h2]
The game reads a lot of per-frame data (vertices, shader data, constants) directly from system memory. In a desktop PC that is fast. Over Thunderbolt without Resizable BAR every one of those GPU reads crosses a slow, high-latency link, and the GPU spends most of the frame waiting.

[h2]The fix[/h2]
A version.dll proxy that you drop into the game folder. It puts those buffers in VRAM and keeps a copy in system memory for the game to write to. Only the changed data is copied to VRAM before the GPU needs it, so the GPU reads everything from its own VRAM. Version 0.2.0 no longer depends on the small CPU-visible VRAM window that version 0.1.0 used, so it keeps working in long sessions. Game files are not modified. To uninstall, delete the two files.

[h2]Results on my system[/h2]
i7-12700H laptop with a TB4 port, RTX 5070 Ti eGPU (no ReBAR), driver 617.42, Windows 11 25H2:
[list]
[*]Heaviest spot: [b]about 11-13 → 40-46 FPS[/b] (v0.1.0: 31-34 FPS, and 16-26 FPS once its VRAM window filled up in a long session)
[/list]

[h2]Who it is for[/h2]
[list]
[*]NVIDIA GPU without ReBAR, especially in an eGPU enclosure.
[*]With ReBAR, or on AMD / Intel GPUs, it switches itself off and changes nothing.
[/list]

[h2]Install / uninstall[/h2]
[list]
[*]Install: copy version.dll and egpu_fix.ini into the game folder (Steam → right-click the game → Manage → Browse local files).
[*]Uninstall: delete version.dll and egpu_fix.ini.
[*]MelonLoader users: MelonLoader also uses version.dll. Rename its version.dll to winhttp.dll first (untested combination).
[/list]

[h2]Known limitations[/h2]
[list]
[*]It uses about 300-350 MB more system RAM.
[*]At the heaviest spot the frame rate is limited by how fast changed data can be copied over Thunderbolt.
[*]There is a known technical risk (the copy target overlaps the game's own buffers in VRAM). No problems seen so far.
[*]If something looks wrong, set ShadowHeaps=0 in egpu_fix.ini to get the older v0.1.0 behaviour, and tell me whether it changes anything.
[*]Tested in one 1-hour session; longer sessions and online co-op are not tested yet. The game has no anti-cheat (as of the current Steam build), and the mod does not touch game logic or network traffic. Still, use it online at your own risk.
[*]If the game crashes, please remove the mod and retry before reporting the crash to Moon Studios.
[/list]

[h2]Problems or results?[/h2]
Please reply here or open an issue on GitHub. Attach egpu_proxy.log from the game folder (it contains no user names or paths) and say which GPU, enclosure, link and CPU you have. For performance issues, set Measure=1 in egpu_fix.ini first.

Download (Nexus): NEXUS_URL
Source code and details (GitHub, MIT): https://github.com/rfadeew/nrftw-egpu-fix

Not affiliated with Moon Studios. Moon, if you read this: a short technical write-up of the cause is on the GitHub page.
```
