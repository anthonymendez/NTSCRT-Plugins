# Versioning and releases

One version for everything (Resolve OpenFX bundle, Final Cut Pro app, installers), kept in
[`VERSION`](../VERSION) as `X.Y.Z`. CMake reads it (`PROJECT_VERSION`), so it ends up in:

- the OpenFX plug-in: the **Version** field in its controls, its description, and
  `NTSCRT.ofx.bundle/Contents/Resources/VERSION` (what the installers print);
- the FxPlug app and renderer `Info.plist` (`CFBundleShortVersionString` / `CFBundleVersion`).

## Which number to bump
- **patch**: bug fixes only.
- **minor**: new controls or features. Adding a param is fine (schema params and choice options are
  append-only, see docs/params.md).
- **major**: a change that breaks saved projects or presets (removing / renaming a param id,
  reordering choice options). Avoid.

OpenFX identity: hosts store effects in projects by plug-in id + **OFX major version**, so the OFX
major is fixed (`kOfxIdentityMajor = 1` in ofx/ntscrt_ofx.cpp) and independent of `VERSION`; the OFX
minor is `X*10000 + Y*100 + Z`, so hosts always pick the newest install. Only bump
`kOfxIdentityMajor` if old projects really can't use the new plug-in (they'd lose the effect).

## Every change
Add a line to `CHANGELOG.md` under `## [Unreleased]` (Added / Changed / Fixed / Removed), written for
editors, not developers.

## Cutting a release
```bash
python scripts/release.py patch      # or minor / major / X.Y.Z; no argument = the version in VERSION
git commit -am "Release vX.Y.Z"
git tag -a vX.Y.Z -m "NTSCRT vX.Y.Z"
git push origin main vX.Y.Z
```
The script dates the Unreleased section as the new version and sets `VERSION`. The tag push runs CI,
which checks the tag matches `VERSION`, builds Windows + macOS (+ Final Cut when the FxPlug SDK secret is
set), and publishes a GitHub Release with that CHANGELOG section as its notes. The installers' default
(`latest`) then picks it up: users update by running the install line again.

## Installers (MSI / pkg)
Each release also carries `NTSCRT-Installer-windows.msi` (`installer/windows/`, WiX v5) and
`NTSCRT-Installer-macos.pkg` (`installer/macos/`, payload-free `pkgbuild` + `productbuild`). They are
versioned with `VERSION` (the MSI's version drives Windows upgrades) but contain no plug-in: they embed
`install.ps1` / `install.sh` from the same commit and run it elevated, which downloads the **latest**
release when the user runs the installer. So an older installer still installs the newest plug-in, and a
fix to the install scripts only reaches installer users with the next release. CI builds both and
smoke-tests them against the freshly built zips (`NTSCRT_FROM`): MSI install + uninstall on Windows, the
pkg's `postinstall` on macOS. Build locally: `installer/windows/build-msi.ps1` or
`sh installer/macos/build-pkg.sh`. Both are unsigned for now (see the README's first-run steps).
MSI properties for testing / offline installs: `msiexec /i NTSCRT-Installer-windows.msi NTSCRT_FROM=C:\path\NTSCRT-OFX-windows-x64.zip`
or `NTSCRT_VERSION=v0.3.0`; the pkg's `postinstall` reads the same two environment variables.
