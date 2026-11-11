# Security policy

## Supported versions

Only the [latest release](https://github.com/anthonymendez/NTSCRT-Plugins/releases/latest) gets fixes.
The installers and install scripts always install the latest release, so updating is the fix.

## Reporting a vulnerability

Please **don't open a public issue** for a security problem. Report it privately through GitHub:
*Security → Report a vulnerability* on this repository
([direct link](https://github.com/anthonymendez/NTSCRT-Plugins/security/advisories/new)).

Include what you found, how to reproduce it (a project, preset file or command if you have one) and the
version (*Presets → Version* in the effect, or `Contents/Resources/VERSION` inside the bundle).

This is a one-person hobby project, so there's no guaranteed response time; expect an answer within a
couple of weeks. Once a fix is released the advisory is published, with credit if you'd like it.

## What's in scope

- The plug-in bundles (`NTSCRT.ofx.bundle`, `NTSCRT.app`) and what they load: preset JSON files and the
  bundled shaders.
- The installers (`.msi`, `.pkg`) and the install scripts (`install.ps1`, `install.sh`), including how they
  download and verify releases.
- `scripts/ntscrt_doctor.py`, including what it collects into its diagnostics zip.

Bugs in DaVinci Resolve, Final Cut Pro, or in third-party components (librashader, ntsc-rs, slang-shaders)
belong with those projects, though a report here is welcome if NTSCRT is how they're reached.
