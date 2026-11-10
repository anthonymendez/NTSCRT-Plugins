# Testing NTSCRT on your computer

Thanks for trying NTSCRT. This page covers installing it, a 5-minute manual check in DaVinci Resolve
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

## 2. Manual check (5 minutes)
**DaVinci Resolve** (free or Studio, 21.1.2 or newer):
1. Put a video clip on a timeline. *Effects → OpenFX → Filters → NTSCRT*: drag it onto the clip.
   The picture should get the VHS/CRT look within a second or two.
2. In the Inspector, *Presets → Version* shows the installed version. Pick a few presets
   (V Hold glitch, Webcam, …) and a few *CRT → Preset* shaders; each should change the look.
   On a Mac, please include the **sim** CRT preset: it's the heaviest shader and hasn't run on real
   Mac hardware yet (CI's virtual GPU times out on it).
3. Scrub around and play: no black frames, no error banner over the viewer.
4. Add a *Text+* title on a track above the clip and put NTSCRT on the title. *Transparency →
   Transparent areas*: **Clip to element** keeps the effect inside the letters; **Let effect spill**
   lets ghosting and glow spread over the video underneath.
5. Render a few seconds on the Deliver page and play the file back.

**Final Cut Pro** (12.4 or newer, macOS):
1. Effects browser → *NTSCRT* → drag onto a clip. The look should appear in the viewer.
2. Change a few parameters and the preset; scrub; render/share a few seconds.
3. Put it on a title as in step 4 above.

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
- `--resolve-tests` runs about 10 automatic checks inside DaVinci Resolve: alpha, the Edit page,
  frame order, every CRT preset, bit depths, sizes and speed. This takes about 5 minutes. **Quit Resolve
  first:** the doctor starts Resolve without a window (headless), runs the tests and quits it again.
  It needs **Resolve Studio** with *Preferences → System → General → External scripting using: Local*.
  On free Resolve, or for Final Cut only, leave `--resolve-tests` off.
- Either way it checks the install and collects logs into
  **`ntscrt-diagnostics-<date>.zip` on your Desktop**, and prints a short pass/fail summary.

The zip is plain text. It contains:
- system info: OS, CPU, GPU and driver;
- the NTSCRT install state;
- NTSCRT's own log;
- only the NTSCRT/OpenFX lines of Resolve's log (the full log lists your project names);
- crash reports from the last 7 days that mention Resolve, Final Cut or NTSCRT;
- installer logs;
- the test report.

Nothing is uploaded anywhere. Look through it if you like, then send it along with a description of
what went wrong.

The plug-in's own log, if you want to look:
- Windows: `%LOCALAPPDATA%\NTSCRT\ntscrt-ofx.log`
- macOS: `~/Library/Logs/NTSCRT/ntscrt-ofx.log`

## 4. Uninstall
- Windows: *Settings → Apps → NTSCRT plug-in installer → Uninstall*.
- macOS: `curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh -s -- --uninstall`
