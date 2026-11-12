# Testing NTSCRT on your computer

Thanks for trying NTSCRT. This page covers installing it, a 10-minute manual check in DaVinci Resolve
and/or Final Cut Pro, an automatic test run (Resolve Studio), and how to send a debug report if
anything misbehaves.

## 1. Install
Use the installer from the [latest release](https://github.com/anthonymendez/NTSCRT-Plugins/releases/latest)
(or the one-line install in the [README](../README.md#install)):
- **Windows:** `NTSCRT-Installer-windows.msi`. If SmartScreen warns, click *More info → Run anyway*
  (the installer isn't code-signed yet).
- **macOS:** `NTSCRT-Installer-macos.pkg`. If macOS blocks it, open *System Settings → Privacy & Security*
  and click *Open Anyway*. It installs the Resolve plug-in and/or the Final Cut plug-in, whichever app
  it finds.

Then restart Resolve / Final Cut.

**Testing a release candidate** (a `vX.Y.Z-rc.N` tag marked *Pre-release* on the
[releases page](https://github.com/anthonymendez/NTSCRT-Plugins/releases)): the installers and the plain
install line skip those, so pass the tag to the install line (here `v1.2.0-rc.1`; use the one you were asked
to test):
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.ps1))) -Version v1.2.0-rc.1
```
```bash
curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh -s -- --version v1.2.0-rc.1
```
The script checks the download against the release's `SHA256SUMS` and stops if it doesn't match or is
missing (only v0.3.0 and v0.4.0, which predate checksums, install without one, with a warning). Running the
install line again without `-Version` / `--version` goes back to the latest release.

## 2. Manual check (10 minutes)
**DaVinci Resolve** (free or Studio, 21.1.2 or newer):
1. Put a video clip on a timeline. *Effects → OpenFX → Filters → NTSCRT*: drag it onto the clip.
   The picture should get the VHS/CRT look within a second or two.
2. In the Inspector, *Presets → Version* shows the installed version. Pick a few presets
   (V Hold glitch, Webcam, …) and a few *CRT → Shader* choices; each should change the look.
   On a Mac, please include the **sim** shader: it's the heaviest and hasn't run on real
   Mac hardware yet (CI's virtual GPU times out on it).
3. **Animated preset:** choose *Glitch pop* in *Presets → Preset* and play: the glitch should repeat
   every 2 seconds (for up to a minute or two around the playhead on a long clip), and the NTSC and CRT
   controls it moves show keyframes. Then choose *Clean VHS*: the keyframes go away and the glitch stops
   repeating. Also on an **Adjustment Clip** over the clip, with the playhead on it (timelines start at
   01:00:00:00, so it is an hour in): choosing *5 second blips* should take well under a second, without
   freezing Resolve, and play right away where the playhead is.
4. **Digital video formats:** choose *Digital video/MiniDV* in the same menu: *Digital video* switches on and
   *Downscale → Width* becomes 720, the rest of the look stays. Then *Digital video/CD-ROM (Video 1)*: the
   Microsoft Video 1 codec (*Digital video → Compression → Codec: Block colors*): a 12 fps picture made of
   flat colour blocks, 240 pixels wide. The *CD-ROM (low quality)* preset is the full look built on it.
5. **Horizontal resolution:** *Downscale → Horizontal resolution → Arcade (384px)* sets the Width to 384;
   typing another Width switches it to *Custom*.
6. **Everything off:** untick *Crop*, *Downscale before shader*, *Digital video*, *NTSC signal*, *Glitch*,
   *CRT shader* and *Screen Loop*: the picture should be exactly the original clip.
7. Scrub around and play: no black frames, no error banner over the viewer. Jump far into the clip with
   Digital video on: the first frame there may take a moment, the next ones shouldn't.
8. Add a *Text+* title on a track above the clip and put NTSCRT on the title. *Transparency →
   Transparent areas*: **Clip to element** keeps the effect inside the letters; **Let effect spill**
   lets ghosting and glow spread over the video underneath.
9. **Float precision** (optional, Fusion page): add a *Background* tool with *Type: Horizontal* gradient from
   black to white, *Image → Depth: float32*, then NTSCRT (CRT shader *Glow (Gauss)*) and a *Brightness
   Contrast* tool with a high *Gain*. The glow should stay smooth; with *Depth: int8* on the Background it
   shows steps.
10. **Debug logging:** in *Presets*, tick **Debug logging**, play a few frames, untick it, then click
    **Open logs folder**: Explorer / Finder opens the folder with `ntscrt-ofx.log`, which has one detailed line
    per frame you played. Then click **Turn on debug logging for all effects**, play a few frames of another
    clip with NTSCRT (its own checkbox off), and click **Turn off debug logging for all effects**: those frames
    are in the log in detail too (`debug_by=all`).
11. Render a few seconds on the Deliver page and play the file back.

**Final Cut Pro** (12.4 or newer, macOS). Only if the release you installed includes the Final Cut plug-in.
The Final Cut plug-in hasn't been released yet: releases without it have no `NTSCRT-FxPlug-macos-universal.zip`
on their release page, and the installer says "skipping Final Cut Pro". Skip this part then.
1. Effects browser → *NTSCRT* → drag onto a clip. The look should appear in the viewer.
2. Change a few parameters and the preset (include an animated one such as *Glitch pop*, and a
   *Digital video/…* format); scrub; render/share a few seconds. Check the picture isn't upside down.
3. Put it on a title as in step 8 above.
4. *Debug Logging*, *Open Logs Folder* and *Debug Logging for All Effects…* as in step 10 (the log is
   `ntscrt-fxplug.log`, see below).

Note anything that looks wrong, along with what you did just before.

## 3. Automatic tests + debug report
You need Python 3. **Windows:** install it from python.org, or with `winget install Python.Python.3.12`.
**macOS:** run `python3 --version` in Terminal, and accept the offer to install the developer tools if it appears.

Download the doctor script and run it:

**macOS** (Terminal):
```bash
curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/scripts/ntscrt_doctor.py -o ntscrt_doctor.py
python3 ntscrt_doctor.py --resolve-tests
```
**Windows** (PowerShell):
```powershell
irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/scripts/ntscrt_doctor.py -OutFile ntscrt_doctor.py
python ntscrt_doctor.py --resolve-tests
```
- `--resolve-tests` runs about ten automatic checks inside DaVinci Resolve: alpha, the Edit page,
  frame order, every CRT shader against the reference renderer, bit depths, sizes, speed and debug logging
  (and, from v0.4.1 on, animated presets and 16-bit / float precision). It downloads the test suite of the
  version you have installed. This takes about 5 minutes (`--quick` renders fewer frames). **Quit Resolve
  first:** the doctor starts Resolve without a window (headless), runs the tests and quits it again (with
  Resolve open it tests in it instead: don't touch Resolve until it finishes).
  It needs **Resolve Studio** with *Preferences → System → General → External scripting using: Local*.
  On free Resolve, or for Final Cut only, leave `--resolve-tests` off.
- Either way it checks the install and collects logs into
  **`ntscrt-diagnostics-<date>.zip` on your Desktop**, and prints a short pass/fail summary.

The zip is plain text. It contains:
- system info: OS, CPU, GPU and driver;
- the NTSCRT install state;
- NTSCRT's own log;
- only the NTSCRT/OpenFX lines of Resolve's log (the full log lists your project names);
- crash reports from the last 7 days that mention Resolve, Final Cut or NTSCRT, trimmed to the crash itself
  (the error, the thread that crashed and the modules it ran through; Windows minidumps are listed, not included);
- Windows only: application errors mentioning Resolve or NTSCRT from the event log;
- installer logs;
- the test report.

Before zipping, the doctor removes personal details from every file: your home folder becomes `~`, your user
name `<user>` and your computer's name `<computer>`; on a Mac, serial numbers and hardware UUIDs are removed.
Lines that mention a project or file name containing "NTSCRT" are kept as they are.

Nothing is uploaded anywhere. Look through it if you like, then send it along with a description of
what went wrong.

### Debug logging (for a specific problem)
The plug-in always keeps a small log (start-up, the first frames of each effect, every error). To capture
**every** frame of the problem:
1. Select the effect. In its *Presets* group, tick **Debug logging**.
2. Reproduce the problem: play, scrub or render the frames that look wrong.
3. Untick **Debug logging** (it slows rendering a little). Resolve doesn't save it with the project.
4. Click **Open logs folder**: Explorer / Finder opens the folder with the log. Attach that file, or run the
   doctor above, which collects it.

**Debug logging for everything** (several effects, or a problem you can't pin to one clip): instead of ticking
each effect, turn it on once for the whole plug-in. Every NTSCRT effect, on every clip, in every project, then
logs every frame in detail, until you turn it off again (it stays on after restarting Resolve).
1. Select any NTSCRT effect. In its *Presets* group, click **Turn on debug logging for all effects**. The line
   above it, *Debug logging, all effects*, says **ON for every NTSCRT effect** (in Resolve the button's name
   and that line refresh when you reopen the effect's controls). Effects that are already open follow within
   a second.
2. Reproduce the problem.
3. Click the button again (**Turn off debug logging for all effects**): the line says *off*.
4. Click **Open logs folder** and attach `ntscrt-ofx.log`, or run the doctor, which also reports whether this
   switch is on.

In **Final Cut** the button is **Debug Logging for All Effects…**: it shows whether it is on and offers to turn
it on or off. The log is `ntscrt-fxplug.log`.

The switch is a file next to the log, `debug-logging-all-effects.txt`: logging for all effects is on while it
exists, so you can also delete it to turn it off. Setting the environment variable `NTSCRT_DEBUG_LOG=1` before
starting Resolve does the same for that session (see [ENVIRONMENT.md](ENVIRONMENT.md)).

**GPU or CPU path.** On Windows with an NVIDIA GPU, Resolve (in its default CUDA mode) hands the effect its
frames in GPU memory and they stay there; each logged frame says `path=gpu-direct`, `path=gpu-download`
(Digital video, Screen Loop or ntsc-rs need the picture on the CPU) or `path=gpu-cpuconvert`, and `path=cpu`
when Resolve handed over CPU images (AMD / Intel GPUs, OpenCL mode, macOS). If a problem might come from that,
start Resolve with `NTSCRT_OFX_GPU=0` (see [ENVIRONMENT.md](ENVIRONMENT.md)) and try again: the effect then always
gets CPU images, as before 0.5.0. Resolve's Fusion page only notices the change after the plug-in is reinstalled
or updated.

**NTSC on the GPU or ntsc-rs.** The log says once per effect where the NTSC signal runs (`NTSC route: gpu (...)`
or `NTSC route: cpu (why)`), and each logged frame `ntsc_route=gpu` / `ntsc_route=cpu`. If a problem might come
from the GPU NTSC signal, start Resolve with `NTSCRT_NTSC_GPU=0` (ntsc-rs, as 0.5.0-rc.4) and compare.

Where the logs are:
- Resolve, Windows: `%LOCALAPPDATA%\NTSCRT\ntscrt-ofx.log`
- Resolve, macOS: `~/Library/Logs/NTSCRT/ntscrt-ofx.log`
- Final Cut: `~/Library/Containers/<NTSCRT Renderer>/Data/Library/Logs/NTSCRT/ntscrt-fxplug.log` (Final Cut's
  plug-ins run sandboxed). On macOS 14 and later, the doctor may make Terminal ask to "access data from other
  apps": allow it, so it can collect this log.

## 4. Uninstall
- Windows: *Settings → Apps → NTSCRT plug-in installer → Uninstall*.
- macOS: `curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh -s -- --uninstall`
