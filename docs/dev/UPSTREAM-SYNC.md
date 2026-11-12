# Syncing with upstream NTSCRT

The core is a port of [NTSCRT](https://github.com/finnmckenty/NTSCRT) (Swift, macOS), pinned as the
read-only submodule `upstream/NTSCRT`. Current pin: **v0.20.0** (`c6ab639`). This page is the routine for
bringing a later upstream release in: run one script, hand-port the files it lists, and CI checks the rest.

| Tool | What it does |
|---|---|
| [`docs/dev/port-map.json`](port-map.json) | Where every upstream file lands here, and how (its *kind*) |
| `scripts/upstream_diff.py` | Markdown report of an upstream range: what's automatic, what to hand-port, tests, app-only, unmapped |
| `scripts/sync_upstream.py --to <tag>` | The mechanical part: submodule, presets, schema, FxPlug ids, version strings |
| `scripts/check_sync.py` | Is the repo consistent with the pin? (CI job `sync-check`) |
| `.github/workflows/upstream-watch.yml` | Weekly: keeps one `upstream-sync` issue open while upstream has a newer release |

All are stdlib Python 3.9+ and run on Windows and Linux. The submodule is a full clone (not shallow);
CI's `sync-check` job checks it out with `--depth 1`, which is all the checks need.

## 1. Is there something to sync?

The `upstream-watch` workflow runs every Monday (or by hand: Actions → upstream-watch → Run workflow). When
upstream's newest `vX.Y.Z` tag is ahead of the pin it opens (or updates) the issue *Upstream NTSCRT vX.Y.Z is
available*, labelled `upstream-sync`, with the report below; once the pin catches up it comments and closes it.
Dependabot's monthly submodule PR is a second reminder: CI's `sync-check` job fails on it until the sync is done.

Locally:

```sh
git -C upstream/NTSCRT fetch --tags origin            # or pass --fetch below
python scripts/upstream_diff.py --latest-tag          # pin .. newest release
python scripts/upstream_diff.py --to v0.21.0          # pin .. a tag or commit (default: origin/main)
python scripts/upstream_diff.py --from 2b3013d --to c6ab639 --strict --out report.md
```

The report groups every changed upstream file under the port map's roots (`Sources/`, `Tests/`, `presets/`,
`Vendor/ntscrs-capi/`):

- **handled automatically**: presets (copied byte for byte) and schema inputs (`tools/gen_schema.py` parses them);
- **hand-port needed**: upstream file, +/- lines, our target files and notes from the port map;
- **tests to port**: XCTests and the doctest files they go to;
- **vendor pins**: upstream's `Vendor/ntsc-rs` and `Vendor/slang-shaders` before / after, against our `vendor/`;
- **app-only** (collapsed): app UI and infrastructure, read for behaviour only;
- **UNMAPPED**: a file the port map doesn't know. Add an entry (section 3); `--strict` exits 1 while there is one.

