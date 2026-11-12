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
| `NTSCRT_OFX_LOG_VERBOSE` | `1` | Log every render of every NTSCRT effect (normally only the first 20 per effect, every 500th and failures), one short line each. The *Debug logging* checkbox does this for one effect, with timings and the look; `NTSCRT_DEBUG_LOG` for all of them. |
| `NTSCRT_DEBUG_LOG` | `1` (`0` / `off` = not set) | Detailed debug logging (every frame, with timings and the look, as the *Debug logging* checkbox) for every NTSCRT effect while Resolve runs with it, whatever the effects' checkboxes say. The same as the *Turn on debug logging for all effects* button, without the button's file. Read by the Final Cut plug-in too, but Final Cut's sandboxed renderer doesn't normally see your environment: use the button there. |
| `NTSCRT_SETTINGS_DIR` | a folder | Where both plug-ins keep the *Debug logging for all effects* switch, the file `debug-logging-all-effects.txt` (on while it exists). Default: the log's default folder, `%LOCALAPPDATA%\NTSCRT` (Windows), `~/Library/Logs/NTSCRT` (macOS), and for Final Cut `~/Library/Containers/<NTSCRT Renderer>/Data/Library/Logs/NTSCRT`. Not moved by `NTSCRT_OFX_LOG`. Tests set it so they never touch your own switch (`test_ofx`, `run_resolve_tests.py --headless`). |
| `NTSCRT_OFX_NO_OPEN` | `1` | *Open logs folder* only logs the press and opens no window (tests, CI). Headless Resolve (`-nogui`) behaves like this anyway. |
| `NTSCRT_OFX_TEST_PRESET_FILE` | a file path | Testing only, development copies only (`NTSCRT_OFX_DEV_BUNDLE`): while this file exists, each new effect whose Preset menu is *(none)* gets the bundled look its first line names, as if chosen with the playhead at the frame on its second line (third line `guards=1`: with guard keys). Set by `run_resolve_tests.py --headless`; its `position` test writes the file, as scripts can't set OpenFX params on Resolve's Edit page. |
| `NTSCRT_OFX_GPU` | `0` (or `off`), `1` | `0`: don't offer GPU render to Resolve, so it hands the effect CPU images as before 0.5.0 (the CPU path; for A/B tests or a driver problem). Unset: CUDA is offered on Windows with an NVIDIA driver (Resolve's CUDA mode), frames then stay on the GPU ([docs/dev/HOST-GPU.md](dev/HOST-GPU.md)). `1`: also offer Metal on macOS (off by default until it has run on a Mac). Resolve's Fusion page keeps the choice it saw until the plug-in file changes. |
| `NTSCRT_OFX_DEV_BUNDLE` | a folder | A plug-in bundle inside this folder reports itself as a development build that outranks any installed NTSCRT of the same version (Resolve keeps one plug-in per id and version). Set by `tests/resolve/run_resolve_tests.py --bundle` and `launch_resolve_dev.ps1`. |

## Core (both plug-ins and `ntscrt-cli`)

| Variable | Values | What it does |
|---|---|---|
| `NTSCRT_SOURCE_CACHE_MB` | megabytes (default `512`) | Memory budget of each effect's cache of source frames (the earlier frames Digital video and Screen Loop re-read). The least recently used frames are dropped beyond it; at least 8 frames are kept whatever the budget, and never more than 65. 512 MB is about 60 frames at 1080p and 15 at UHD. |
| `NTSCRT_SCREENLOOP_CHECKPOINT_MB` | megabytes (default `256`), `0` = none | GPU memory each effect may keep for Screen Loop checkpoints: snapshots of the loop every 30 frames (and at each window start) that a jump back resumes from instead of re-running the loop from its window start. About 15-22 MB each at the default Downscale width (a 1280x720 TV picture per frame of Delay); the least recently used go first. |
| `NTSCRT_OUTPUT_8BIT` | `1` | 16-bit and float projects get the CRT shader's result at 8 bits, as 0.4.0 did, instead of half-float. |
| `NTSCRT_FAST_TRANSFER` | `0` | The 0.5.0-rc.3 way of moving pictures to and from the GPU, for A/B timing (the picture is the same either way): the host picture is converted into a new CPU buffer each frame and uploaded from there, the result is read back into a buffer and converted from it, with the plain conversion loops (no SIMD, no streaming stores). On the GPU path (`NTSCRT_OFX_GPU`) it only changes how the 8-bit picture for ntsc-rs and a transparent frame in *Clip* mode come down (one copy instead of banded reads). Independent of `NTSCRT_OFX_GPU`. |
| `NTSCRT_READBACK_BANDS` | a number | Bands of rows the result is read back in: the transfer of the next band overlaps the conversion of this one. Default one per ~16 MB, at most 8 (4 for a UHD float frame); `1` = no overlap. |
| `NTSCRT_SHARED_DEVICE` | `0` | Each effect creates its own GPU device and compiled kernels instead of sharing one per process. |
| `NTSCRT_SHARED_CHAINS` | `0` | Each effect compiles its own CRT shader chain instead of sharing one per GPU device and preset (Windows; on macOS chains are never shared). |
| `NTSCRT_LIBRASHADER` | a file path | librashader library to try before the one in the bundle. |
| `NTSCRT_D3D11_WARP` | `1` | Windows: use Microsoft's software renderer (WARP) instead of the GPU. |
| `NTSCRT_D3D11_NO_FENCE` | set (any value) | Windows: wait for the GPU with a query instead of a Direct3D 11.4 fence. |
| `NTSCRT_PROFILE` | `1` | Waits for the GPU after each GPU stage so the per-render timings (`in= digital= ntsc= upload= downscale= glitch= crt= loop= readback= out=` on the log line) show each stage's GPU time instead of where the work is waited for. Slows rendering: profiling only. |
| `NTSCRT_D3D11_NO_BGRA_UAV` | set (any value) | Windows, testing: act as if the GPU can't write BGRA textures from compute shaders. |

## Final Cut Pro plug-in (not released)

| Variable | Values | What it does |
|---|---|---|
| `NTSCRT_FXPLUG_FLIP` | `0` or `1` | Overrides the row order the renderer assumes (`1` = bottom row first). For testing image orientation (fxplug/README.md); set with `launchctl setenv`. |
| `NTSCRT_DEBUG_LOG`, `NTSCRT_SETTINGS_DIR` | as above | As for the OpenFX plug-in, if the renderer sees them (the sandbox usually hides your environment). |

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
