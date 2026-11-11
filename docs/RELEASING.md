# Versioning and releases

One version for everything (Resolve OpenFX bundle, Final Cut Pro app, installers), kept in
[`VERSION`](../VERSION) as `X.Y.Z` (or `X.Y.Z-rc.N` for a release candidate). CMake reads it
(`PROJECT_VERSION` is the numeric `X.Y.Z`; the `-rc.N` shows in the plug-in's Version field and
`Contents/Resources/VERSION`), so it ends up in:

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
`release.py` refuses to run on a dirty tree, on a branch other than `main`, or when `main` is behind
`origin/main` (it runs `git fetch origin main`), so the tag is always a commit CI has built on main.
The script dates the Unreleased section as the new version and sets `VERSION`. The tag push runs CI,
which checks the tag matches `VERSION`, builds Windows + macOS (+ Final Cut when the FxPlug SDK secret is
set), and publishes a GitHub Release with that CHANGELOG section as its notes. The installers' default
(`latest`) then picks it up: users update by running the install line again.

Tag builds restore the Rust build caches that `main` saved (same sources = same cache key), so a tag
pushed right after its `main` run doesn't rebuild librashader / ntsc-rs from scratch.

### Release candidates
```bash
python scripts/release.py rc         # 0.4.0 -> 0.4.1-rc.1; 0.4.1-rc.1 -> 0.4.1-rc.2 (or pass 0.5.0-rc.1)
git commit -am "Release v0.4.1-rc.1" && git tag -a v0.4.1-rc.1 -m "NTSCRT v0.4.1-rc.1"
git push origin main v0.4.1-rc.1
python scripts/release.py            # later: VERSION 0.4.1-rc.N -> the final 0.4.1
```
An `-rc.N` tag is published as a GitHub **prerelease**: `releases/latest` (what the install scripts and
installers download by default) skips it, so only testers who ask for it get it:
`install.ps1 -Version v0.4.1-rc.1`, `install.sh --version v0.4.1-rc.1`, `msiexec ... NTSCRT_VERSION=v0.4.1-rc.1`.
A candidate only changes `VERSION`; the CHANGELOG's Unreleased section stays open and is the
candidate's release notes, until the final release dates it. The MSI's version is numeric, so a
candidate's MSI is `X.Y.Z` too (the final release replaces it: `AllowSameVersionUpgrades`).

### What a release contains, and how to check it
Assets: `NTSCRT-OFX-windows-x64.zip`, `NTSCRT-OFX-macos-universal.zip`, `NTSCRT-FxPlug-macos-universal.zip`
(when built), the two installers, and **`SHA256SUMS`** (SHA-256 of every other asset). The zips carry
`LICENSE` and `THIRD_PARTY_NOTICES.md` at their root (the licence texts, `licenses/`, are inside the
bundle at `Contents/Resources/licenses`). Each asset
also has a signed **build provenance attestation** (`actions/attest-build-provenance`) saying which
workflow run and commit built it:
```bash
sha256sum -c SHA256SUMS --ignore-missing                       # macOS: shasum -a 256 -c SHA256SUMS
gh attestation verify NTSCRT-OFX-windows-x64.zip -R anthonymendez/NTSCRT-Plugins
```
`install.ps1` and `install.sh` download `SHA256SUMS` from the same release and refuse to install a zip
that isn't listed or doesn't match. They fail closed: a release without `SHA256SUMS` is refused, except
v0.3.0 and v0.4.0, which predate checksums and install with a warning (`latest` is first resolved to its
tag through the redirect of `/releases/latest`, so the exception applies to it too). Never publish a
release without `SHA256SUMS`. `-From` / `--from` skips the check. Both accept only `latest` or a tag
of the form `vX.Y.Z[-rc.N]` for `-Version` / `--version` / `NTSCRT_VERSION`.

### Re-running a release
The `release` job is idempotent: re-run it (Actions -> the tag's `ci` run -> Re-run failed jobs) and
it re-uploads every asset with `--clobber` and refreshes the notes of the existing release instead of
failing because the release already exists.

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
or `NTSCRT_VERSION=v0.3.0`; the pkg's `postinstall` reads the same two environment variables and logs
to `/Library/Logs/NTSCRT/install.log` (and, through Installer, `/var/log/install.log`).
CI also smoke-tests the scripts' download path: it serves the built zip and a `SHA256SUMS` over local
HTTP (`install.ps1 -BaseUrl` / `install.sh --base-url`), installs, and checks that a tampered zip is refused,
that a release without `SHA256SUMS` is refused, and that `-Version v0.4.0` without one installs with a warning.

On macOS, `install.sh` only removes the quarantine flag and ad-hoc signs the OFX bundle when the bundle
has no real signature (unsigned / ad-hoc, which is every release so far); a Developer ID-signed bundle
is installed as is. `install.sh --uninstall` with no `--resolve` / `--fcp` removes both plug-ins, and
forgets the pkg's receipt (`pkgutil --forget com.ntscrt.plugins.installer`) once nothing is left.

## Golden images
CI's **Golden images** step (both OSes, strict) compares renders against `tests/golden/ref/` (upstream
NTSCRT `crt-smoke` references) and `tests/golden/self/ref/` (our own baselines); see
[tests/golden/README.md](../tests/golden/README.md). A release whose look changed on purpose
regenerates the self-references first: `cmake --build <build> --config Release --target golden-update`,
review the image diff, commit. After an upstream sync, re-run the **golden** workflow and commit its
`golden-refs` artifact into `tests/golden/ref/`.

The same goes for the pictures in the docs: after a look change, or when presets are added, regenerate the
README previews (`python scripts/make_previews_resolve.py --build <build>`) and the
[presets gallery](PRESETS-GALLERY.md) (`python scripts/make_presets_gallery.py --build <build>`; `--only` for
a few presets). Both render through the plug-in in headless Resolve Studio (closed first) and check the
result against `ntscrt-cli`; `NTSCRT_BUILD_DIR` can stand in for `--build`
([ENVIRONMENT.md](ENVIRONMENT.md)).
