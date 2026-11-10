# NTSCRT Plugins

[NTSCRT](https://github.com/finnmckenty/NTSCRT)'s analog TV look (ntsc-rs signal emulation, TV/VCR glitch
circuits, RetroArch CRT shaders via librashader, digital codec, Screen Loop) as video-editor effects:

- **DaVinci Resolve 21.1.2+** — OpenFX plugin, Windows and macOS
- **Final Cut Pro 12.4+** — FxPlug 4 plugin, macOS

**Install:** see [INSTALL.md](INSTALL.md). It takes about a minute per app.

Status: in development. See `docs/` for architecture and contracts.

## Credits
All of the look comes from NTSCRT by Finn McKenty, [ntsc-rs](https://github.com/ntsc-rs/ntsc-rs) (MIT/ISC/Apache-2.0),
[librashader](https://github.com/SnowflakePowered/librashader) (MPL-2.0) and the
[libretro slang-shaders](https://github.com/libretro/slang-shaders) authors (various licenses, largely GPL).
Bundled third-party components keep their own licenses.

## Build (Windows)
```
cmake -S . -B C:/ntb/main -G "Visual Studio 17 2022" -A x64
cmake --build C:/ntb/main --config Release -- -m
ctest --test-dir C:/ntb/main -C Release
```
