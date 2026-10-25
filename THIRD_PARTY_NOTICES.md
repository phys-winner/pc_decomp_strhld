# Third-party notices

The port's own code and documentation are GPL-3.0-or-later as stated in
readme.md. The following components and derived portions keep their original
licenses and attribution. These notices do not license any original game code
or game data.

## pcrecomp — MIT

Source: https://github.com/sp00nznet/pcrecomp

Pinned revision: `904ac3129d0c9d05efbeec19a1dce6abbe7f5d26`.

The build obtains this tools-only dependency in the ignored directory
`reference/pcrecomp`. Its license also applies to portions of the lifting and
runtime support derived from pcrecomp. Project-specific adaptations and additions
are supplied under the repository's GPL-3.0-or-later terms; preserve this notice
for the upstream portions. The dependency's full LICENSE remains in its checkout.

```text
MIT License

Copyright (c) 2026 sp00nz

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## dlmalloc 2.8.6 — MIT-0

Source: https://gee.cs.oswego.edu/pub/misc/malloc.c

Vendored file: `reference/dlmalloc/malloc.c`.

Copyright 2023 Doug Lea. The complete MIT-0 grant is retained at the beginning of
the source file. This version was relicensed from CC0 to MIT-0 on 25 September
2023. The allocator is included directly by `android/host/memory.c`.

## AMD FidelityFX Super Resolution 1 — MIT

Source: https://github.com/GPUOpen-Effects/FidelityFX-FSR

Upstream shader version: `v1.20210629`.

Adapted file: `android/host/fsr1.h`.

Copyright (c) 2021 Advanced Micro Devices, Inc. All rights reserved.
The complete original MIT notice and an explanation of the GLSL ES adaptations
are retained at the beginning of that file.

## Python development dependencies

Installed separately from `requirements.txt`; their source and wheels are not
vendored by this repository:

| Package | Version | Upstream license | Source |
| --- | --- | --- | --- |
| capstone | 5.0.7 | BSD-3-Clause | https://github.com/capstone-engine/capstone |
| pefile | 2024.8.26 | MIT | https://github.com/erocarrera/pefile |
| unicorn | 2.1.4 | GPL-2.0-only core; BSD Python bindings; upstream component notices apply | https://github.com/unicorn-engine/unicorn |

Unicorn is used for original-code regression checks on the build computer; it is
not linked into the Android app. The Android SDK, NDK and JDK are separately
installed toolchains with their own licenses.

## Original games and middleware

Stronghold, Stronghold Crusader, Stronghold Crusader Extreme, their PC binaries,
Miles Sound System, Bink and original game resources remain the property of
their respective rights holders. They are not supplied by this repository and
are not relicensed by its GPL grant. C generated from those binaries and native
libraries containing that code are build outputs, not GPL-owned source assets.
