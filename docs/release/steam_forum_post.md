# Steam community forum post (No Rest for the Wicked → General Discussions)

Steam markup. Replace `GITHUB_URL` and `NEXUS_URL` before you post.

## Title

[Mod, beta] eGPU without ReBAR: 13.7 → 42 FPS with a small fix (NVIDIA, Thunderbolt)

## Body

```
If you play on an NVIDIA eGPU over Thunderbolt / USB4, you may have noticed that the game runs much worse than the GPU should allow. Sometimes it is even slower than a weaker internal GPU. I found the cause and made a small fix. It is a [b]public beta[/b], tested on one system so far, and I am looking for more eGPU testers.

[h2]The cause[/h2]
The game reads a lot of per-frame data (vertices, shader data, constants) directly from system memory. In a desktop PC that is fast. Over Thunderbolt without Resizable BAR every one of those GPU reads crosses a slow, high-latency link, and the GPU spends most of the frame waiting.

[h2]The fix[/h2]
A version.dll proxy that you drop into the game folder. It asks the NVIDIA driver (through NVAPI) to put those buffers into the small part of VRAM the CPU can see even without ReBAR. The GPU then reads them from its own VRAM. Game files are not modified. To uninstall, delete the two files.

[h2]Results on my system[/h2]
i7-12700H laptop with a TB4 port, RTX 5070 Ti eGPU (no ReBAR), driver 617.42, Windows 11 25H2:
[list]
[*]Worst spot: [b]13.7 → 42 FPS[/b] (GPU busy per frame 73 ms → 7 ms)
[*]Lighter spot: [b]29 → 50-59 FPS[/b] (60 FPS cap)
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
[*]CPU writes into that VRAM window are slow, so heavy scenes can become CPU-limited instead of GPU-limited.
[*]Only 160-192 MB of the window is usable. When it is full, the mod notices ("SYSMEM" line in the log) and leaves further buffers in normal system memory, as without the mod. Correct, just not faster for those.
[*]Tested in one 1-hour session; longer sessions and online co-op are not tested yet. The game has no anti-cheat (as of the current Steam build), and the mod does not touch game logic or network traffic. Still, use it online at your own risk.
[*]If the game crashes, please remove the mod and retry before reporting the crash to Moon Studios.
[/list]

[h2]Problems or results?[/h2]
Please reply here or open an issue on GitHub. Attach egpu_proxy.log from the game folder (it contains no user names or paths) and say which GPU, enclosure, link and CPU you have. For performance issues, set Measure=1 in egpu_fix.ini first.

Download (Nexus): NEXUS_URL
Source code and details (GitHub, MIT): GITHUB_URL

Not affiliated with Moon Studios. Moon, if you read this: a short technical write-up of the cause is on the GitHub page.
```
