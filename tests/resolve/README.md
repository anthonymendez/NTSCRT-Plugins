# Tests inside DaVinci Resolve

`run_resolve_tests.py` checks the OpenFX plug-in in a running Resolve, unattended: how Resolve actually calls
it (alpha, frame order, bit depths, sizes, the Edit page), and that it matches `ntscrt-cli`.
The ctest suites can't cover that. Run it before a release, and after changing anything in `ofx/` or
the alpha / frame handling in `core/`.

## Requirements
- DaVinci Resolve **Studio** with *Preferences → General → External scripting using: Local*.
- Python 3 (the system one is fine; the script only needs the standard library).
- The plug-in build to test, loaded by Resolve. Either install it, or, without administrator rights,
  quit Resolve and start it with the build on `OFX_PLUGIN_PATH`:
  ```powershell
  .\tests\resolve\launch_resolve_dev.ps1 -Bundle <build>\ofx\Release\NTSCRT.ofx.bundle
  ```
  (`<build>` is your CMake build directory, the one you passed to `cmake -B`.)
  That also sets `NTSCRT_OFX_LOG`, where the plug-in writes why a render failed, and
  `NTSCRT_OFX_DEV_BUNDLE`, which makes the copy outrank an installed NTSCRT of the same version
  (Resolve keeps only one plug-in per id and version; without it the installed one wins).

## Run
```bash
# Recommended: Resolve closed; the suite starts it headless (-nogui), tests this build, quits it.
python tests/resolve/run_resolve_tests.py --headless --bundle <build>/ofx/Release/NTSCRT.ofx.bundle --build <build>
# In an open Resolve (tests whatever plug-in it loaded; don't touch Resolve meanwhile):
python tests/resolve/run_resolve_tests.py --build <build> --quick --only alpha,order
```
`--headless` is the unattended mode: with no UI there's no dialog that could block the run. Testers
without a build use `scripts/ntscrt_doctor.py --resolve-tests` (see docs/TESTING.md), which fetches this
suite and runs it headless against the installed plug-in.
`--build` points at the CMake build dir (for `ntscrt-cli` and the bundle's shaders). Results land in
`--out` (default: a new folder under the temp dir): `report.md`, `report.json`, and the rendered
frames plus amplified diff images for anything that failed. Exit code 0 = no failures.

The tests run in their own project, **NTSCRT Resolve Tests** (reused when it's open, each run adding
timelines stamped with the time; recreated otherwise). The project that was open is saved first and
reopened at the end. Don't use Resolve until it finishes.

How frames are captured: each test builds a Fusion comp in a timeline clip, routes the tool under test
to MediaOut and renders the frames through the **render queue** (Deliver) as PNGs. Fusion Savers aren't
used because a scripted Fusion render opens a modal "Render completed" box that blocks scripting. Alpha is
recovered from a render over black and one over white. All timelines stay at 640×360: changing a
timeline's resolution from a script crashed Resolve 21.1.2, so other sizes are made inside Fusion.

## Tests
| Test | Checks | Pass when |
|---|---|---|
| version | the plug-in Resolve loaded | its Version field equals `VERSION` |
| alpha | Text+ → NTSCRT in Fusion, both Transparent areas modes | Clip: alpha = source alpha; Spill: alpha never below the source and some light outside the title; the title itself changed |
| edit | NTSCRT on a Text+ title on the Edit page (default Clip) | the empty area around the title is unchanged; the title changed |
| order | default look, Digital codec on, Screen Loop on | a fresh instance rendering frames out of order matches sequential frames (mean diff ≤ 0.05) |
| cli | default look on 8 frames, then every CRT preset | plug-in vs `ntscrt-cli` mean diff ≤ 2 (of 255) |
| animated | *Glitch pop* chosen from the Preset menu, 40 frames; Horizontal resolution *Arcade*; the *Digital video/MiniDV* entry | the keyed values move over time; plug-in vs `ntscrt-cli --timeline` mean diff ≤ 2, and some frame differs from the frozen look by > 4; Width becomes 384; MiniDV turns Digital video on with Width 720 |
| depth | 8-bit int, 16-bit int, 16-bit float sources | each within mean 1.5 of the 32-bit float result |
| precision | a black-to-white ramp through Glow (Gauss) at 320 px wide, delivered as a 16-bit PNG from float32, float16 and int8 projects | int8: every value an 8-bit level (k*257); float32 / float16: at least half the values between 8-bit levels and 3x the distinct levels |
| sizes | 1279×719, 1080×1920, 321×241 sources inside Fusion | renders, not black, lines up with the bypassed source |
| speed | 1920×1080, every CRT preset | informational: ms per frame NTSCRT adds |
| debug | *Debug logging* on for a few frames, then off; *Open logs folder* pressed (`SetInput`) | one detailed log line (timings, look) per frame, none after turning it off, the picture unchanged; the button press is logged and, headless, opens no window; Resolve still answers |

## When something fails
Open the diff image named in the report (differences ×8). For render errors, read the
`NTSCRT_OFX_LOG` file and Resolve's log (`%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\logs\davinci_resolve.log`,
search for `ntscrt`). If every test after the first few fails with "Resolve kept refusing", a dialog
is open in Resolve: close it and rerun.
