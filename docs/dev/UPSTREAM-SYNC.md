# Syncing with upstream NTSCRT

The core is a port of [NTSCRT](https://github.com/finnmckenty/NTSCRT) (Swift, macOS), pinned as the
read-only submodule `upstream/NTSCRT`. Current pin: **v0.20.0** (`c6ab639`). This page is the routine for
bringing a later upstream release in.

## 1. Bump the pin and list what changed

```sh
cd upstream/NTSCRT
git fetch origin main                     # the submodule is shallow: add --unshallow once if needed
OLD=$(git rev-parse HEAD)
git log --oneline $OLD..origin/main       # one line per upstream commit
git diff --stat $OLD origin/main
git log -p $OLD..origin/main -- Sources Tests presets   # the part that matters
git checkout origin/main
cd ../.. && git add upstream/NTSCRT
```

Read every commit message: upstream explains each behaviour change and its numbers there.

## 2. What maps where

| Upstream | Here | Notes |
|---|---|---|
| `Sources/CrtCodec/DigitalVideoParams.swift` | `core/stages/digital/renderer.{h,cpp}` (`kParams`, `formats()`), schema via `tools/gen_schema.py` | ids, ranges, defaults, `structureIDs`, `unusedBy`, formats |
| `Sources/CrtCodec/DigitalVideoRenderer.swift` | `core/stages/digital/renderer.cpp` | camera (scale, capture), stream, render |
| `Sources/CrtCodec/DigitalCodec.swift` | `core/stages/digital/codec.{h,cpp}` | DCT codec, quality curve |
| `Sources/CrtCodec/Video1Codec.swift` | `core/stages/digital/video1.{h,cpp}` | Microsoft Video 1 |
| `DigitalSettings.migrated`, `AppState` look loading | `core/stages/digital/migrate.h` | used by the core and both host param models |
| `Sources/CrtCore/DigitalStage.swift` | `core/stages/digital/digital.cpp` | capture size |
| `Sources/CrtCore/Pipeline.swift` (`DownscaleSpec`), `AppState.downscaleSpec` | `core/stages/digital/fields.h`, `core/stages/downscale/downscale.cpp`, `FrameContext::line_height` | One field |
| `Sources/CrtCore/ScanlineGrid.swift` (`SupersampledPass`, `snappedSize`) | `core/stages/crt/crt.cpp`, `crt_sizing.h` | |
| `Sources/CrtCore/Downscaler.swift`, `Views/DownscalePanel.swift` | `core/stages/downscale/`, schema | |
| `Sources/CrtCore/GlitchParams.swift`, `Receiver.swift`, `GlitchStage.swift` | `core/stages/glitch/` | |
| `Sources/CrtCore/Presets.swift`, `AppState.appShaderDefaults` | `core/stages/crt/`, schema | |
| `Sources/CrtCore/Howlaround.swift` | `core/stages/howlaround/` | Screen Loop |
| `AppState.appNtscDefaults`, `NtscSettingsModel.swift` | `core/stages/ntsc/ntsc.cpp`, `tools/gen_schema.py` | |
| `presets/*.json` | `presets/` | **byte-identical copies** (`git show origin/main:presets/<name>.json > presets/<name>.json`) |
| `Tests/CrtCoreTests/*Tests.swift` | `tests/<stage>/test_*.cpp` (doctest) | port new and changed tests |
| `Sources/CrtApp/Views/*`, `ContentView.swift` env hooks (`CRT_*`), export, DMG scripts | nothing | app UI / app automation: read them for behaviour (what a menu writes), port only that |

## 3. Port

1. Port every Swift diff under `Sources/CrtCodec`, `Sources/CrtCore` and the look-loading parts of
   `Sources/CrtApp/AppState*.swift`. Keep the arithmetic and its order (the codec is deterministic, and staying
   close to upstream's numbers keeps looks matching the app); note deliberate differences at the top of the C++ file.
2. Port the new and changed XCTests to doctest next to the existing ones (Metal-only tests: port what they
   check about sizes and shapes, not the GPU call).
3. Copy new or changed `presets/*.json` byte for byte; check with
   `git hash-object presets/<f>` = `git -C upstream/NTSCRT rev-parse origin/main:presets/<f>`.
4. Regenerate the schema: `python tools/gen_schema.py --slang-shaders vendor/slang-shaders`, then
   `python fxplug/tools/update_param_ids.py` (new FxPlug ids, append-only).
5. Saved projects: hosts store params by **id** and every value, defaults included (checked in Resolve:
   a Fusion comp lists all ~360 NTSCRT inputs), so a changed **default** only affects new instances. But:
   - an id must never disappear (the generator refuses): a removed upstream control stays as a hidden
     `"legacy": true` param (see `DIGITAL_LEGACY` in `tools/gen_schema.py` and docs/params.md);
   - a control whose **meaning** changes (e.g. 0.19's Quality curve) needs a new id for the new meaning,
     and the old id becomes legacy, mapped onto the new one in `migrate.h`;
   - choice options may only be appended.
6. Bundled preset menu: new presets sort after the released ones. When releasing, append their names to
   `released_preset_order()` in `ofx/ofx_params.cpp` (never reorder it).
7. Add the new looks to the Resolve `cli` test (`DIGITAL_LOOKS` in `tests/resolve/run_resolve_tests.py`)
   when they use a stage the test doesn't cover yet.
8. Record the version: the README status line, the pin line at the top of this page, the
   `docs/params.md` look-format line, and a CHANGELOG entry listing changed defaults and what happens to
   existing projects.

## 4. Verify

```sh
cmake --build C:/ntb/<dir> --config Release -- -m
ctest --test-dir C:/ntb/<dir> -C Release --output-on-failure
python tests/resolve/run_resolve_tests.py --headless --bundle C:/ntb/<dir>/ofx/Release/NTSCRT.ofx.bundle --build C:/ntb/<dir>
```

The `cli` test renders each Digital look through the plug-in's Preset menu and through `ntscrt-cli` with
the preset file: it checks that the host-side migration (`preset_value`) and the core's agree.

## Sync log

- **v0.20.0** (`c6ab639`, from v0.18.0 `2b3013d`): Digital video CCD response (replaces Exposure, Black
  level), RGB-thirds aliasing with a detail boost above 50%, no Lens softness; Quality on an even curve
  (looks version 2, version 1 migrated); Microsoft Video 1 codec and the CD-ROM format; One field by default
  and sent down the chain as a field; formats in the Preset menu; bundled looks *Chunky webcam* and
  *CD-ROM (low quality)*; Compression group first. App-only (not ported): the CRT_DIGITAL `width=` and
  CRT_EXPORT_MP4 automation hooks, the panel's codec-dependent hiding (here: `visible_if` on `codec`),
  preview sizing, README; licences handled separately.
