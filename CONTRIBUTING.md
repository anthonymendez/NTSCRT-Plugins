# Contributing

Thanks for helping. Bug reports, test reports from your own machine, and pull requests are all welcome.

## Reporting a bug

Use the [bug report form](https://github.com/anthonymendez/NTSCRT-Plugins/issues/new/choose). The most useful
thing you can attach is the diagnostics zip from `ntscrt_doctor.py` ([docs/TESTING.md](docs/TESTING.md),
section 3): it collects the plug-in's log, your Resolve and GPU versions, and the install state, with your
user name and home folder removed. For a problem with specific frames, turn on **Debug logging** in the
effect first (same page).

Security problems: see [SECURITY.md](SECURITY.md) instead of opening an issue.

## Building and testing

See [Build from source](README.md#build-from-source) in the README. Before sending a pull request:

- `ctest` passes (Windows: `ctest --test-dir <build> -C Release`; macOS: `ctest --test-dir build`).
  A change that alters a look on purpose regenerates the golden self-references
  (`cmake --build <build> --config Release --target golden-update`, see [tests/golden](tests/golden/README.md)).
- If you changed rendering, compare against the previous build with the same preset, for example with
  `ntscrt-cli` (`tools/ntscrt-cli`) or the Resolve test suite in [tests/resolve](tests/resolve/README.md)
  (needs Resolve Studio).
- macOS code can't be built everywhere; CI builds and tests the OpenFX plug-in on Windows and macOS on every
  pull request. The Final Cut (FxPlug) sources are only syntax-checked against stub SDK headers there, never
  built, because the `FXPLUG_SDK_URL` secret isn't set (see [fxplug/README.md](fxplug/README.md)); build them
  yourself with the FxPlug SDK if you change them.

## Pull requests

- Keep each pull request to one change, and describe what it fixes and how you checked it.
- Add a line for every user-visible change under `## [Unreleased]` in [CHANGELOG.md](CHANGELOG.md).
  Don't edit `VERSION`; releases are cut with `scripts/release.py` ([docs/RELEASING.md](docs/RELEASING.md)).
- Parameter IDs are stable: never rename or reuse an id in `core/params/schema.json` or
  `fxplug/param_ids.json`, since saved projects refer to them. `fxplug/README.md` explains how to add new ones.
- The core is a port of [NTSCRT](https://github.com/finnmckenty/NTSCRT): a change to the look should match what
  the NTSCRT app does, or say why it differs.
- Don't add files whose licence doesn't allow redistribution. If you add a third-party component, add it to
  [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and its licence text to `licenses/` (Rust crates:
  `python scripts/gen_rust_licenses.py`).

Notes for working on the code base (contracts between components, parallel-work rules) are in
[docs/dev/](docs/dev/).

## Licence

By contributing you agree that your contribution is licensed under the [MIT licence](LICENSE), like the rest
of the project's own code.
