# Third-party notices

The NTSCRT plug-ins' own code is MIT-licensed (see [LICENSE](LICENSE)). The core is a C++ port of
[NTSCRT](https://github.com/finnmckenty/NTSCRT) by Finn McKenty, also MIT, whose copyright line is
kept in `LICENSE`.

The plug-ins ship the third-party software below, which keeps its own licences. Two of them are
not MIT and stay under their own licences as **separate files** next to the MIT plug-in binary:

- **librashader** (MPL-2.0) is its own library file, `librashader.dll` / `librashader.dylib`,
  loaded at run time.
- The **RetroArch CRT shaders** (several under GPL) are shipped as unmodified shader source files in
  `Resources/shaders/` and compiled at run time by librashader.

Neither of them is compiled into the plug-in binary, and neither requires the plug-ins' own code to
use a different licence. Full licence texts are in [`licenses/`](licenses/). The same files
(`LICENSE`, this file and `licenses/`) ship inside every bundle:
`NTSCRT.ofx.bundle/Contents/Resources/` for DaVinci Resolve, and
`NTSCRT.app/Contents/PlugIns/NTSCRT Renderer.pluginkit/Contents/Resources/` for Final Cut Pro.

| Component | Licence | Where it ends up |
|---|---|---|
| ntsc-rs | MIT (of MIT OR ISC OR Apache-2.0) | linked into the plug-in binary |
| Rust crates used by ntsc-rs and librashader | permissive, two MPL-2.0 crates; see `licenses/rust-crates.txt` | plug-in binary / librashader library |
| librashader | MPL-2.0 (of MPL-2.0 OR GPL-3.0-only) | separate `librashader` library |
| slang-shaders (crt-royale, crt-aperture, crt-easymode, crt-hyllian, crtsim, glow) | per file: GPL-2.0-or-later, GPL, MIT, public domain | separate source files in `Resources/shaders/` |
| OpenFX Support library | BSD-3-Clause | linked into the Resolve plug-in binary |
| nlohmann/json | MIT | linked into the plug-in binary |
| stb (stb_truetype) | MIT (or public domain) | linked into the plug-in binary |
| FxPlug SDK frameworks | Apple FxPlug SDK licence | Final Cut Pro bundle only |
| doctest | MIT | tests only, not shipped |

## NTSCRT (the upstream app this is ported from)

- **What:** the processing pipeline, parameter schema, presets and stage designs, ported from Swift
  to C++ (`core/`, `presets/`).
- **Licence:** MIT, Copyright (c) 2026 Finn McKenty. The copyright line is in [LICENSE](LICENSE).
- **Source:** https://github.com/finnmckenty/NTSCRT

## ntsc-rs (the NTSC stage)

- **What:** the `ntsc-rs` core library by valadaptive (`vendor/ntsc-rs/crates/ntscrs`), compiled as
  a static library through a small C wrapper (`rust/ntscrs-capi`, also MIT OR ISC OR Apache-2.0) and
  linked into the plug-in binary.
- **Licence:** MIT OR ISC OR Apache-2.0, used here under MIT. (Only ntsc-rs's GUI app,
  `crates/gui`, is GPL-3.0; the plug-ins don't use it.)
- **Source:** https://github.com/ntsc-rs/ntsc-rs (pinned as the `vendor/ntsc-rs` submodule).

```
Copyright © valadaptive

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## librashader (the CRT shader runtime)

- **What:** `librashader.dll` (Windows) / `librashader.dylib` (macOS), built unmodified from
  SnowflakePowered/librashader at commit `76462c030b75c4f2d56e5386c3d4d7d1128318b8` (the
  `vendor/librashader` submodule) with
  `cargo build --release --locked -p librashader-capi --no-default-features --features stable,runtime-d3d11`
  on Windows and `--features stable,runtime-metal` on macOS (see `core/stages/crt/CMakeLists.txt`).
  The plug-ins also compile against its C API headers (`include/librashader.h`,
  `include/librashader_ld.h`).
- **Licence:** librashader offers MPL-2.0 OR GPL-3.0-only; the plug-ins distribute it under
  **MPL-2.0** (text in `licenses/MPL-2.0.txt`). MPL-2.0 applies file by file, so it covers
  librashader's files only, not the plug-ins' code. The C API headers are MIT, Copyright 2022
  chyyran, with the notice kept in the files.
- **Source Code Form:** https://github.com/SnowflakePowered/librashader/tree/76462c030b75c4f2d56e5386c3d4d7d1128318b8
  (also in this repository as the `vendor/librashader` submodule).

## Rust crates inside the two Rust builds

The ntsc-rs static library and librashader statically include open-source Rust crates (169 in
total over the Windows and macOS builds, including glslang and SPIRV-Cross through their `-sys`
crates). All are under permissive licences (MIT, Apache-2.0, BSD, ISC, Zlib, CC0, Unlicense,
Unicode-3.0), except librashader's own crates (above) and two small MPL-2.0 crates used by
librashader's cache (`persy`, `smartstring`), which stay under MPL-2.0 inside the librashader library.
Every crate, its licence, and its licence and copyright text is listed in
[`licenses/rust-crates.txt`](licenses/rust-crates.txt), regenerated by
`python scripts/gen_rust_licenses.py` (adapted from upstream NTSCRT's script).

## RetroArch CRT shaders (slang-shaders)

- **What:** the files listed in `resources/shader-manifest.txt`, copied unmodified from libretro's
  slang-shaders (the `vendor/slang-shaders` submodule, commit
  `cb01f2f238700eed7ec1c859be1737a9fbcdcb7c`) into `Resources/shaders/` as source, and compiled at run
  time by librashader. They are separate works, not part of the plug-ins' code.
- **Licence:** slang-shaders has no single licence for the collection; each shader carries its own,
  in its files:
  - **crt-royale** (`crt/shaders/crt-royale/`, the files it includes from `blurs/` and `include/`
    excepted): GPL-2.0-or-later, Copyright (C) 2014 TroggleMonkey. Its `LICENSE.TXT` ships with it.
  - **crt-aperture**, **crt-easymode**: "License: GPL" in the shader (by EasyMode).
  - **crt-hyllian**: MIT, Copyright (C) 2011-2024 Hyllian.
  - **crtsim**: public domain (CC0 1.0 dedication by J. Kyle Pittman).
  - **Royale blur and helper headers** (`blurs/shaders/royale/`, `include/blur-functions.h`,
    `include/gamma-management.h`, `include/quad-pixel-communication.h`,
    `include/special-functions.h`): MIT, Copyright (C) 2014 TroggleMonkey.
  - **crtglow** (`crt/shaders/glow/`), the hyllian support passes, `include/compat_macros.inc` and
    the `.slangp` preset files carry no licence notice of their own; they are shipped as published in
    slang-shaders.

  GPL texts: `licenses/GPL-2.0.txt`, `licenses/GPL-3.0.txt`. The shaders ship as source, so their
  source is the shipped files themselves.
- **Source:** https://github.com/libretro/slang-shaders/tree/cb01f2f238700eed7ec1c859be1737a9fbcdcb7c

## OpenFX Support library (DaVinci Resolve plug-in)

- **What:** the OpenFX C++ Support library (`Support/Library`, `Support/include`) and API headers,
  release `OFX_Release_1.5.1`, compiled into `NTSCRT.ofx`.
- **Licence:** BSD-3-Clause.
- **Source:** https://github.com/AcademySoftwareFoundation/openfx

```
BSD 3-Clause License

Copyright (c) 2025, OpenFX and contributors to the OpenFX project

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

3. Neither the name of the copyright holder nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## nlohmann/json

- **What:** JSON for Modern C++ v3.11.3 (header-only), used for presets and the parameter schema.
- **Licence:** MIT. **Source:** https://github.com/nlohmann/json

```
Copyright (c) 2013-2022 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## stb

- **What:** `stb_truetype.h` (glyph rasterizer for the Screen Loop counter) in the plug-ins;
  `stb_image.h` / `stb_image_write.h` in the command-line tool and tests only. Pinned at commit
  `5c205738c191bcb0abc65c4febfa9bd25ff35234`.
- **Licence:** the author offers MIT or public domain (Unlicense); used here under MIT.
  **Source:** https://github.com/nothings/stb

```
Copyright (c) 2017 Sean Barrett

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```

## FxPlug SDK (Final Cut Pro plug-in only, not released yet)

The Final Cut Pro bundle embeds Apple's `FxPlug.framework` and `PluginManager.framework` from the
FxPlug SDK, redistributed under the terms of Apple's FxPlug SDK licence agreement. They are not part
of this repository and are not covered by the MIT licence.

## Used to build or test only (not shipped)

- **doctest** 2.4.11 (unit tests), MIT, Copyright (c) 2016-2023 Viktor Kirilov,
  https://github.com/doctest/doctest
- **Slang** compiler (`slangc`, pinned in `cmake/Shaders.cmake`), Apache-2.0 WITH LLVM-exception,
  https://github.com/shader-slang/slang. Only the shaders it compiles end up in the binary.

## Credits without bundled code

- The Glitch stage's design follows Trevor Blackwell's analogtv in xscreensaver; NTSCRT's receiver
  simulation is its own code, written from that design.
- The Digital video stage's quantization tables are the reference tables from the JPEG standard
  (ITU-T T.81, Annex K).
