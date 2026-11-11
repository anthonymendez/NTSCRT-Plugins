# Golden images

`test_golden` (ctest label `golden`) compares rendered pixels against committed PNGs in two ways.
CI runs it in its own **Golden images** step on Windows and macOS with `NTSCRT_GOLDEN_STRICT=1`
(a missing reference fails) and `-V`, so every comparison's numbers are in the log. Failures
upload `golden-mismatch-<os>` (`<name>.png` = our render, `<name>.diff.png` = |difference| x 8).

## 1. Upstream references (`manifest.json`, `ref/`)

Renders by **upstream NTSCRT's own `crt-smoke`** (the macOS app's renderer) from
`.github/workflows/golden.yml`, compared on RGB with `ntscrt_core`'s render of the same setup:
does the port look like the app?

The workflow builds upstream at the submodule commit (librashader pinned, ntsc-rs, slang-shaders).
Upstream's `DigitalVideoRenderer.swift` doesn't type-check with the Swift compilers on GitHub's
runners (6.1 to 6.2.4: "unable to type-check this expression in reasonable time"), so after one
unpatched try the workflow applies `scripts/ci/patch_upstream_build.py` to the runner's working
tree (the per-pixel closure becomes a local `func`, same arithmetic; never committed).

Only setups where `crt-smoke` renders what the app renders are comparable, so the manifest sticks to:
- **chain input heights that divide the output height** (no ScanlineGrid supersampling, which the app
  and `ntscrt_core` do and `crt-smoke` doesn't);
- **app shader defaults**: `render_golden.py` passes upstream `AppState.appShaderDefaults`
  (glow presets: BOOST 1.1, GLOW_ROLLOFF 2.4, BLOOM_STRENGTH 0.1) as `--set`, like the app;
- **full ntsc-rs presets**, not `"default"` (pure ntsc-rs defaults in `crt-smoke`, the app's house
  defaults in `ntscrt_core`);
- **odd nearest-downscale ratios** (320 -> 107): at an exact even ratio every sample point is on a
  texel edge and D3D11 and Metal pick different neighbours;
- frame numbering: `crt-smoke --frame N` is `ntscrt` `frame_index` N - 1.

Not covered here (the app's other stages have no `crt-smoke` path): Glitch, Digital, Screen Loop,
crop, alpha. Those are in the self-references.

Tolerance: mean |difference| over RGB, 0..255 units, `default_threshold` 2.0 (3.0 with ntsc-rs).
Measured: 0.1 to 1.6 (Windows NVIDIA, WARP); the residue is GPU float math, the Lanczos stand-in
for vImage, and crt-sim's feedback on its first frame.

Refresh after an upstream sync or a manifest change (editing the manifest re-runs the workflow):
```
gh workflow run golden --ref <branch>          # or let the push trigger it
gh run download <run-id> -n golden-refs -D tests/golden/ref
```
Look at the images, then commit `tests/golden/ref` (keep `INDEX.txt` out: it's regenerated).

## 2. Self-references (`self/manifest.json`, `self/ref/`)

`ntscrt_core`'s own renders, committed as **regression baselines** for what the upstream
comparison can't reach: Digital (both codecs, a keyframe and a predicted frame), Glitch at several
frames, every Screen Loop preset, alpha Clip / Spill (straight and premultiplied), crop (with and
without alpha), a moving sequence through Medium VHS, `render_scale` 0.5, and every bundled look
preset at frame 12. Inputs are `TestAssets/game-frame.png` box-downscaled to 160x120 to keep the
PNGs small (about 2.5 MB in all).

Compared on RGBA, straight colour weighted by its alpha (what shows once composited). Tolerances
(per entry, or the manifest defaults): `threshold` 1.0 = mean |difference|, and `max_over16` 0.005
= share of pixels with a channel off by more than 16, which catches a local change (a missing
glitch band) that barely moves the mean. Measured against the Windows NVIDIA baselines: WARP
(Windows CI) mean <= 0.43, no pixel off by > 16; macOS CI (Metal, paravirtual GPU): see the CI log.
`preset-cd-rom-low-quality` has a wider tolerance: Video 1 after a nearest GPU downscale flips
block colours at edges when the GPU differs by a fraction of a pixel. An entry can carry per-OS
tolerances (`{"windows": a, "macos": b}`) or per-OS baselines (`self/ref-windows/`, `self/ref-macos/`,
which win over `self/ref/`).

**After an intended look change**, regenerate them (one command, from your build tree):
```
cmake --build <build> --config Release --target golden-update
```
Then check `git diff --stat tests/golden/self`, look at the changed images and commit them with the
change. `NTSCRT_GOLDEN_UPDATE=os` (environment, running `test_golden` yourself) writes the per-OS
directory instead; a CI mismatch artifact can also be dropped into it as is
(`gh run download <id> -n golden-mismatch-macos` then copy `self/*.png` minus `*.diff.png`).

On macOS GitHub runners only, a render that hangs the virtual GPU ("GPU Hang") is a warning and
the entry is skipped (counted in the summary line), as in `tests/crt`.

## Environment

| Variable | Effect |
|---|---|
| `NTSCRT_GOLDEN_STRICT=1` | a missing reference or input fails instead of skipping |
| `NTSCRT_GOLDEN_ONLY=<id>` | run one entry (either manifest) |
| `NTSCRT_GOLDEN_UPDATE=1` / `os` | write self-references instead of comparing |
| `NTSCRT_GOLDEN_REF_DIR` | upstream reference dir (default `tests/golden/ref`) |
| `NTSCRT_GOLDEN_OUT_DIR` | where mismatches are written |
| `NTSCRT_TESTASSETS_DIR` | input images (default `upstream/NTSCRT/TestAssets`) |
| `NTSCRT_D3D11_WARP=1` | Windows: render on WARP, like CI |

`ntscrt-imgdiff a.png b.png [--threshold T] [--diff out.png]` compares two PNGs the way the
upstream comparison does.
