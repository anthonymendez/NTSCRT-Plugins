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

First release (will be 0.1.0).

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

[Unreleased]: https://github.com/anthonymendez/NTSCRT-Plugins/commits/main
