# NTSCRT for Final Cut Pro (FxPlug 4)

An FxPlug 4 effect for Final Cut Pro 12.4+ and Motion, built on the same
`ntscrt_core` C ABI as the OpenFX plug-in.

**Status: not released.** It compiles in CI when the FxPlug SDK secret is set (see below), but it has
not run in Final Cut Pro or Motion yet, and releases don't include it. See
[Not verified yet](#not-verified-yet-no-mac-here-ci-only-compiles-and-inspects-the-bundle).

## What gets built

```
NTSCRT.app                                   wrapper app (required by FxPlug 4)
  Contents/MacOS/NTSCRT                      installs the FCP effect template, then quits
  Contents/Resources/Motion Templates/NTSCRT/NTSCRT.moef
  Contents/PlugIns/NTSCRT Renderer.pluginkit XPC service = the plug-in (CFBundlePackageType XPC!)
    Contents/MacOS/NTSCRTRenderer            universal arm64 + x86_64
    Contents/Frameworks/FxPlug.framework, PluginManager.framework   (copied from the SDK)
    Contents/Resources/presets/, shaders/, param_ids.json
    Contents/Resources/LICENSE, THIRD_PARTY_NOTICES.md, licenses/   (also in the app's Resources)
```

- `src/NTSCRTPlugIn.mm` — `FxTileableEffect` (Objective-C++) over `ntscrt.h`:
  - parameters are created at load time from `ntscrt_param_schema_json()`
    (schema groups → FxPlug subgroups, bool → checkbox, int/float → sliders,
    choice → popup, color → color well), plus *Preset* (bundled looks),
    *Load Preset…* / *Save Preset…* buttons, *Screen Loop* (Off / Custom / bundled
    loops), *Seed*, *History Frames*, *Debug Logging* and *Open Logs Folder*;
  - `pluginState:` evaluates every parameter at the render time and builds the
    look JSON (and Screen Loop JSON); with a temporal stage on (Digital, Glitch,
    Screen Loop) and some parameter keyframed (`FxKeyframeAPI_v3`) it also evaluates
    the look at up to 16 earlier frames (`look_at`), each built once per parameter
    generation and reused by the next frames;
  - `scheduleInputs:` requests the effect source at the render time plus the earlier
    frames the stages read (`ntscrt_source_frames_needed`: Digital video back to its
    keyframe, Screen Loop's window; none for Glitch or the CRT), at most *History
    Frames*; those reach the core through `fetch_source`;
  - renders the whole frame (`kFxPropertyKey_NeedsFullBuffer`) with
    `ntscrt_render_metal` on the host's `MTLDevice` (one `ntscrt_instance` +
    command queue per concurrent render per GPU), falling back to
    `ntscrt_render_cpu` directly on the IOSurfaces; renderers, caches and the
    parameter generation belong to one effect instance (one `NTSCRTPlugIn` object);
    a tiled output renders the frame once and copies the other tiles from it;
  - a parameter change drops only the core caches it can affect
    (`ntscrt_flush_caches_mask`, the same mapping as the OpenFX plug-in); a frame
    rendered before that now has a different look (an unreported keyframe edit)
    drops every temporal cache;
  - `NTSCRT_ERR_GPU` / `NTSCRT_ERR_MISSING_RESOURCE` / `NTSCRT_ERR_SOURCE_UNAVAILABLE`
    still wrote a complete frame: the render succeeds and the reason is logged once.
- `model/` — host-independent parameter model (stable IDs, value conversion,
  look assembly, plug-in state). Built and unit-tested on every platform
  (`test_fxplug_model`).
- `param_ids.json` — **append-only** map from schema ids to FxPlug parameter
  IDs. FCP/Motion projects store values by these numbers: never edit or reuse
  one. After the schema changes run `python3 fxplug/tools/update_param_ids.py`
  and commit the result (CI warns when it is stale; unmapped params still work
  with a hashed fallback ID, but that ID is not guaranteed stable).
- `tools/make_motion_template.py` — generates the FCP effect template.

## Building locally (Mac)

1. Install Xcode 15+ and the **FxPlug SDK 4.3.x** from
   <https://developer.apple.com/download/all/> (search "FxPlug"; needs an Apple
   developer login). It installs to `/Library/Developer/Frameworks` and
   `/Library/Developer/SDKs/FxPlug.sdk`.
