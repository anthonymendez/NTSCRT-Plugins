# Working rules for parallel agents

- Upstream source to port lives read-only in `upstream/NTSCRT` (submodule, upstream v0.20.0, commit c6ab639; see
  [UPSTREAM-SYNC.md](UPSTREAM-SYNC.md) for moving it). Read it; never edit it.
  Its own submodules (`Vendor/ntsc-rs`, `Vendor/slang-shaders`) are not checked out; `Vendor/ntscrs-capi` is.
- Contracts: `core/include/ntscrt/ntscrt.h`, `core/stages/stages.h`, `core/gpu/gpu.h`, `core/src/internal.h`,
  `docs/params.md`, `cmake/Shaders.cmake` (function signature). Don't change them; if you must, append the change
  and its reason to `docs/dev/CONTRACT-CHANGES.md` and keep it minimal.
- Edit only the paths your brief says you own. Shared files (root `CMakeLists.txt`, `cmake/Deps.cmake`):
  add lines, don't restructure; mention them in your report.
- Windows build (MSVC 2022 Build Tools, CMake 4.x, Rust stable x86_64-pc-windows-msvc):
  ```
  cmake -S . -B C:/ntb/<your-agent-letter> -G "Visual Studio 17 2022" -A x64
  cmake --build C:/ntb/<your-agent-letter> --config Release -- -m
  ctest --test-dir C:/ntb/<your-agent-letter> -C Release --output-on-failure
  ```
  Always use a short build dir under `C:/ntb/` (deep paths break MSBuild's FileTracker).
- macOS-only code (Metal, FxPlug) can't be compiled here: keep it behind `if(APPLE)`, write it carefully, and it is
  verified by GitHub Actions (`.github/workflows/`; the FxPlug sources only against stub SDK headers,
  `fxplug/tests/stubs`).
- DaVinci Resolve (tests/resolve, the preview and gallery scripts): one headless Resolve at a time across agents.
  Take the lock `mkdir C:/ntb/resolve.lock` (retry every 30 s, and wait while a `Resolve.exe` runs), `rmdir` it when
  done, even on failure; never kill a Resolve you didn't start. Environment knobs: [../ENVIRONMENT.md](../ENVIRONMENT.md).
- Tests: doctest, in `tests/<area>/` with a `CMakeLists.txt` calling `ntscrt_add_test(name files...)`.
  Port the matching upstream XCTests (`upstream/NTSCRT/Tests/CrtCoreTests/`) where they exist.
- Commit on your worktree branch with clear messages. End commit messages with:
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. Don't push; the lead merges.
- Report: what's done, what's stubbed/unverified, contract changes, shared-file edits, how to test.