It also lists the files outside the roots (README, upstream's own scripts and its `port/` Windows port) and every
upstream commit subject in the range. Read every commit message: upstream explains each behaviour change and its
numbers there. `git -C upstream/NTSCRT log -p <old>..<new> -- <file>` shows one file's history.

## 2. The mechanical sync

```sh
python scripts/sync_upstream.py --to v0.21.0 --dry-run   # what would change; changes nothing
python scripts/sync_upstream.py --to v0.21.0             # do it (refuses a dirty tree)
```

In order, it: moves the submodule to `--to`; copies upstream's `presets/` byte for byte (adds and updates;
a preset upstream deleted is reported, not deleted: delete it here, or keep it on purpose by listing it in the
port map's `retained_presets`); regenerates `core/params/schema.json` with `tools/gen_schema.py --slang-shaders
vendor/slang-shaders` (when the generator refuses a breaking change, the sync stops with its exit code: see
section 4, fix it, then re-run with `--allow-dirty`; `--allow-breaking` is passed through, for use only before a
first release); runs `fxplug/tools/update_param_ids.py` (new FxPlug ids, append-only); and updates the version
strings (the README "Synced with upstream NTSCRT vX" line, the `docs/params.md` look-format line and the pin line
at the top of this page). Then it prints the hand-port list and the manual steps left. It never commits or pushes.

If upstream moved its `Vendor/slang-shaders` or `Vendor/ntsc-rs` pin, the report says so: bump ours to match
(`vendor/slang-shaders` also needs `SLANG_COMMIT` in `tools/gen_schema.py` and the comment in
`core/stages/crt/CMakeLists.txt`; `check_sync.py` names every place).

## 3. Port

`docs/dev/port-map.json` says where each upstream file goes. Each entry has `upstream` (a path, or a glob:
`*` within a directory, `**` any depth; an exact path wins, otherwise the first matching glob), `kind` (one or
more of the kinds below), `ours` (our files; `check_sync.py` checks they exist) and `notes`.

| Kind | Meaning |
|---|---|
| `hand-port` | Port the Swift diff to the C++ in `ours` |
| `generated-input` | `tools/gen_schema.py` parses it, so the schema regenerates (also `hand-port` when C++ mirrors it) |
| `preset` | Copied byte for byte |
| `test-port` | Port new and changed XCTests to doctest |
| `shader-port` | Metal code: port to our slang shaders / CPU path |
| `app-only` | App UI, automation hooks, playback, export encoding: read for behaviour, port nothing |

`check_sync.py` fails when a file under the roots at the pin matches no entry, and when the `generated-input`
entries and the files `gen_schema.py` reads disagree, so a new upstream file always gets classified.

1. Port every hand-port diff. Keep the arithmetic and its order (the codec is deterministic, and staying close to
   upstream's numbers keeps looks matching the app); note deliberate differences at the top of the C++ file.
   `AppState.swift` is mostly app state: port only what changes a look or a render (defaults, look loading,
   `applyDigitalFormat`, `downscaleSpec`).
2. Port the new and changed XCTests next to the existing ones (Metal-only tests: port what they check about sizes
   and shapes, not the GPU call).
3. App-only files: read the diff for behaviour a menu writes into a look; port only that.

## 4. Saved-project compatibility

Hosts store params by **id** and every value, defaults included (checked in Resolve: a Fusion comp lists all
~360 NTSCRT inputs), so a changed **default** only affects new instances. But:

- an id must never disappear (the generator refuses): a removed upstream control stays as a hidden
  `"legacy": true` param (see `DIGITAL_LEGACY` in `tools/gen_schema.py` and docs/params.md);
- a control whose **meaning** changes (e.g. 0.19's Quality curve) needs a new id for the new meaning,
  and the old id becomes legacy, mapped onto the new one in `migrate.h`;
- choice options may only be appended.

## 5. By hand, after the script

1. Bundled preset menu: new presets sort after the released ones. When releasing, append their names to
   `released_preset_order()` in `hostparams/src/params.cpp` (never reorder it). New upstream Digital video formats
   (schema `digital_formats`) are menu entries too: append `Digital video/<name>` the same way.
   Animated presets need nothing: `core/params/timeline.h` ports upstream's `Timeline.swift`; port changes
   to it when upstream changes how keys evaluate.
2. Add the new looks to the Resolve `cli` test (`DIGITAL_LOOKS` in `tests/resolve/run_resolve_tests.py`)
   when they use a stage the test doesn't cover yet.
3. A CHANGELOG entry listing changed defaults and what happens to existing projects, and a line in the sync log
   below.
4. Refresh the golden references (they record the upstream commit they were rendered from, and `check_sync.py`
   compares it with the pin): `gh workflow run golden --ref <branch>`, then
   `gh run download <run-id> -n golden-refs -D tests/golden/ref` (see tests/golden/README.md).
5. `git add upstream/NTSCRT` with the rest, and commit.

## 6. Verify

```sh
python scripts/check_sync.py
cmake --build C:/ntb/<dir> --config Release -- -m
ctest --test-dir C:/ntb/<dir> -C Release --output-on-failure
python tests/resolve/run_resolve_tests.py --headless --bundle C:/ntb/<dir>/ofx/Release/NTSCRT.ofx.bundle --build C:/ntb/<dir>
```

`check_sync.py` (CI job `sync-check`: ubuntu, only `upstream/NTSCRT` and `vendor/slang-shaders`; the NTSC
descriptors come from the committed `core/params/ntsc_descriptors.json`, so no Rust) fails when:

- **a.** regenerating the schema differs from `core/params/schema.json`;
- **b.** a file in `presets/` differs from upstream's blob at the pin (raw bytes, so a flipped byte or a changed
  line ending counts; `.gitattributes` marks `presets/** -text`), upstream has a preset we lack, or we have one it
  doesn't (unless listed in `retained_presets`);
- **c.** `schema.json`'s `upstream_ntscrt` or a `tests/golden/ref/*.json` `upstream_commit` isn't the submodule
  commit, or the submodule checkout isn't the recorded gitlink;
- **d.** the slang-shaders commit differs between the `vendor/slang-shaders` gitlink, `SLANG_COMMIT` in
  gen_schema.py, `schema.json`, `core/stages/crt/CMakeLists.txt` and upstream's `Vendor/slang-shaders`; or
  `vendor/ntsc-rs` differs from upstream's `Vendor/ntsc-rs`;
- **e.** the librashader commit differs between the `vendor/librashader` gitlink, `LIBRASHADER_COMMIT` in
  `.github/workflows/golden.yml` and `core/stages/crt/CMakeLists.txt`;
- **f.** the port map leaves an upstream file unmapped, is malformed, names an `ours` path that doesn't exist, or
  disagrees with gen_schema.py about the generated inputs.

Gitlinks are read from the index (HEAD in CI), so un-initialised vendor submodules are fine. `ci.yml` skips
docs-only pushes except `docs/dev/port-map.json` (its path filter lists it explicitly), so a port-map change
still runs the job; presets are outside `docs/` and always do. `--no-schema` skips check a. Publishing a release
needs the job too.

The Resolve `cli` test renders each Digital look through the plug-in's Preset menu and through `ntscrt-cli` with
the preset file: it checks that the host-side migration (`preset_value`) and the core's agree.

## Upstream references for Digital video, Glitch and Screen Loop

`tests/golden` compares against upstream renders only for CRT, Downscale and ntsc-rs, because upstream's
`crt-smoke` has no other stages. What upstream's own tools can render headlessly (checked at v0.20.0 and at
upstream `main` `8b26092`, November 2026):

| Tool | Renders | Can't |
|---|---|---|
| `crt-smoke` (`Sources/CrtSmoke`) | PNG: image, optional downscale, optional ntsc-rs (`--ntsc`, `--frame`), one `.slangp`, `--set` params | Digital, Glitch, Screen Loop, crop, ScanlineGrid supersampling |
| `crt-video-smoke` (`Sources/CrtVideoSmoke`) | A video through `Mp4Exporter` to H.264 / HEVC / ProRes 422 | `glitch: nil, digital: nil` hard-coded, no ntsc-rs; lossy output |
| `crt-sweep` (`Sources/CrtSweep`) | Numbers: each shader parameter's effect at its min / max | no images, no other stage |
| App hooks (`ContentView.runDevHooks`; need the GUI app `crt-app` running with `CRT_SOURCE`) | `CRT_LOOK=<look.json>` loads any look (every stage); `CRT_DIGITAL="<format>,knob=v,width=480"`, `CRT_GLITCH="id=v,..."`, `CRT_HOWL="knob=v"` set stages; `CRT_EXPORT_MP4=<out>` (+ `CRT_EXPORT_SIZE`, `CRT_SHADER_OFF`) exports H.264 and quits; `CRT_HOWL_RENDER=<out.mp4/gif>` renders Screen Loop; `CRT_DIGITAL_GALLERY=<dir>` writes a **lossless PNG** of every Digital format's chain input (the Digital stage alone, capture size, frame 9) | all but the gallery are lossy (H.264 at width × height × 30 bit/s, or GIF); `Mp4Exporter` doesn't apply Digital under a Screen Loop yet; `CRT_DIGITAL_CHECK` / `CRT_GLITCH_EXPORT_CHECK` are pass/fail self-tests, not reference writers; needs a full app build and a window session |

Upstream `main` after v0.21.0 (not yet in a tag) adds, for upstream's own Rust/wgpu Windows port under `port/`,
the reference tools this needs, each compiled with plain `swiftc` from the **unmodified** Swift sources:

- `port/codec/ref/` (`build.sh`, `main.swift`, `cases.json`): `swiftc -O Sources/CrtCodec/*.swift main.swift
  -framework Accelerate`; `ntscrt-codec-ref run cases.json <dir>` writes raw RGBA for 44 Digital video cases
  (every format, Video 1 and DCT data loss, datamosh with dropped keyframes, chroma modes, low-light noise) from
  synthetic integer-math inputs. CPU only: no Metal, no librashader.
- `port/scripts/glitch-ref/` (Receiver + GlitchParams: the receiver's field plan as JSON) and
  `port/scripts/glitch-ref-render/` (+ GlitchStage, Metal: a picture through the Glitch stage at time *t* with
  knobs, as PNG), driven by `port/scripts/make-glitch-refs.sh`.
- `port/scripts/make-look-refs.py`: `crt-smoke` for every bundled look without Glitch / Digital / crop /
  animation (what golden.yml already does here).
- Screen Loop has no upstream reference tool (`port/engine/tests/howl.rs` only checks self-consistency).

Proposal, in order of value per effort:

1. **Digital video (CPU, exact):** in golden.yml, build the codec reference with `swiftc` (seconds; no SwiftPM,
   librashader or Metal; apply `scripts/ci/patch_upstream_build.py`'s `capture()` rewrite if the runner's
   compiler still can't type-check `DigitalVideoRenderer.swift`), run our own `tests/golden/digital-cases.json`
   and commit the outputs as PNGs under `tests/golden/ref/digital/`. A doctest in `tests/digital` runs the same
   cases through `core/stages/digital/renderer.cpp` and expects **bit-identical** output (deterministic CPU
   code). Until the pin includes `port/`, keep a copy of the 225-line `main.swift` (MIT, with attribution) in
   `scripts/ci/ref/`; after that sync, build upstream's.
2. **Glitch:** build `glitch-ref` / `glitch-ref-render` the same way. The field plans compare numerically with
   `core/stages/glitch/receiver.cpp` (exact, CPU); the renders compare with our Glitch stage alone (GPU, a
   tolerance like ntsc-rs's).
3. **Screen Loop and the full chain:** no upstream tool. Add `scripts/ci/crt-golden/main.swift` as an extra
   executable target appended to upstream's `Package.swift` in the runner's working tree only (as
   `patch_upstream_build.py` patches upstream today), driving `Pipeline.prepareChainInput` (crop, downscale,
   Digital, NTSC), `GlitchStage` and `HowlaroundLoop` and writing a PNG per frame in `Mp4Exporter.exportStill`'s
   order: lossless references for every stage and whole looks, compared like the CRT ones.
4. A quick win without new Swift: `CRT_DIGITAL_GALLERY` with the GUI app on macos-15 gives the six formats'
   Digital-only PNGs, but needs the full app build and a window session on the runner: less robust than 1.

## Sync log

- **v0.20.0** (`c6ab639`, from v0.18.0 `2b3013d`): Digital video CCD response (replaces Exposure, Black
  level), RGB-thirds aliasing with a detail boost above 50%, no Lens softness; Quality on an even curve
  (looks version 2, version 1 migrated); Microsoft Video 1 codec and the CD-ROM format; One field by default
  and sent down the chain as a field; formats in the Preset menu; bundled looks *Chunky webcam* and
  *CD-ROM (low quality)*; Compression group first. App-only (not ported): the CRT_DIGITAL `width=` and
  CRT_EXPORT_MP4 automation hooks, the panel's codec-dependent hiding (here: `visible_if` on `codec`),
  preview sizing, README; licences handled separately.