2. `rustup target add aarch64-apple-darwin x86_64-apple-darwin`
3. ```
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
   cmake --build build --target ntscrt_fxplug -j
   ```
   Result: `build/fxplug/dist/NTSCRT.app` (ad-hoc signed by default).
   Options: `-DNTSCRT_FXPLUG_SDK_DIR=...`, `-DNTSCRT_CODESIGN_IDENTITY="Developer ID Application: …"`,
   `-DNTSCRT_FXPLUG_BUNDLE_ID=com.example.NTSCRT`, `-DNTSCRT_SHADERS_DIR=<dir copied to Resources/shaders>`.
4. Copy `NTSCRT.app` to `/Applications` and open it once: that registers the
   plug-in with PlugInKit and installs the template into
   `~/Movies/Motion Templates.localized/Effects.localized/NTSCRT/`.
   Check registration with `pluginkit -mAvvv -p FxPlug`.
5. Restart Final Cut Pro; the effect is under **Effects → NTSCRT**.

Debugging: the renderer logs to `~/Library/Containers/<renderer bundle id>/Data/Library/Logs/NTSCRT/ntscrt-fxplug.log`
(load, failures, the first 20 renders; every frame with timings and the look while *Debug Logging* is on;
*Open Logs Folder* shows it in Finder). Load lines and failures also reach the system log:
`log stream --predicate 'eventMessage CONTAINS "[NTSCRT]"'`.
FCP stops using a plug-in that crashes 3 times within 30 s; restart FCP after a crash.

## CI: enabling the FxPlug job (what you must do)

The SDK can't be downloaded anonymously, so the `fxplug` job in
`.github/workflows/ci.yml` fetches it from a private URL you provide. Without
the secret the job succeeds with a notice "FxPlug skipped".

1. Download the FxPlug SDK (e.g. `FxPlug_SDK_4.3.x.dmg`) from
   <https://developer.apple.com/download/all/>.
2. Put it somewhere CI can download with a plain HTTPS GET that is not public,
   e.g. one of:
   - a release asset on a **private** GitHub repo, with a pre-signed URL; or
   - an S3/R2/GCS object with a long-lived pre-signed URL; or
   - any private file host giving a direct-download link.
   Accepted payloads: the `.dmg` as shipped, the `.pkg`, or a `.zip` of either
   (or a zip of an installed `/Library/Developer` tree).
3. In GitHub → *anthonymendez/NTSCRT-Plugins* → Settings → Secrets and variables
   → Actions → **New repository secret**: name `FXPLUG_SDK_URL`, value = that URL.
   Also required: secret `FXPLUG_SDK_SHA256` = `shasum -a 256` of the file (the job fails without it).
4. Re-run the `ci` workflow. The job installs the SDK (`scripts/ci/install_fxplug_sdk.sh`),
   builds `ntscrt_fxplug`, verifies plists / universal binary / signature and
   uploads `NTSCRT-FxPlug-macos-universal` (ad-hoc signed).

Apple's SDK license doesn't allow redistributing it: keep the URL private and
never commit the SDK.

Distribution builds need a Developer ID certificate (`NTSCRT_CODESIGN_IDENTITY`,
which also enables the hardened runtime) and notarization
(`xcrun notarytool submit … --wait`, `xcrun stapler staple NTSCRT.app`); CI doesn't do that.

## Motion template

FCP only shows FxPlug effects through Motion templates. The generated
`NTSCRT.moef` applies the filter (by plug-in UUID `6E1C2A8B-7D44-4C1E-9B7A-3F5D2E8C1A90`)
to the Effect Source and publishes the plug-in's top-level **NTSCRT** group
(parameter ID 1, which contains every control). It deliberately has no Project
Loop End marker because the effect is temporal.

The generator writes Motion's XML by hand and has not been opened in Motion
yet. If FCP doesn't list the effect or shows no controls, author it in Motion
once and commit it; it then replaces the generated file:

1. Motion → New → **Final Cut Effect**. Apply *Filters → NTSCRT → NTSCRT* to *Effect Source*.
2. Select the filter; in the Inspector, on the **NTSCRT** group's pop-up choose **Publish**.
3. File → Save (Publish) as *NTSCRT*, category *NTSCRT*.
4. Copy `~/Movies/Motion Templates.localized/Effects.localized/NTSCRT/NTSCRT/NTSCRT.moef`
   to `fxplug/motion/NTSCRT/NTSCRT.moef` and rebuild.

## Avoiding "shows in Motion but not in Final Cut Pro"

There's an unanswered Apple developer forum thread (FCP 12.2, SDK 4.3.4) about
exactly this. This project avoids every likely cause we know of:

