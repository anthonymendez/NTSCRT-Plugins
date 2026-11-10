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
- **Transparency → Transparent areas** for titles and clips that don't fill the frame: *Clip to element*
  (effect stays inside the element, clean edges) or *Let effect spill* (ghosting, smear and glow extend past
  it as light over what's underneath). Presets don't change it.
- **Version** shown in the plug-in's controls (Resolve).
- One-line installers: `install.ps1` (Windows) and `install.sh` (macOS, Resolve and Final Cut).

### Fixed
- Installers replace the plug-in atomically: a failed or interrupted install leaves the previous version
  working, and installing while Resolve is open works (the new version loads on restart).
- Premultiplied sources no longer get dark fringes or halos at soft edges.
- Fusion: sources that don't cover the frame (e.g. Text+) rendered black.

[Unreleased]: https://github.com/anthonymendez/NTSCRT-Plugins/commits/main
