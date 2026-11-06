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
| lupa | 2.6 | MIT; bundled Lua uses its upstream MIT license | https://github.com/scoder/lupa |
| PyYAML | 6.0.3 | MIT | https://github.com/yaml/pyyaml |

Unicorn is used for original-code regression checks on the build computer; it is
not linked into the Android app. The Android SDK, NDK and JDK are separately
installed toolchains with their own licenses.

## Unofficial Crusader Patch — GPL-3.0

Source: https://github.com/UnofficialCrusaderPatch/UnofficialCrusaderPatch

Pinned umbrella revision: `db6755fbfa55a4abeb2d1231487d1e9f3e434413`.
UCP3 framework: `fd326a41d8927eaee42dfb542658843138c15867`.
UCP2 Legacy module: `04e826d00f1a5f8a623e15acffc743f115ee7fb1`.

Copyright remains with the Unofficial Crusader Patch contributors. The upstream
LICENSE files and authorship remain in the pinned checkout. The Android build
translates supported Legacy patch instructions to guarded ARM64 functions and
checked guest-data writes. Android-specific adjustments are in `tools/ucp.py`,
`config/ucp-android.json` and `android/host/mods.c`; upstream files are unmodified.
The complete GPL version 3 text is in the repository LICENSE and APK assets.
Lupa and Lua execute the original patch scripts only on the build computer.

## GOG installer reader and compression libraries

The APK includes an ARM64 executable built from these pinned sources. The
Android CMake build and metadata entry point are separate adaptations in
`android/gog`; upstream files are unmodified. This software reads installers as
data and does not run their Windows programs.

| Component | Revision / version | License | Source |
| --- | --- | --- | --- |
| innoextract | `81fd9b95b76ee5115ce116bfdbc3bc6b887b809d` | Zlib | https://github.com/dscharrer/innoextract |
| Boost | 1.84.0 | Boost Software License 1.0 | https://www.boost.org/ |
| bzip2 / libbzip2 | `6a8690fc8d26c815e798c588f796eabe9d684cf0` | bzip2 license | https://gitlab.com/bzip2/bzip2 |
| liblzma from XZ Utils | `061a1e4d2d0b1c3f5c9a637fd94bac035a826340` | Public domain | https://github.com/tukaani-project/xz |

innoextract is Copyright (C) 2011–2020 Daniel Scharrer. bzip2 is Copyright
(C) 1996–2019 Julian R Seward. Boost retains the copyright notices in its source
files. The selected XZ revision places liblzma in the public domain; its GPL
shell scripts and LGPL getopt are not linked into this executable. Android's
system zlib supplies deflate support. Full upstream notices are retained in
`android/licenses` and packaged under `assets/licenses` in the APK.

The APK's small native payload decoder also uses the same pinned public-domain
liblzma. Game libraries are stored as ARM64-filtered XZ chunks and unpacked into
private app storage before loading; the XZ license above covers this decoder.

## Original games and middleware

Stronghold, Stronghold Crusader, Stronghold Crusader Extreme, their PC binaries,
Miles Sound System, Bink and original game resources remain the property of
their respective rights holders. They are not supplied by this repository and
are not relicensed by its GPL grant. C generated from those binaries and native
libraries containing that code are build outputs, not GPL-owned source assets.