- there is a Motion template (FCP lists effects only through templates);
- the XPC service is **sandboxed** (the failing report had no sandbox; Apple's
  template and shipping FCP plug-ins are sandboxed), app sandboxed too;
- the Info.plist matches FxPlug 4 plug-ins that work in FCP:
  `PROXPCProtocol`, `PrincipalClass FxPrincipal`, `com.apple.version 1.1`,
  `NSViewService_PKSubsystem`, `XPCService` with `_NSApplicationMain`,
  `JoinExistingSession`, `_AdditionalSubServices/viewbridge`,
  `ProPlugDynamicRegistration = NO`, `protocolNames = FxFilter`;
- `FxPlug.framework` and `PluginManager.framework` are **embedded** in the
  pluginkit (`@loader_path/../Frameworks`) and signed inside-out, so the
  service runs the SDK it was built against instead of a mismatched one
  (the reported crash was an unrecognized selector from `startServicePrincipal`);
- the plug-in bundle id is prefixed by the wrapper app's id.

## Not verified yet (no Mac here; CI only compiles and inspects the bundle)

- Running in FCP/Motion: parameter UI, preset buttons (NSOpen/SavePanel from the
  XPC service), template publishing, temporal scheduling.
- Image orientation: the renderer flips rows when a tile reports
  `kFxImageOrigin_BOTTOM_LEFT` (Apple: Motion is bottom-left, FCP top-left).
  If output is upside down, set `NTSCRT_FXPLUG_FLIP=0` or `1` with `launchctl setenv`
  to test, then fix `BottomUp()` in `NTSCRTPlugIn.mm`.
- The Metal render path: `ntscrt_render_metal` is implemented in the core (Metal GPU device,
  `core/gpu/metal/`) and the renderer uses it on the host's `MTLDevice`, but it has no test of
  its own yet and has never run inside Final Cut. If it fails, frames take the CPU fallback
  (logged once per process).
- Parameter retrieval at earlier times for `look_at` is capped at 16 frames
  (each costs a full parameter sweep over XPC); older frames reuse the current look.
  Whether `FxKeyframeAPI_v3` may be called from `pluginState:` (it's used there to
  skip earlier looks when nothing is keyframed; an error counts as "keyframed").
- Time: the effect's frame number is `timelineTime:fromInputTime:(renderTime)` minus
  the same conversion of `startTimeForEffect:`. Apple says FCP's input and timeline
  times are independent and that `startTimeForEffect:` is relative to the clip's
  native start, but not which base `renderTime` uses; with *Debug Logging* on the
  log prints the raw times (`debug state ...`) to check this.
- One plug-in object per effect instance: per-instance render history relies on it
  (Apple documents `addParametersWithError:` and `properties:` per instance, not the
  object's lifetime).
- Render scale comes from `FxImageTile.scaleX` with `kFxPixelTransform_Scale` declared;
  Apple's docs recommend the pixel transforms and don't spell out how `scaleX` relates
  to them (the debug log prints scaleX / scaleY / pixelAspect).
- `addPushButtonWithName:parameterID:selector:parameterFlags:`: Apple's reference shows
  the selector as `SEL *`; the code passes `@selector(...)` like Apple's samples.
- CI's macOS job compiles the sources against `tests/stubs/FxPlug/FxPlugSDK.h`, a stub
  of the documented API (names and types from Apple's reference, placeholder values):
  it catches typos and C++ errors, not differences from the real SDK.

Checked against Apple's FxPlug 4 reference (developer.apple.com/documentation/professional_video_applications,
2026-11-11): `protocolNames` = `FxFilter` is right for an FxTileableEffect (the plist article: it "still
requires either FxFilter or FxGenerator"); `FxImageTile.imageOrigin` / `kFxImageOrigin_BOTTOM_LEFT` exist
(Motion bottom-left, FCP top-left); `scaleX`, `requestError`, `ioSurface`, `deviceRegistryID`,
`metalTextureForDevice:` and `kFxImageTileRequestSourceEffectClip` are spelled as used;
`kFxPropertyKey_MayRemapTime` was wrong (NO, though the effect reads earlier frames; now YES) and
`kFxPropertyKey_PixelTransformSupport` was missing (now Scale).
- Whether hosts call `parameterChanged` while *opening* a project. If they do for the Preset / Screen Loop menus, the chosen preset would be re-applied over the saved values; move preset application behind a "last applied" hidden param if that shows up.
