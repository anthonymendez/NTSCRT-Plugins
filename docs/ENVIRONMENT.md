# Environment variables

Every environment variable the plug-ins, installers, scripts and tests read. None is needed for
normal use: they are for troubleshooting, testing and development. Values are read once, when the
plug-in loads (or the script starts), so restart DaVinci Resolve after changing one.

Setting one for Resolve:
- **Windows:** *Settings → System → About → Advanced system settings → Environment Variables → New*
  (user variables), or `setx NTSCRT_SOURCE_CACHE_MB 1024` in a terminal. Then quit and restart Resolve.
- **macOS:** `launchctl setenv NTSCRT_SOURCE_CACHE_MB 1024` in Terminal (lasts until you log out), then
  quit and restart Resolve. `launchctl unsetenv NAME` removes it.

## Plug-in (DaVinci Resolve / OpenFX)

| Variable | Values | What it does |
|---|---|---|
| `NTSCRT_OFX_LOG` | a file path, or `off` | Where the plug-in's log goes, or `off` for no log. Default: `%LOCALAPPDATA%\NTSCRT\ntscrt-ofx.log` (Windows), `~/Library/Logs/NTSCRT/ntscrt-ofx.log` (macOS); rotated to `ntscrt-ofx.old.log` at 1 MB. |
| `NTSCRT_OFX_LOG_VERBOSE` | `1` | Log every render of every NTSCRT effect (normally only the first 20 per effect, every 500th and failures). The *Debug logging* checkbox does this for one effect, with timings and the look. |
| `NTSCRT_OFX_NO_OPEN` | `1` | *Open logs folder* only logs the press and opens no window (tests, CI). Headless Resolve (`-nogui`) behaves like this anyway. |
| `NTSCRT_OFX_DEV_BUNDLE` | a folder | A plug-in bundle inside this folder reports itself as a development build that outranks any installed NTSCRT of the same version (Resolve keeps one plug-in per id and version). Set by `tests/resolve/run_resolve_tests.py --bundle` and `launch_resolve_dev.ps1`. |

## Core (both plug-ins and `ntscrt-cli`)

| Variable | Values | What it does |
|---|---|---|
| `NTSCRT_SOURCE_CACHE_MB` | megabytes (default `512`) | Memory budget of each effect's cache of source frames (the earlier frames Digital video and Screen Loop re-read). The least recently used frames are dropped beyond it; at least 8 frames are kept whatever the budget, and never more than 65. 512 MB is about 60 frames at 1080p and 15 at UHD. |
| `NTSCRT_SCREENLOOP_CHECKPOINT_MB` | megabytes (default `256`), `0` = none | GPU memory each effect may keep for Screen Loop checkpoints: snapshots of the loop every 30 frames (and at each window start) that a jump back resumes from instead of re-running the loop from its window start. About 15-22 MB each at the default Downscale width (a 1280x720 TV picture per frame of Delay); the least recently used go first. |
| `NTSCRT_OUTPUT_8BIT` | `1` | 16-bit and float projects get the CRT shader's result at 8 bits, as 0.4.0 did, instead of half-float. |
| `NTSCRT_SHARED_DEVICE` | `0` | Each effect creates its own GPU device and compiled kernels instead of sharing one per process. |
| `NTSCRT_SHARED_CHAINS` | `0` | Each effect compiles its own CRT shader chain instead of sharing one per GPU device and preset (Windows; on macOS chains are never shared). |
| `NTSCRT_LIBRASHADER` | a file path | librashader library to try before the one in the bundle. |
| `NTSCRT_D3D11_WARP` | `1` | Windows: use Microsoft's software renderer (WARP) instead of the GPU. |
| `NTSCRT_D3D11_NO_FENCE` | set (any value) | Windows: wait for the GPU with a query instead of a Direct3D 11.4 fence. |
| `NTSCRT_D3D11_NO_BGRA_UAV` | set (any value) | Windows, testing: act as if the GPU can't write BGRA textures from compute shaders. |

## Final Cut Pro plug-in (not released)

| Variable | Values | What it does |
|---|---|---|
| `NTSCRT_FXPLUG_FLIP` | `0` or `1` | Overrides the row order the renderer assumes (`1` = bottom row first). For testing image orientation (fxplug/README.md); set with `launchctl setenv`. |

## Installers

| Variable | Read by | What it does |
|---|---|---|
| `NTSCRT_VERSION` | Windows MSI (property), macOS .pkg | Release tag to install instead of the latest: `latest` or `vX.Y.Z[-rc.N]` (`msiexec /i NTSCRT-Installer-windows.msi NTSCRT_VERSION=v0.5.0-rc.1`). The scripts take `-Version` / `--version`. |
| `NTSCRT_FROM` | Windows MSI (property), macOS .pkg | A local release zip (or bundle folder) to install instead of downloading; skips the `SHA256SUMS` check. The scripts take `-From` / `--from`. |
| `NTSCRT_REPO` | `install.ps1`, `install.sh`, `ntscrt_doctor.py` | GitHub `owner/name` to download from (forks). |

## Scripts and tests

| Variable | Read by | What it does |
|---|---|---|
| `NTSCRT_BUILD_DIR` | `scripts/make_previews.ps1`, `make_previews_resolve.py`, `make_presets_gallery.py` | Default CMake build dir (`-Build` / `--build`). |
| `NTSCRT_REQUIRE_GPU` | GPU, pixel-format and golden tests | `1`: a missing GPU device fails the test instead of skipping it (CI sets it). |
| `NTSCRT_GOLDEN_STRICT` | `test_golden` | `1`: a missing reference image fails instead of being skipped (CI sets it). |
| `NTSCRT_GOLDEN_UPDATE` | `test_golden` (self references) | `1` rewrites the shared baselines, `os` the per-OS ones (the `golden-update` target runs it with `1`). |
| `NTSCRT_GOLDEN_ONLY` | `test_golden` | Run only the entry with this id. |
| `NTSCRT_GOLDEN_OUT_DIR` | `test_golden` | Where the actual and diff images of failures go (default `golden-out`). |
| `NTSCRT_GOLDEN_REF_DIR` | `test_golden` | Upstream reference folder (default `tests/golden/ref`). |
| `NTSCRT_RESOURCE_DIR` | `test_golden` | Core resource folder (shaders, presets). |
| `NTSCRT_TESTASSETS_DIR` | `test_golden` | Upstream test images (default `upstream/NTSCRT/TestAssets`). |
| `NTSCRT_TEST_EXHAUSTIVE` | `test_convert_threads` | `1`: check the rounding of every float in 0…1 instead of a sample. |
| `NTSCRT_TEST_OFX_BINARY` | `test_ofx` | Run the mock-host tests against another build of `NTSCRT.ofx` (timings before / after). |
| `RESOLVE_SCRIPT_API`, `RESOLVE_SCRIPT_LIB` | `tests/resolve/run_resolve_tests.py` | Resolve's scripting folder and `fusionscript` library, when not in the default place. |
| `OFX_PLUGIN_PATH` | OpenFX hosts | Extra plug-in folders (the standard OpenFX variable; the dev launchers use it). The doctor reports it when set. |
