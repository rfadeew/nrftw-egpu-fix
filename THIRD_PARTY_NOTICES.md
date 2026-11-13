# Third-party notices

## MinHook 1.3.4

`version.dll` contains MinHook, compiled from `third_party/minhook`.
Source: https://github.com/TsudaKageyu/minhook

```
MinHook - The Minimalistic API Hooking Library for x64/x86
Copyright (C) 2009-2017 Tsuda Kageyu.
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

 1. Redistributions of source code must retain the above copyright
    notice, this list of conditions and the following disclaimer.
 2. Redistributions in binary form must reproduce the above copyright
    notice, this list of conditions and the following disclaimer in the
    documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
"AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER
OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

MinHook includes the Hacker Disassembler Engine 64 C,
Copyright (c) 2008-2009, Vyacheslav Patkov, under the same 2-clause BSD terms.
The full text is in `third_party/minhook/LICENSE.txt`.

## NVIDIA NVAPI

No NVAPI files are included in this project or in the release package.
The DLL loads the `nvapi64.dll` that the NVIDIA display driver installs in
`System32`, and calls it through `nvapi_QueryInterface`.

The function IDs (`NvAPI_Initialize`, `NvAPI_D3D12_CreateHeap2`,
`NvAPI_D3D12_CreateCommittedResource`, `NvAPI_D3D12_QueryCpuVisibleVidmem`),
the flag values and the parameter struct layouts in `src/hooks.cpp` were taken
from the public NVAPI SDK, which NVIDIA publishes under the MIT license at
https://github.com/NVIDIA/nvapi.

NVIDIA and NVAPI are trademarks of NVIDIA Corporation. This project is not
affiliated with or endorsed by NVIDIA or by Moon Studios.
