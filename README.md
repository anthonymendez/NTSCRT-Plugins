# NTSCRT Plugins

[NTSCRT](https://github.com/finnmckenty/NTSCRT)'s analog TV look (ntsc-rs signal emulation, TV/VCR glitch
circuits, RetroArch CRT shaders via librashader, digital codec, Screen Loop) as video-editor effects:

- **DaVinci Resolve 21.1.2+** — OpenFX plugin, Windows and macOS. Released.
- **Final Cut Pro 12.4+** — FxPlug 4 plugin, macOS. **Not released yet:** the code is in `fxplug/`, but it
  hasn't been built or tested in Final Cut, and releases don't include it. The Final Cut parts of this page
  apply once a release does.

Free and open source under the [MIT licence](#licence). Synced with upstream NTSCRT v0.20.0.

![Animated before/after: a scrolling synthwave test scene untouched on the left, through NTSCRT's default look on the right](docs/images/hero-before-after.webp)
*Before / after with the default look, in motion: chroma bleed, moving noise, scanlines and CRT glow.*

![Four animated NTSCRT looks: V Hold glitch, Obliterated, Webcam (low quality) and a close-up of the CRT Royale shader](docs/images/looks-grid.webp)
*A few of the bundled looks and CRT shaders. Both animations are rendered by the Resolve plug-in itself
(DaVinci Resolve running headless) with `scripts/make_previews_resolve.py`; `scripts/make_previews.ps1`
makes the same animations through `ntscrt-cli` when Resolve isn't available.*

## Install

| App | Supported |
|---|---|
| DaVinci Resolve (Windows) | 21.1.2 and later (free or Studio), x64 |
| DaVinci Resolve (macOS) | 21.1.2 and later (free or Studio), Apple silicon + Intel |
| Final Cut Pro (macOS) | not released yet (planned: 12.4 and later) |

### Requirements

- **Windows:** Windows 10 or 11 (tested on 11), x64, with a Direct3D 11 GPU (feature level 11.0; any GPU from the last
  ten years). Without one the plug-in falls back to Windows' software renderer (WARP), which works but is slow.
- **macOS:** macOS 12 Monterey or later, Apple silicon or Intel, with a Metal GPU (every Mac that runs macOS 12).
- **DaVinci Resolve 21.1.2 or later.** Older versions aren't tested.
- The installers need an internet connection (they download the latest release from GitHub).

**Quit DaVinci Resolve / Final Cut Pro first.**

### Download the installer

From the [latest release](https://github.com/anthonymendez/NTSCRT-Plugins/releases/latest):

- **Windows:** `NTSCRT-Installer-windows.msi` (DaVinci Resolve)
- **macOS:** `NTSCRT-Installer-macos.pkg` (DaVinci Resolve; Final Cut Pro too once a release includes it)

Double-click it and follow the steps. The installer is small: when it runs it downloads the **latest release**
from GitHub and installs that, so an old installer still gets you the newest plug-in (it needs an internet
connection).

The installers aren't signed yet, so the first run needs one extra click:

- **Windows:** if SmartScreen says "Windows protected your PC", click **More info → Run anyway**. Then accept the
  admin prompt.
- **macOS:** if it says the package "can't be opened" / is from an unidentified developer, open
  *System Settings → Privacy & Security*, scroll down and click **Open Anyway** next to the NTSCRT message, then
  open the package again. Enter your password when asked.

**Updating:** run the installer again, or on Windows use **Update NTSCRT plug-in** in the Start menu. Both
install the latest release.
**Uninstalling:** Windows: remove *NTSCRT plug-in installer* in *Settings → Apps* (this removes the plug-in too).
macOS: the `--uninstall` line below.

### Or paste one line

**Windows** (PowerShell): installs the DaVinci Resolve plugin
```powershell
irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.ps1 | iex
```

**macOS** (Terminal): installs for DaVinci Resolve (and Final Cut Pro once a release includes it)
```bash
curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh
```

Accept the admin prompt (Windows) or enter your password (macOS), then start the app:

- **Resolve**: *Effects → OpenFX → Filters → NTSCRT*. Drag it onto a clip, or into a node on the Color page.
- **Final Cut Pro**: *Effects browser → NTSCRT*. Drag it onto a clip, then adjust it in the Video Inspector.
  Keep `/Applications/NTSCRT.app`: the plug-in lives inside it.

The scripts download the latest release, install it, and clear the macOS download quarantine. Re-run the
same line to update. You can read them first: [install.ps1](install.ps1), [install.sh](install.sh).

**Options**

| | Windows (`install.ps1`) | macOS (`install.sh`) |
|---|---|---|
| Uninstall | `-Uninstall` | `--uninstall` |
| Specific release | `-Version v0.3.0` | `--version v0.3.0` |
| Only one app | (Resolve only on Windows) | `--resolve` or `--fcp` |
| Install a zip you downloaded | `-From path\to.zip` | `--from path/to.zip` |

Passing options to a one-liner:
```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.ps1))) -Uninstall
```
```bash
curl -fsSL https://raw.githubusercontent.com/anthonymendez/NTSCRT-Plugins/main/install.sh | sh -s -- --fcp
```

<details><summary>Manual install from the zip (no scripts)</summary>

Download from the repo's **Releases** page: `NTSCRT-OFX-windows-x64.zip`, `NTSCRT-OFX-macos-universal.zip`
or `NTSCRT-FxPlug-macos-universal.zip`.

- **Resolve on Windows:** unzip, then double-click **`Install NTSCRT.cmd`**, or copy the `NTSCRT.ofx.bundle`
  folder into `C:\Program Files\Common Files\OFX\Plugins\` yourself.
- **Resolve on macOS:** unzip, copy `NTSCRT.ofx.bundle` into `/Library/OFX/Plugins/`, then run
  `sudo xattr -dr com.apple.quarantine /Library/OFX/Plugins/NTSCRT.ofx.bundle`.
- **Final Cut Pro:** unzip, drag `NTSCRT.app` into Applications and open it once. It registers the plug-in,
  installs the effect template, then quits. If macOS says it "can't be opened" (builds aren't notarized
  yet), click **Open Anyway** in *System Settings → Privacy & Security* and open it again.
- **Uninstall by hand:** delete the `NTSCRT.ofx.bundle` you copied. For Final Cut Pro, delete
  `/Applications/NTSCRT.app` and `~/Movies/Motion Templates.localized/Effects.localized/NTSCRT`.
</details>

### Updating
Run the installer or the same install line again (on Windows, also **Update NTSCRT plug-in** in the Start
menu): it installs the latest release over the old one and prints both versions.
Your projects keep their effects and settings. See what changed in [CHANGELOG.md](CHANGELOG.md). In Resolve, the
installed version shows in the effect's **Presets → Version** field. A specific version: `-Version v0.3.0`
(Windows) or `--version v0.3.0` (macOS).

### Using it

- **Preset** menu: pick a look (Clean VHS, Medium VHS, V Hold glitch, Webcam…). It sets every control (and
  replaces their keyframes), and you can then tweak and keyframe any of them. Animated looks (*Glitch pop*,
  *15 second glitches*, …) arrive as keyframes that play the NTSCRT app's animation from the clip's first frame,
  looping until its end. The **Digital video/…** entries (Webcam (2005), MiniDV, CD-ROM…) are upstream's formats:
  they switch Digital video on and set its controls and the Downscale width, leaving the rest of the look alone.
- **Screen Loop preset**: video feedback (a camcorder filming its own TV).
- **Load preset… / Save preset…**: preset files are the same JSON as the NTSCRT app, and the NTSC section is
  compatible with ntsc-rs presets. Saved files also carry the Screen Loop settings (and, while Screen Loop is
  on, a second file `<name> (Screen Loop).json` for the NTSCRT app).
- Apply it to the clip at its **native resolution**. On a low-res clip in a high-res timeline, the analog artifacts get
  scaled up with the picture.
- **Titles and clips that don't fill the frame**: *Transparency → Transparent areas* picks how the effect treats
  the empty parts. **Clip to element** (default) keeps the effect inside the element's shape with clean
  edges. **Let effect spill** lets ghosting, smear and glow extend past it, shown as light over the video
  underneath (like a Screen blend). Presets don't change this setting.
- Screen Loop and the Digital-video codec depend on earlier frames, so the first render after jumping far
  into a clip can take a moment.

### Troubleshooting

Testing it for someone, or something's wrong? [docs/TESTING.md](docs/TESTING.md) has a 5-minute check and a
`ntscrt_doctor.py` command that collects a debug report (logs, install state, optional automatic tests).

| Problem | Fix |
|---|---|
| Installer says "Couldn't replace the installed plugin" | A file in the plugin is in use. Quit Resolve completely (check the system tray and Task Manager) and run it again. The installed version is left untouched. Installing with Resolve open normally works; the new version loads on the next restart. |
| Folders named `NTSCRT.ofx.bundle.old-…` next to the plugin | The previous version, kept while Resolve still had it loaded. Resolve ignores them, and the next install or uninstall deletes them. |
| Effect doesn't show up in Resolve | Restart Resolve fully. Check that the bundle is in the OFX folder above and that its whole folder was copied (`Contents\Win64\NTSCRT.ofx` on Windows, `Contents/MacOS/NTSCRT.ofx` on macOS). |
| Resolve skips the plugin at startup | Resolve marks plugins that crashed as disabled. Re-enable it in *Preferences → Video Plugins* and send us the log. |
| macOS: "NTSCRT.ofx is damaged / can't be verified" | Run the install line again: it clears the quarantine. For a manual copy, run the `xattr` command above. |
| Final Cut: effect missing | Open `/Applications/NTSCRT.app` once more and restart Final Cut. In Terminal, `pluginkit -mAvvv -p FxPlug` should list `NTSCRT Renderer`. |
| Need a log for a bug report | In the effect's *Presets* group, tick **Debug logging**, reproduce the problem (play or render those frames), untick it, then click **Open logs folder** and attach `ntscrt-ofx.log` (Resolve) or `ntscrt-fxplug.log` (Final Cut). Or run `ntscrt_doctor.py` ([docs/TESTING.md](docs/TESTING.md)), which collects it for you. |
| Picture is black or unchanged | Usually a partial copy: the `shaders/` folder or `librashader` file inside the bundle is missing. Run the install line again. Resolve shows the plugin's error message when it can't render. |

Building from source instead: see [Build from source](#build-from-source) below, and [fxplug/README.md](fxplug/README.md) for Final Cut.

## Known issues and limitations

- **Animated presets are keyframed, not linked.** Choosing *Glitch pop*, *15 second glitches* and the other
  animated looks writes their animation as keyframes (one per frame on each moving control), repeated from the
  clip's first frame to its end (at most 3600 frames; the last values hold after that). Changing the clip's
  length or frame rate later doesn't re-time them: choose the preset again. **Save preset…** saves the look
  at the playhead, not the animation.
- **Edits and seeks can be slow** with the Glitch, Digital video or Screen Loop stages on: they depend on
  earlier frames, so the plug-in re-renders those after a jump or a parameter change.
- **Memory:** the plug-in keeps recent source frames per effect instance, which adds up on 4K timelines with
  many instances.
- **8-bit processing:** frames are processed at 8 bits per channel, so 16-bit / float timelines can show
  banding in smooth gradients after heavy grading on top of the effect.
- **Installers aren't code-signed or notarized** yet (see the extra click under [Install](#install)).
- **macOS:** the CI runs on a virtual GPU, so the heaviest CRT shader (*sim*) hasn't been run on real Mac
  hardware yet. Reports welcome ([docs/TESTING.md](docs/TESTING.md)).
- **Final Cut Pro:** not released (see the top of this page).

Bugs and requests: [open an issue](https://github.com/anthonymendez/NTSCRT-Plugins/issues/new/choose); the bug
form asks for the `ntscrt_doctor.py` report.

## Credits
All of the look comes from [NTSCRT](https://github.com/finnmckenty/NTSCRT) by Finn McKenty (MIT),
[ntsc-rs](https://github.com/ntsc-rs/ntsc-rs) (MIT/ISC/Apache-2.0),
[librashader](https://github.com/SnowflakePowered/librashader) (MPL-2.0) and the
[libretro slang-shaders](https://github.com/libretro/slang-shaders) authors (various licences, largely GPL).

## Licence
The plug-ins' own code is under the [MIT licence](LICENSE), Copyright (c) 2026 Anthony Mendez, and
Copyright (c) 2026 Finn McKenty for the NTSCRT code it is ported from.

Bundled third-party components keep their own licences: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)
and [licenses/](licenses/). In particular, librashader (MPL-2.0) is a separate library file and the
RetroArch CRT shaders (several under the GPL) are separate shader source files, both shipped unmodified next
to the MIT plug-in; their licences cover those files only. The same notices ship inside every bundle
(`Contents/Resources/`).

## Build from source
You need CMake 3.24+ (4.x works), Rust stable (`rustup`), Python 3, Git, and the submodules:
```
git clone --recursive https://github.com/anthonymendez/NTSCRT-Plugins.git
cd NTSCRT-Plugins
```
(or `git submodule update --init --depth 1` in an existing clone). Other dependencies (OpenFX SDK,
nlohmann/json, stb, doctest, the Slang compiler) are downloaded at configure time.

### Windows
Visual Studio 2022 (or its Build Tools) with the C++ workload. Use a **short build folder** such as
`C:uild
tscrt`: deep paths break MSBuild and the Rust builds.
```
cmake -S . -B C:/build/ntscrt -G "Visual Studio 17 2022" -A x64
cmake --build C:/build/ntscrt --config Release -- -m
ctest --test-dir C:/build/ntscrt -C Release
```
The plug-in is `C:/build/ntscrt/ofx/Release/NTSCRT.ofx.bundle`: copy it into
`C:\Program Files\Common Files\OFX\Plugins\` and restart Resolve.

### macOS
Xcode 15 or later (or its Command Line Tools), CMake, and both Rust targets for a universal build:
```
rustup target add aarch64-apple-darwin x86_64-apple-darwin
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build --parallel "$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```
The plug-in is `build/ofx/Release/NTSCRT.ofx.bundle`: copy it into `/Library/OFX/Plugins/` and restart Resolve.
Leave out `CMAKE_OSX_ARCHITECTURES` to build for your Mac only. The Final Cut plug-in needs Apple's FxPlug SDK:
see [fxplug/README.md](fxplug/README.md).

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Releasing: [docs/RELEASING.md](docs/RELEASING.md).
