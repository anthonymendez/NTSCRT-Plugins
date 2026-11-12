# Changelog

All notable changes to the NTSCRT plug-ins (DaVinci Resolve OpenFX and Final Cut Pro FxPlug).
The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the version
follows [Semantic Versioning](https://semver.org/):

- **patch** (0.1.0 → 0.1.1): bug fixes only; looks and saved projects render the same or better.
- **minor** (0.1.x → 0.2.0): new controls or features; saved projects still load.
- **major** (0.x → 1.0, 1.x → 2.0): a change that breaks saved projects or presets.

The version lives in [`VERSION`](VERSION): the version of the next release (the one the
**Unreleased** section becomes). Every change adds a line under **Unreleased**;
`python scripts/release.py` turns that section into a release (see [docs/RELEASING.md](docs/RELEASING.md)).

## [Unreleased]

Animated presets, the Digital video formats in the Preset menu and a second codec (Microsoft Video 1, for the
CD-ROM look), synced with upstream NTSCRT v0.21.0. Faster seeks and edits, less memory with several effects,
half-float CRT output on 16-bit and float timelines, and fixes for Horizontal resolution and preset files.
Installs are now checked against the release's checksums. Still Resolve-only: the Final Cut plug-in is
written but not yet built.

### Added
- **Animated presets:** choosing *Glitch pop*, *15 second glitches* or another bundled look with upstream
  timeline keys now plays its animation. The keys become keyframes, evaluated as the NTSCRT app does (easing,
  step snapping, held toggles): on each moving control a key where its curve bends (Resolve draws straight lines
  between keys; within 0.5% of the control's range of the app's value on every frame, exact for whole numbers,
  Glitch timing controls and wherever a value holds), two per change on checkboxes and menus. The timeline lasts
  its duration at the clip's frame rate and loops from the clip's first frame; the keys cover whole loops around
  the playhead, as many as about 3000 keyframes allow (at least one loop, at most 3600 frames; the first / last
  values hold outside them).
- **Digital video formats** (Webcam (2005), Video call, MiniDV, Early web video, CD-ROM (Video 1), Datamosh) in the
  Preset menu as *Digital video/…*, as in upstream 0.20: they switch Digital video on and set its controls and the
  Downscale width, leaving the rest of the look alone. They come after this release's new looks in the menu.
- Digital video: a second codec, **Microsoft Video 1** (*Codec: Block colors*), used by the **CD-ROM (Video 1)**
  format.
- Bundled looks from upstream: *Chunky webcam*, *CD-ROM (low quality)*.
- Looks and preset files saved by older NTSCRT versions or the 0.4.0 plug-ins are converted when they load (core
  and both hosts).
- [Presets gallery](docs/PRESETS-GALLERY.md): an animated GIF of every bundled look, Digital video format,
  Screen Loop preset and CRT shader, linked from the README.
- Release candidates: `vX.Y.Z-rc.N` tags are published as GitHub prereleases, which the installers' default
  (`latest`) skips; testers install one with `-Version` / `--version vX.Y.Z-rc.N`.
- Releases include `SHA256SUMS` and a signed build-provenance attestation for every file
  (`gh attestation verify <file> -R anthonymendez/NTSCRT-Plugins`).
- MIT licence (`LICENSE`), shared with upstream NTSCRT, which the core is ported from.
- `THIRD_PARTY_NOTICES.md` and `licenses/` (MIT, MPL-2.0, GPL-2.0, GPL-3.0, Apache-2.0 and every Rust crate's
  licence in `licenses/rust-crates.txt`). Both bundles ship them in `Contents/Resources/`, plus crt-royale's own
  `LICENSE.TXT` next to its shaders, and the release zips carry `LICENSE` and `THIRD_PARTY_NOTICES.md` at their
  root.
- `SECURITY.md`, `CONTRIBUTING.md`, issue forms (the bug form asks for the `ntscrt_doctor.py` zip, the Resolve
  version and the GPU) and a pull-request template.
- With every stage off the effect passes the source through untouched (OpenFX isIdentity; Resolve uses it),
  and a source with non-square pixels gets a warning on the effect (it is processed as square).
- For Resolve and other OpenFX hosts, the plug-in declares the source frames it reads (getFramesNeeded), says
  it is best rendered in frame order (sequential render 2), stops fetching earlier frames when the host aborts
  a render, takes interlaced sources as whole frames and renders both fields of a frame as that frame (a field
  at t + 0.5 used the next frame), and asks the host for RGBA when the source is RGB.
- **Debug logging for all effects:** a *Turn on / off debug logging for all effects* button next to *Debug
  logging* (Final Cut: *Debug Logging for All Effects…*) logs every frame of every NTSCRT effect in detail, on
  every clip and in every project, until it is pressed again, without ticking each effect; a status line shows
  whether it is on. It is a switch outside the project (`debug-logging-all-effects.txt` next to the log), seen
  by open effects within a second, logged when effects are created and when it changes, and reported by
  `ntscrt_doctor.py`. `NTSCRT_DEBUG_LOG=1` does the same for one Resolve session; `NTSCRT_SETTINGS_DIR` moves
  the switch (tests). See docs/TESTING.md.

### Changed
- **Glitch** and **Digital video** (with Datamosh) past the first minute of an effect look slightly different
  from 0.5.0-rc.2: the Glitch receiver's simulated history (sync, vertical roll, colour lock) restarts at every
  minute boundary of the effect (a minute of timecode: 01:00:00:00, 01:01:00:00, ...; 60 s of the rounded frame
  rate), as if the TV were switched on there, and Datamosh can't drop the first keyframe of a minute, so the
  stream never reaches further back. A rolling picture (Vertical hold off centre) can jump once a minute. The
  first minute of an effect renders exactly as before (the NTSCRT app's behaviour); this bounds what a frame or
  an edit costs anywhere else (seconds per hour of timeline before). Existing projects with Glitch on render
  differently past their first minute, and on an Adjustment Clip, whose frames are timeline frames, everywhere
  but in the timeline's first minute.
- One parameter model for both plug-ins: the new host-independent library `hostparams/` builds the look, applies
  presets (keyframes, Digital video formats, choice applies, migration), evaluates the grey-out / hide rules,
  orders the Preset menus and writes the log for the Resolve and the Final Cut plug-in alike, replacing
  `ofx/ofx_params.*` and most of `fxplug/model/param_model.*`. Resolve renders exactly as before (a test compares
  every bundled preset's look with what the old code built). Where the two differed, Final Cut now does what
  Resolve does (table in docs/params.md): looks are built from the controls alone (no hidden saved preset copy),
  the Screen Loop menu's Off / (Custom) switch the Screen Loop checkbox, conditions and choice labels are read
  the same way, the Preset menu keeps the released order, Seed goes up to 2147483647.
- Synced with upstream NTSCRT v0.21.0 (was v0.20.0): Digital video gains **Low-light noise** (grain that grows in
  dark scenes, up to 8x, as a camera turns up its gain) and **Halo width** (how far Sharpening's outlines reach,
  1-4 px), and the bundled look *Cheap camcorder*. Both new controls default to off / 1 px, so existing projects
  and looks keep their settings; but upstream changed the sensor-noise random pattern (rows no longer repeat), so
  any look with Sensor noise above 0 shows different grain of the same strength (mean difference per pixel, of 255:
  *Chunky webcam* 2.6 to 5.8, *CD-ROM (low quality)* about 4.9, *Webcam (low quality)* about 1.0, the Webcam
  (2005), Video call and MiniDV formats 1.2 to 3.4); looks without Sensor noise (Early web video, CD-ROM (Video 1),
  Datamosh) are unchanged.
- Synced with upstream NTSCRT v0.20.0 (was v0.18.0). Digital video: **CCD response** replaces Exposure and
  Black level (a 90s 3-CCD camcorder's auto exposure, flare, knee and setup lift); Aliasing samples red,
  green and blue a third of a pixel apart (rainbow moiré) and boosts fine detail above 50%; Lens softness is
  gone; **Quality** runs on an even curve; the Compression group comes first.
- Digital video **Fields** defaults to **One field**, and One field now sends the CRT the field itself (half
  the Downscale's lines, each two rows tall): a scanline per field line, the 240p look.
- Changed defaults (new instances only; Resolve saves every value with the project, so existing instances
  keep theirs): Digital Fields Progressive → One field; Quality 0.22 → 0.749 on the new curve (the same
  quantizer). Existing 0.4.0 projects with Digital video on: Quality and Exposure / Black level are kept as
  hidden "(from 0.4.0)" controls that show up in those instances and are converted as upstream does (same
  quantizer; CCD response from the stronger of Exposure / Black level); Lens softness no longer has an effect
  and Aliasing looks different (upstream's new sampling).
- Choosing a look preset clears the keyframes of every control it sets (it replaces the look; undo restores
  them). Before, a keyframed control got a key at the playhead and kept its other keys.
- Bundled presets keep their place in the Preset menu (saved projects store the index); new ones are added
  after the released ones.
- 16-bit and 32-bit float projects get the CRT shader's result at half-float precision instead of 8 bits: a
  smooth gradient through Glow (Gauss) comes out with ~2350 levels instead of ~250, so it no longer bands when
  it's graded afterwards. 8-bit projects, and looks with the CRT shader off, render exactly as before
  (`NTSCRT_OUTPUT_8BIT=1` brings back the 8-bit output).
- Several NTSCRT effects in one project share one GPU device and one set of compiled GPU kernels, and on
  Windows also one compiled shader chain per CRT preset without frame history (all but crtsim; on macOS each
  effect keeps its own chain). With 8 instances at 1080p (Glow (Gauss)): instances are created in ~0 ms
  instead of ~21 ms each, the first frames take ~0.9 s instead of 1.6 s in total, and the process uses
  ~390 MB instead of ~1140 MB of memory and ~190 MB instead of ~410 MB of GPU memory. Rendering waits for the
  GPU on an event instead of spinning a CPU core.
- Faster frames, mostly on large pictures: converting the host picture to and from the internal format runs on
  all CPU cores and no longer calls a slow rounding function per channel (the result is bit-identical), and the
  read-back buffer is kept between frames. A UHD frame with the CRT shader off now takes ~15 ms instead of
  ~250-330 ms in the converter benchmark; a 1080p Glow (Gauss) frame ~20 ms instead of ~60 ms.
- Less work per frame: opaque sources (most video) no longer get a per-pixel alpha buffer, the pixel-format
  conversion loops are specialised per format, an uncropped picture is moved instead of copied, identical look
  JSON is parsed once (the host sends the same text for every frame of an unanimated look), the history of
  other frames' looks is bounded, and upload / output / Glitch textures are reused across frames. Output is
  unchanged.
- **Screen Loop seeks:** the loop keeps checkpoints of what it has rendered (every 30 frames and at each window
  start, within 256 MB of GPU memory per effect, `NTSCRT_SCREENLOOP_CHECKPOINT_MB`), so a jump back, or to a
  frame near one rendered before, resumes from the closest one instead of re-running the loop from its window
  start: in Resolve, jumping between frames 100-141 of a clip now takes ~0.1-0.5 s instead of ~4-6 s. The loop's
  NTSC pass for earlier frames also runs on worker threads while the next frames are prepared, so a first jump
  far into a clip is ~1.5x faster (1080p, frame 600: ~6.7 s instead of ~10.3 s; UHD ~26 s instead of ~41 s).
  Every frame renders byte-identically to before.
- The install scripts and installers check the downloaded zip against the release's `SHA256SUMS` and
  refuse to install if it is missing or doesn't match (`-From` / `--from` with your own zip skips the check).
  v0.3.0 and v0.4.0 predate `SHA256SUMS` and install with a warning, without verification. `-Version` / `--version` / `NTSCRT_VERSION` accept only `latest` or
  `vX.Y.Z[-rc.N]`.
- macOS: `install.sh` only removes the quarantine flag and ad-hoc signs the plug-in when it has no real
  signature. The .pkg installer logs to `/Library/Logs/NTSCRT/install.log` (and `/var/log/install.log`)
  instead of a fixed file in /tmp.
- `ntscrt_doctor.py` removes your user name, home folder and computer name from everything in its zip (and Mac
  serial numbers / hardware UUIDs), and trims crash reports to the crash itself. Non-text files are left out.

### Fixed
- Resolve: **choosing an animated preset an hour into a timeline** (the default 01:00:00:00 start), on an
  Adjustment Clip, froze Resolve for about a minute and put the animation an hour before the clip. Resolve gives
  an Adjustment Clip timeline frame numbers with a made-up 0..1999 range, so the keys went to frames 0..3599
  (97,200 keyframes for *5 second blips* at 60 fps) and the next frame rebuilt the look of every frame since
  frame 0 for the Glitch receiver (216,000 looks, 37 s at UHD). The keys now cover the playhead and are sparse
  (2,620 for that preset, written in ~0.1 s), and the first frame after it takes ~35 ms in the mock host at
  640x360 instead of 28 s. Frames numbered from the clip's start are used whenever the host time lies in the
  clip's range (clips on the Edit page, Fusion); otherwise frame numbers are timeline frames.
- Resolve: an hour or more into a timeline, the first frame and every Glitch or Downscale edit no longer
  re-simulate the Glitch receiver from the timeline's start (~2.6 s at 640x360, ~5 s at UHD in Resolve; now
  ~10 ms, the same as at the start): its history, and Digital video's stream with Datamosh, restart every
  minute of the effect (see Changed). Keyframed (animated) looks only re-read the keyframed controls for the
  earlier frames the receiver replays, not all ~300.
- Resolve, Adjustment Clips and other items without media: Digital video and Screen Loop got none of the
  earlier frames they read (the plug-in refused frames outside the made-up 0..1999 range), and the effect told
  Resolve it needed frames 1999 to the current one (214,000 frames an hour in). Both now use the frames around
  the current one.
- Resolve: far into a timeline (an hour in, the default 01:00:00:00 start), choosing a preset, editing the
  CRT shader or pressing *Load* with no file no longer re-runs the Glitch receiver from the timeline's first
  frame (seconds per hour; in 0.4.0 every edit there took ~45 s at UHD). Param edits no longer freeze
  Resolve's interface while a frame is rendering: the caches they drop are dropped before the next render.
- The one-line installs (`install.ps1` / `install.sh` from `main`) work again with v0.3.0 and v0.4.0, which
  predate `SHA256SUMS`: they resolve `latest` to its tag and install those two releases with a warning
  instead of refusing them; a missing `SHA256SUMS` on any other release is still refused.
- Final Cut: choosing or loading a Screen Loop preset file without an `"enabled"` key turns Screen Loop on in the
  shared model itself (the core's "missing means on" rule), replacing a workaround in the plug-in.
- Resolve: loading the plug-in no longer creates and destroys a GPU device just to list the bundled presets
  (about 140 ms at every describe); they are read straight from the bundle. Final Cut likewise.
- Faster seeks and edits in Resolve with Glitch on (the default): when no parameter is keyframed the plug-in no
  longer rebuilds the look from all ~300 parameters for every earlier frame the Glitch receiver replays (up to
  18 per frame while playing, every frame from 0 after an edit); keyframed looks are built once per frame and
  reused until a parameter changes. An edit now only drops the caches that parameter can affect (a CRT tweak no
  longer replays Glitch from frame 0; debug logging, presets menus and preset files drop nothing).
- Source-frame cache memory: it kept up to 65 full-size frames per effect with no limit (about 2 GB at UHD with
  Screen Loop); it now stays within 512 MB per effect (`NTSCRT_SOURCE_CACHE_MB` changes it, see
  docs/ENVIRONMENT.md), dropping the least recently used frames. Frames are also keyed by render scale, size,
  alpha mode and crop, so switching to proxy or editing the crop never reuses a wrong-size or stale picture.
- **Downscale → Horizontal resolution** sets the Width again, and editing the Width by hand switches it to
  *Custom*, as in the NTSCRT app. The Digital video formats' labels (*MiniDV (720px)*, …) are added to its
  options.
- Saved presets are upstream's JSON: they carry `"version": 2` and the ntsc-rs settings version, and Glitch /
  Screen Loop toggles are stored as 1/0. **Save preset…** now also saves the Screen Loop settings (under
  `"screen_loop"`, which Load applies, plus `<name> (Screen Loop).json` while the loop is on). It saves the look
  at the playhead, not the keyframes.
- A CRT shader that can't load (missing or broken preset, librashader missing) no longer fails every frame and
  retries the load each time: the frame renders without the shader and the reason is shown once (Resolve: a
  persistent warning on the effect, plus the log). `ntscrt-cli` writes such frames and prints a warning.
- After the GPU device is lost (driver reset, GPU removed) the effect makes a new device and renders the frame
  again, instead of failing every frame from then on; if that fails it keeps rendering with the CPU stages and
  says so. Without a GPU, a cropped picture now fills the whole frame (letterboxed) instead of leaving part of it
  unwritten.
- The plug-in no longer risks hanging when the host unloads it: the Digital video worker threads are
  stopped when the last NTSCRT instance is deleted, never during unload.
- A NaN alpha in a 16-bit float or 32-bit float source now counts as transparent instead of coming back as an
  infinite alpha (and infinite colour when premultiplied).
- macOS: `install.sh --uninstall` left the Resolve plug-in installed when DaVinci Resolve itself wasn't
  installed; it now removes both plug-ins and forgets the .pkg installer's receipt.
- `ntscrt_doctor.py` collects the macOS installer log from `/Library/Logs/NTSCRT/` (where the .pkg writes it).
- Final Cut plug-in (not released yet, never run in Final Cut): Screen Loop presets turn the loop on (they
  rendered nothing); earlier source frames are requested only for Digital video and Screen Loop, as many as they
  read (Glitch alone asked for 8 per frame); each effect instance has its own renderers and caches (two clips
  shared history) and parameter generation; looks of earlier frames are read only when something is keyframed and
  built once per edit (was up to 16 parameter sweeps per frame); degraded frames (CRT shader skipped, a source
  frame missing) count as rendered and are logged once; an edit drops only the caches it affects; the frame's other
  tiles reuse its render; `MayRemapTime` is now YES (it reads frames before the output time) and
  `PixelTransformSupport` says Scale; a look built over a version 1 preset no longer says version 1 (the core
  converted Digital video Quality twice). Like the OpenFX plug-in it now turns animated presets into
  keyframes, lists the Digital video formats in the Preset menu, applies Horizontal resolution's Width (and
  the other menus that set values) and lets keyable menus take keyframes; saved looks carry the Screen Loop
  settings.

### Developer
- Tests for long timelines: `test_ofx` applies *5 second blips* an hour into a 60 fps timeline on a mock
  Adjustment Clip (Resolve's made-up 0..1999 range) and on a clip starting at frame 216000 (keyframes, apply
  time, first render, Glitch edit, playback and seeks against renders after a purge), compares the edits an hour
  in with the same edits near frame 0, and renders sparse keys against a key on every frame (identical pixels);
  `test_hostparams` checks every bundled animated preset's keys against the per-frame evaluation at four frame
  rates (linear and smooth-spline hosts). The Resolve test `position` puts the effect on Edit-page items starting
  at 01:00:10:00, logs the frame ranges Resolve reports, applies an animated look there (through
  `NTSCRT_OFX_TEST_PRESET_FILE`, as scripts can't set OpenFX params on the Edit page) and compares the frames
  with `ntscrt-cli --timeline`; it also measures how the Edit and Fusion pages interpolate keyframes (linearly).
  The plug-in logs the host's frame ranges when an effect is created, at its first render and when a preset is
  applied.
- `test_howl_seek`: Screen Loop through the C ABI on the GPU, cold seeks, scrubbing and playing on after a seek
  checked byte-identical to playing from frame 0 (checkpoints on and off); engine tests for checkpoints and
  pipelined passes; `bench_screenloop` (playback and seek timings, run by hand); the Resolve `speed` test lists
  a Screen Loop instance's render times across jumps.
- Golden image tests now compare something: 11 references rendered by upstream NTSCRT's own `crt-smoke`
  (golden workflow, macOS; the CRT shaders, downscale methods and ntsc-rs presets) and 52 self-reference
  baselines from the plug-ins' core (Digital with both codecs, Glitch over time, every Screen Loop preset,
  alpha Clip / Spill, crop, a moving sequence, proxy render scale, every bundled look), checked strictly on
  Windows and macOS in CI. `cmake --build <build> --target golden-update` regenerates the baselines after an
  intended look change (tests/golden/README.md).
- The golden workflow builds upstream's `crt-smoke` again (a build-time workaround for a Swift type-checker
  limit in upstream's `DigitalVideoRenderer.swift`, applied on the runner only).
- Ported upstream tests: Video1Tests, FieldLinesTests and the DigitalVideoTests changes; the Resolve `cli`
  test also compares the bundled Digital video looks against `ntscrt-cli`.
- Tests: timeline evaluation against upstream's rules and a direct evaluation, preset keyframes in the OFX and
  FxPlug models and through the mock OpenFX host, choice `applies`, preset file round trip; 16-bit and
  half-float frames (every alpha value round-trips; NaN, infinities, subnormals and out-of-range values come
  out in range), proxy render scale and crop with a transparent source.
- Resolve test suite: the new `animated` test compares an animated preset against `ntscrt-cli --timeline` and
  checks Horizontal resolution and a Digital video format; `speed` also reports the plug-in's time for a frame
  after a seek, a CRT edit, a Glitch edit and with keyframed Glitch values.
- `ntscrt-cli --timeline`: plays a look's timeline keys per frame, as the plug-ins key them.
  `core/params/timeline.h` holds the evaluation the plug-ins share; the FxPlug model gains `preset_keys`.
- Core: `ntscrt_source_frames_needed` tells a host which earlier source frames a render can fetch (Digital video
  back to its keyframe, Screen Loop's window; none for Glitch or the CRT), and `ntscrt_flush_caches_mask` lets a
  host drop only the caches an edit can affect.
- CI: the Final Cut plug-in's Objective-C++ is syntax-checked on macOS against stub FxPlug headers
  (`fxplug/tests/stubs`), so it is compiled even without the FxPlug SDK secret.
- `scripts/gen_rust_licenses.py` regenerates `licenses/rust-crates.txt`.
- `scripts/make_presets_gallery.py` regenerates the presets gallery's GIFs (through the plug-in in headless
  Resolve, or `ntscrt-cli`) and its page. It, `make_previews.ps1` and `make_previews_resolve.py` take the build
  folder from `--build` / `-Build` or `NTSCRT_BUILD_DIR` (they had a fixed path), and the README previews now
  choose bundled presets from the Preset menu as a user would.
- README previews are animated WebP instead of GIF (12.7 MB → 6.3 MB), rendered from the full-colour frames
  (no 128-colour palette) through the plug-in in Resolve, with the upstream 0.20 looks; *V Hold glitch* now
  plays its animation.
- Docs: the README no longer says "in development", says clearly that the Final Cut plug-in isn't released, and
  has system requirements, known issues, a Licence section and macOS build steps; docs/ENVIRONMENT.md lists
  every environment variable; docs/dev/UPSTREAM-SYNC.md says how to sync with a later upstream release; the
  agent working notes moved to `docs/dev/` (`AGENTS.md`, `CONTRACT-CHANGES.md`).
- Tests for debug logging for all effects: `test_hostparams` (the shared switch: location, polling, one report
  per change across threads), `test_ofx` (button, status line, detailed renders, a change made outside the
  host) and the Resolve test `debugall`; the mock-host and headless Resolve tests keep the switch in their own
  folder (`NTSCRT_SETTINGS_DIR`) and ignore the shell's `NTSCRT_DEBUG_LOG`.
- Upstream sync tooling: `docs/dev/port-map.json` maps every upstream file to its kind and our targets;
  `scripts/upstream_diff.py` reports what an upstream range needs (hand-ports, tests, presets, vendor pins);
  `scripts/sync_upstream.py --to <tag>` does the mechanical sync (submodule, byte-identical presets, schema,
  FxPlug ids, version strings; `--dry-run`); CI job `sync-check` (`scripts/check_sync.py`) fails when the schema,
  presets, golden refs or vendor pins disagree with the upstream pin or the port map misses a file; the weekly
  `upstream-watch` workflow keeps one `upstream-sync` issue open while upstream has a newer release.

## [0.4.0] - 2026-11-10

Debug logging and a one-click logs folder for bug reports, and README previews rendered by the plug-in in
Resolve. Still Resolve-only: the Final Cut plug-in is written but not yet built.

### Added
- *Presets → Debug logging* checkbox (Resolve and Final Cut): logs every frame that effect renders, with
  per-phase timings and the full look, for bug reports. Presets never change it; Resolve doesn't save it with the project.
- *Presets → Open logs folder* button: shows the NTSCRT log folder in Explorer / Finder (when Resolve runs
  without a window, `-nogui`, it only logs the press).
- Final Cut plug-in: a log file like the Resolve one (`ntscrt-fxplug.log` in the renderer's sandbox container,
  `~/Library/Containers/<renderer>/Data/Library/Logs/NTSCRT/`). Load lines and failures also go to the system log.
- `ntscrt_doctor.py` collects the Final Cut log and the last 24 hours of `[NTSCRT]` system-log lines on macOS.
- Resolve test suite: `debug` test (Debug logging logs every frame without changing the picture).

### Fixed
- `run_resolve_tests.py --bundle` and `launch_resolve_dev.ps1` tested the *installed* plug-in whenever it had the
  same version as the build (Resolve keeps one plug-in per id and version). The dev copy now outranks it
  (`NTSCRT_OFX_DEV_BUNDLE`).

### Changed
- README: the before/after preview is now an animated GIF (`scripts/make_previews.ps1` renders it with ffmpeg).
- README previews: the looks grid is an animated GIF too, and both GIFs are now rendered by the OpenFX plug-in in
  DaVinci Resolve (headless) with the new `scripts/make_previews_resolve.py`, checked against `ntscrt-cli`.

## [0.3.0] - 2026-11-10

First public release. (0.1 and 0.2 were internal milestones and were never tagged.)

**This release contains the DaVinci Resolve plug-in for Windows and macOS.** The Final Cut Pro plug-in is
in the source but not in this release: CI builds it only when Apple's FxPlug SDK is available to it.
The macOS installer skips Final Cut Pro until a release includes it.

### Added
- DaVinci Resolve 21.1.2+ OpenFX plug-in (Windows x64 with Direct3D 11, macOS universal with Metal) and
  Final Cut Pro 12.4+ FxPlug 4 plug-in (macOS universal), sharing one C++ core ported from the NTSCRT app:
  crop, Digital video codec, NTSC/VHS signal (ntsc-rs), downscale, TV/VCR glitch, CRT shaders
  (librashader, 7 presets) and Screen Loop video feedback.
- Every parameter keyframable; bundled look and Screen Loop presets; load/save NTSCRT preset files.
- Random-access rendering matches sequential playback for the temporal stages.
- Double-click installers: `NTSCRT-Installer-windows.msi` (Windows, with an "Update NTSCRT plug-in" Start-menu
  item; uninstalling it removes the plug-in) and `NTSCRT-Installer-macos.pkg` (macOS). They always download and
  install the latest release.
- **Transparency → Transparent areas** for titles and clips that don't fill the frame: *Clip to element*
  (effect stays inside the element, clean edges) or *Let effect spill* (ghosting, smear and glow extend past
  it as light over what's underneath). Presets don't change it.
- **Version** shown in the plug-in's controls (Resolve).
- One-line installers: `install.ps1` (Windows) and `install.sh` (macOS, Resolve and Final Cut).

### Fixed
- Installers replace the plug-in atomically: a failed or interrupted install leaves the previous version
  working, and installing while Resolve is open works (the new version loads on restart).
- Premultiplied sources no longer get dark fringes or halos at soft edges.
- Fusion: the effect failed (black output) on sources that don't cover the frame or have no frame rate,
  such as Text+ titles.
- CRT shaders aperture and easymode looked wrong (darker, a much stronger mask): the plug-in also sent
  the other CRT presets' settings, and presets that share a setting name (gamma, mask strength)
  overwrote each other's values. Now only the selected preset's settings are sent (Resolve and Final Cut).
- Windows: the CRT shaders needed the legacy DirectX 9 runtime (D3DX9_43.dll), which a clean Windows
  doesn't have, so the CRT stage failed to load there. The bundled librashader now only contains the
  Direct3D 11 (Windows) / Metal (macOS) runtime it uses.

### Developer
- `tests/resolve/`: plug-in tests inside a running DaVinci Resolve (alpha modes, Edit page, out-of-order
  frames, plug-in vs `ntscrt-cli`, bit depths, sizes, speed) and `launch_resolve_dev.ps1` to start
  Resolve with a build without installing it. Runs unattended: `--headless` starts Resolve with
  `-nogui`; frames are captured through the render queue (Fusion's own renders open a dialog).
- `scripts/ntscrt_doctor.py`: checks an install and collects a debug zip (system/GPU info, install state,
  plug-in log, NTSCRT lines of Resolve's log, crash reports, installer logs, optional Resolve test run).
  docs/TESTING.md walks a tester through installing, checking and reporting.
- The Resolve plug-in keeps a small diagnostic log (load, instances, first renders, every failure):
  `%LOCALAPPDATA%\NTSCRT\ntscrt-ofx.log` / `~/Library/Logs/NTSCRT/ntscrt-ofx.log`, rotated at 1 MB.
  `NTSCRT_OFX_LOG=<file|off>` moves or disables it, `NTSCRT_OFX_LOG_VERBOSE=1` logs every frame.

[Unreleased]: https://github.com/anthonymendez/NTSCRT-Plugins/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/anthonymendez/NTSCRT-Plugins/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/anthonymendez/NTSCRT-Plugins/releases/tag/v0.3.0
