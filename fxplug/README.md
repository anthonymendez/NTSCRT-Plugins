# NTSCRT for Final Cut Pro (FxPlug 4)

An FxPlug 4 effect for Final Cut Pro 12.4+ and Motion, built on the same
`ntscrt_core` C ABI as the OpenFX plug-in.

## What gets built

```
NTSCRT.app                                   wrapper app (required by FxPlug 4)
  Contents/MacOS/NTSCRT                      installs the FCP effect template, then quits
  Contents/Resources/Motion Templates/NTSCRT/NTSCRT.moef
  Contents/PlugIns/NTSCRT Renderer.pluginkit XPC service = the plug-in (CFBundlePackageType XPC!)
    Contents/MacOS/NTSCRTRenderer            universal arm64 + x86_64
    Contents/Frameworks/FxPlug.framework, PluginManager.framework   (copied from the SDK)
    Contents/Resources/presets/, shaders/, param_ids.json
```

- `src/NTSCRTPlugIn.mm` — `FxTileableEffect` (Objective-C++) over `ntscrt.h`:
  - parameters are created at load time from `ntscrt_param_schema_json()`
    (schema groups → FxPlug subgroups, bool → checkbox, int/float → sliders,
    choice → popup, color → color well), plus *Preset* (bundled looks),
    *Load Preset…* / *Save Preset…* buttons, *Screen Loop* (Off / Custom / bundled
    loops), *Seed* and *History Frames*;
  - `pluginState:` evaluates every parameter at the render time and builds the
    look JSON (and Screen Loop JSON); with a temporal stage on (Digital, Glitch,
    Screen Loop) it also evaluates the look at up to 16 earlier frames (`look_at`);
  - `scheduleInputs:` requests the effect source at the render time plus
    *History Frames* earlier frames; those reach the core through `fetch_source`;
  - renders the whole frame (`kFxPropertyKey_NeedsFullBuffer`) with
    `ntscrt_render_metal` on the host's `MTLDevice` (one `ntscrt_instance` +
    command queue per concurrent render per GPU), falling back to
    `ntscrt_render_cpu` directly on the IOSurfaces;
  - any parameter change bumps a generation counter; an instance whose
    generation is stale (or that sees a different look for a frame it rendered
    before, e.g. after a keyframe edit) calls `ntscrt_flush_caches` first.
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

Debugging: `log stream --predicate 'eventMessage CONTAINS "[NTSCRT]"'`.
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
   Optional: secret `FXPLUG_SDK_SHA256` = `shasum -a 256` of the file to pin it.
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
- `ntscrt_render_metal` was still a stub when this was written; until it lands
  every frame takes the CPU fallback (logged once per process).
- Parameter retrieval at earlier times for `look_at` is capped at 16 frames
  (each costs a full parameter sweep over XPC); older frames reuse the current look.
- Whether hosts call `parameterChanged` while *opening* a project. If they do for the Preset / Screen Loop menus, the chosen preset would be re-applied over the saved values; move preset application behind a "last applied" hidden param if that shows up.
