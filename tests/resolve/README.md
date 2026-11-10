# Tests inside DaVinci Resolve

`run_resolve_tests.py` checks the OpenFX plug-in in a running Resolve: how Resolve actually calls
it (alpha, frame order, bit depths, sizes, proxy, the Edit page), and that it matches `ntscrt-cli`.
The ctest suites can't cover that. Run it before a release, and after changing anything in `ofx/` or
the alpha / frame handling in `core/`.

## Requirements
- DaVinci Resolve **Studio** with *Preferences → General → External scripting using: Local*.
- Python 3 (the system one is fine; the script only needs the standard library).
- The plug-in build to test, loaded by Resolve. Either install it, or, without administrator rights,
  quit Resolve and start it with the build on `OFX_PLUGIN_PATH`:
  ```powershell
  .\tests\resolve\launch_resolve_dev.ps1 -Bundle C:\ntb\main\ofx\Release\NTSCRT.ofx.bundle
  ```
  That also sets `NTSCRT_OFX_LOG`, where the plug-in writes why a render failed.

## Run
```bash
python tests/resolve/run_resolve_tests.py --build C:/ntb/main            # all tests, ~5 min
python tests/resolve/run_resolve_tests.py --build C:/ntb/main --quick --only alpha,order
```
`--build` points at the CMake build dir (for `ntscrt-cli` and the bundle's shaders). Results land in
`--out` (default: a new folder under the temp dir): `report.md`, `report.json`, and the rendered
frames plus amplified diff images for anything that failed. Exit code 0 = no failures.

The tests run in their own project, **NTSCRT Resolve Tests**, recreated on every run. The project that
was open is saved first and reopened at the end. While the tests run, Fusion is in batch mode (no
dialogs); don't use Resolve until it finishes.

## Tests
| Test | Checks | Pass when |
|---|---|---|
| version | the plug-in Resolve loaded | its Version field equals `VERSION` |
| alpha | Text+ → NTSCRT in Fusion, both Transparent areas modes | Clip: alpha = source alpha; Spill: alpha never below the source and some light outside the title; the title itself changed |
| edit | NTSCRT on a Text+ title on the Edit page (default Clip) | the empty area around the title is unchanged; the title changed |
| order | default look, Digital codec on, Screen Loop on | a fresh instance rendering frames out of order matches sequential frames (mean diff ≤ 0.05) |
| cli | default look on 8 frames, then every CRT preset | plug-in vs `ntscrt-cli` mean diff ≤ 2 (of 255) |
| depth | 8-bit int, 16-bit int, 16-bit float sources | each within mean 1.5 of the 32-bit float result |
| sizes | 1279×719, 1080×1920, 320×240 | output has the source size and isn't black |
| proxy | Fusion half-resolution proxy render | mean diff ≤ 12 against the full render scaled down (skipped if Fusion ignores proxy) |
| speed | 1920×1080, every CRT preset | informational: ms per frame NTSCRT adds |

## When something fails
Open the diff image named in the report (differences ×8). For render errors, read the
`NTSCRT_OFX_LOG` file and Resolve's log (`%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\davinci_resolve.log`,
search for `ntscrt`). If every test after the first few fails with "Resolve kept refusing", a dialog
is open in Resolve: close it and rerun.
