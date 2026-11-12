# Parameters, looks and presets

## Look JSON (per-frame input to the core)
The host passes the upstream NTSCRT **look preset format** (see `presets/*.json`; synced with upstream
NTSCRT v0.21.0, look `"version"` 2) to `ntscrt_render_*`, evaluated at the frame being rendered:

```jsonc
{
  "downscale": {"enabled": true, "method": "...", "preset": "...", "width": 320},
  "ntsc":      {"enabled": true, "settings": { /* ~63 ntsc-rs keys, ntsc-rs "version":1 preset format */ }},
  "shader":    {"enabled": true, "preset": "glow_gauss", "params": {"NAME": 0.5}},   // preset = upstream Presets.swift id
  "glitch":    {"enabled": true, "values": {"<GlitchParam id>": 0.0}},     // toggles stored as 0/1 numbers
  "digital":   {"enabled": false, "values": {"<DigitalParam id>": 0.0},    // choices stored as index numbers
                "legacy": {"quality": 0.22}},   // NTSCRT-Plugins: 0.4.0 controls, see "Digital video" below
  "crop":      {"enabled": false, "ratio": "1:1", "x": 0.5, "y": 0.5, "scale": 1.0},
  "alpha":     {"mode": "clip"},   // NTSCRT-Plugins addition: "clip" | "spill" (see below)
  "timeline":  { /* ignored by the core: hosts turn its keys into keyframes (below) */ },
  "view":      { /* ignored */ }
}
```
Screen Loop is passed separately as `feedback_json` (upstream `"kind": "ntscrt-video-feedback"` files in
`presets/Screen Loop/`: `{"kind", "version": 2, "values": {...}, "seconds"?}`). NTSCRT-Plugins adds
`"enabled"` (schema param `screenloop.enabled`, path `fb:/enabled`); hosts may instead pass NULL when off.

Missing keys = upstream defaults (upstream AppState initial values: downscale on, ntsc on with the house
settings, glitch on with healthy values, digital off, shader on with `glow_gauss`, crop off). Unknown keys
are ignored. Bundled preset files load unchanged.

**digital** (upstream `DigitalParam` ids; `codec` 0 = DCT, 1 = Microsoft Video 1, which ignores `chroma`,
`deblocking` and `datamosh`). Looks saved before upstream 0.19 (`"version"` 1, or no `"version"` with
`exposure` / `black_level` / `lens_blur` and no `ccd_response`, as the 0.4.0 plug-ins saved them) are
migrated as upstream does (`core/stages/digital/migrate.h`): Quality converted from JPEG's curve to the
same quantizer on today's even curve, Exposure / Black level become CCD response (the stronger decides),
Lens softness is dropped. `digital.legacy` holds the 0.4.0 plug-in controls a saved project still has
(hidden schema params `digital.quality`, `digital.exposure`, `digital.black_level`, `digital.lens_blur`,
written only while non-zero): `legacy.quality` (old curve) stands in for `values.quality` while that is at
its default 0.749; legacy Exposure / Black level set CCD response while it is 0.

Upstream 0.21 added `low_light_noise` (0…1, default 0: Sensor noise grows up to 8× as the frame's median
brightness falls below sRGB 100) and `sharpening_width` (Halo width, 1…4 capture px, default 1 = the 3×3
sharpening of earlier versions). Both defaults leave older looks unchanged, so a look without them (any look
saved before 0.21) renders as before; the Digital video formats write them at those defaults. Upstream's
new-window starting values (the *Cheap camcorder* look) are not schema defaults: a new instance starts from
the controls' defaults, and *Cheap camcorder* is a bundled look. 0.21 also changed the sensor noise's random
pattern (each row's seed is hashed, so rows no longer repeat the row six above a pixel along): a look with
Sensor noise above 0 renders different grain of the same strength.

With Fields on One field (the default since upstream 0.20) and the Downscale on, the chain after the
Digital stage gets the field: half the Downscale's lines, each two picture rows tall (the CRT draws a
scanline per field line); Screen Loop keeps square lines.

**ntsc.settings**: ntsc-rs `"version": 1` preset keys (flat; a group's name is its on/off bool). The stage
overlays the look's settings on the house defaults (ntsc-rs defaults + upstream `AppState.appNtscDefaults`),
so a partial object works; numbers are coerced to the type ntsc-rs expects (3.0 → 3 for ints/enums, 0/1 for
bools). Legacy ntscQT presets pass through unchanged.

**crop** (upstream `AppState.lookDictionary` form): `ratio` "W:H" (upstream offers 1:1, 3:4, 2:3, 9:16, 1:2,
4:3, 3:2, 16:9, 2:1; any positive pair works), `x`/`y` 0…1 (where the crop sits where there's room; 0.5 =
centered), `scale` 1…8 (zoom inside the largest crop). Legacy `position` sets x and y. Upstream writes the
object only when the crop is on, so **a crop object without `enabled` counts as on**; hosts write `enabled`
explicitly. The Wave-0 spellings `aspect` / `center` / `zoom` are still accepted. Output = the cropped region
at source pixel density (SourceCrop.swift geometry, even sizes).

**alpha.mode** (no upstream equivalent; matters only for sources with transparency, e.g. titles):
- `"clip"` (default): the chain sees the element's own colours (unpremultiplied; fully transparent areas are
  filled with the nearest edge colour so blurs and smears don't pull black into the edges). Output alpha =
  source alpha.
- `"spill"`: the chain sees the element over black. Output alpha = max(source alpha, brightest output
  channel): ghosts, smear, glow and noise outside the element show as light over what's underneath, like a
  Screen blend of the element-over-black render.
The host says whether its RGB is premultiplied (`ntscrt_render_args.source_premultiplied`) and must keep
the output's convention equal to the source's. With crop on, source alpha no longer lines up with the
picture: Clip outputs opaque, Spill uses the brightness term only. CRT curvature / Screen Loop geometry
moves the picture but not the source alpha, so Clip cuts the warped picture at the unwarped shape.

## Param schema (`core/params/schema.json`, returned by `ntscrt_param_schema_json()`)
Drives both host UIs. Hosts create one host param per entry, then build the look JSON by
writing each param's value at its JSON-Pointer `path`.

```jsonc
{
  "version": 1,
  "groups": [{"id": "ntsc", "label": "NTSC (TV)", "parent": null}],
  "params": [{
    "id": "ntsc.composite_noise_intensity",   // stable host param id (never rename after release)
    "group": "ntsc",
    "label": "Composite noise",
    "hint": "tooltip text",
    "type": "bool" | "int" | "float" | "choice" | "color",
    "default": 0.05, "min": 0, "max": 1, "display_min": 0, "display_max": 0.5, "step": 0.001,
    "options": [{"value": "x", "label": "X"}],   // choice only; value is written to JSON
    "path": "/ntsc/settings/composite_noise_intensity",  // RFC 6901 JSON Pointer into the look (or "fb:/..." for feedback_json)
    "animatable": true,                           // false = holds for a whole render (upstream "structure", stage switches, menus outside the timeline)
    "enabled_if": "/ntsc/settings/composite_noise"  // optional: grey out when that path is false
  }]
}
```
Rules: ids are dotted `group.name`; schema order = UI order; CRT shader params are listed per
preset under group `crt.<preset id>` with `enabled_if` on `/shader/preset == <value>` expressed as
`"visible_if": {"path": "/shader/preset", "equals": "royale"}`. `visible_if` without `equals` means "while
the value at `path` is truthy"; params marked `"legacy": true` use it on their own path (shown and written
only while non-zero, or with an `equals` that never matches: never).

Generated by `tools/gen_schema.py` (stdlib Python; reads upstream Swift, the ntsc-rs descriptor snapshot
`core/params/ntsc_descriptors.json` from the `ntsc-dump` tool, and `#pragma parameter` lines of
libretro/slang-shaders @ cb01f2f2). Don't hand-edit schema.json.

**Saved-project compatibility (hosts store choices by index, params by id):**
- a choice's `options` may only be **appended** — never reordered or removed (the generator refuses,
  unless `--allow-breaking`, which is only for before the first release);
- ids never change or disappear, and stay unique after mapping non-alphanumerics to `_` (host param names).

**Writing the look:** for each param write its value at `path` (bools with `"encode": "number"` as 1/0;
choices write the option's `value`, which may be a string or a number). Also write every entry of the
top-level `"constants"` map (`/version`, `/ntsc/settings/version`, `fb:/kind`, `fb:/version`). Params
whose `visible_if` is false are **not** written: CRT presets share `/shader/params/<NAME>` keys
(e.g. both glow presets have `BOOST`), so only the selected preset's params go into the look.
`enabled_if` is UI-only (grey out); still write the value. `"preset": false` = applying or loading a
preset leaves that param alone (presets describe a look, not how the clip's transparency is used).

Optional fields (hosts may ignore them):
`unit` (display suffix), `percent` (display value×100 as %), `scale: "log"` (slider mapping),
`display_min`/`display_max` (slider range inside `min`/`max`), `encode: "number"`, `structure` (digital
codec "structure" param: holds for a whole render, upstream doesn't keyframe it), `position`
(Screen Loop point set by dragging on the preview), `neutral` (double-click value), `integral` (shader
param with integer steps), option fields `hint`, `legacy` (kept so old looks render; upstream UI hides
it), `slangp` (preset file), and `applies` — a map of path → value written when that option is chosen
(downscale presets set the width).

`"digital_formats"` (top level): upstream `DigitalFormat.all`, each `{id, name, width, hint, applies}`;
upstream 0.20 lists them in the Preset menu (a "Digital video" section). Choosing one writes every
`applies` entry: `/digital/enabled` true, `/digital/values` (the full values object), the legacy controls
cleared, the Downscale on with the format's width. The rest of the look stays as it is.

Bundled presets in the hosts' Preset menus: released presets keep their index (saved projects store it):
the order a release shipped, then newer presets alphabetically (`hostparams/src/params.cpp`
`released_preset_order`; add the new names there when releasing). The Digital video formats are menu entries
`Digital video/<name>` (in `released_preset_order` since 0.5.0, after that release's new looks).

Screen Loop: `fb:/seconds` (upstream "Length" for stills, 0.5…60 s) is the time drift/push/spin/loop
span; hosts may set it from the effect duration.

**Choice `applies`:** when the user picks an option that has `applies`, the host writes each entry into the
param owning that path (Horizontal resolution `SNES (256px)` sets Width 256). When the user then edits such a
value by hand so it no longer matches, the choice drops to its option without `applies` (`Custom`), as
upstream's Downscale panel does. The Digital video formats' Downscale labels (`MiniDV (720px)`, ...) are
options too (appended after `Custom`), so a look made with a format keeps its label.

**Writing presets:** the look a host saves is built as above, so it is upstream's file format: constants
(`"version": 2`), toggles stored as 1/0 where `encode: number` says so. NTSCRT-Plugins adds the Screen Loop
settings under `"screen_loop"` (a feedback object; upstream ignores the key, Load applies it) and, while the
loop is on, writes them beside the look as `<name> (Screen Loop).json` too. Saving writes the look at the
playhead (not the keyframes).

Host extras (not in schema, each host adds them): **Preset** choice (bundled presets from
`ntscrt_bundled_presets_json`), **Load preset…/Save preset…** (file path), **Seed** (int), **Screen Loop preset**.
Choosing a preset writes all its values into the host params (so they can then be keyframed), clearing
those params' keyframes first; a Digital video format writes only what it applies.

## Animated presets (`timeline`)
Upstream looks can carry `"timeline": {"duration": seconds, "fps", "enabled", "keys": [...]}`, each key
`{"t": 0…1, "easing": "Linear"|"Ease in"|"Ease out"|"Ease in-out"|"Hold", "shader", "ntsc", "glitch",
"digital"}` (upstream `Sources/CrtApp/Timeline.swift`). Evaluation (`core/params/timeline.h`, shared by both hosts
and `ntscrt-cli --timeline`) is upstream's: the segment's easing shapes its progress; CRT params lerp in Float and
snap to their step; ntsc-rs floats lerp, ints lerp and round, bools / enums hold; Glitch toggles and Digital
choices hold; Digital structure controls (frame rate, keyframe interval, codec, chroma, fields, aliasing) never
key; keys carry the whole ntsc settings object (it replaces the look's), the other sections merge over the
look's values. A look with keys animates whatever `enabled` says (upstream opens the timeline when one loads).

**Host time:** the timeline starts at the effect's first frame (the frame the core calls 0) and lasts
`duration` seconds at the host's frame rate: `N = round(duration × fps)` frames, frame `n` at
`t = (n mod N) / (N - 1)`, so it **loops** until the effect ends, as upstream's playback does (the timeline's
own `fps` is ignored: it is upstream's rate for stills). OFX: frame 0 is the source clip's first frame when the
host time lies inside the clip's frame range (Fusion, and Resolve's Edit page for clips: times from 0 at the
clip's start). Resolve gives effects without media (an Adjustment Clip) timeline times, an hour or more in,
with a made-up range of 0..1999: then frame numbers are host times (frame 0 = the timeline's frame 0) and the
effect's length is unknown.

Applying the preset clears every look param's keyframes and writes **sparse** host keyframes for the params
that move (`preset_keys`): numbers get a key where their per-frame curve bends (a piecewise-linear fit within
0.5% of the value's swing, capped at 0.5% of its slider range and at 2% of the value itself, half a step for
finely stepped CRT values; exact, i.e. a key wherever the curve isn't a straight line, for integers, the Glitch
receiver's timing controls and ntsc-rs's `*_scale` settings, and wherever the value holds still). That is exact
for hosts that interpolate linearly between keys: Resolve's Edit and Fusion pages (measured by the Resolve test
`position`) and FxPlug, whose keys are made with linear segments (`guards` adds a key either side of each bend
for smooth-spline hosts). Bools and choices get two keys per change (the old value on the frame before, the new
one on the frame it changes). Params that hold still get a plain value. The keyed stretch is whole passes from
frame 0, as many as about `kMaxPresetKeys` (3000) keys allow (at least one pass, at most 3600 frames), covering
the playhead (a quarter of it before the playhead when the effect is longer) and inside the effect when its
length is known; before it the timeline's first values hold, after it the last ones. `tests/hostparams` checks
every bundled animated preset against the per-frame evaluation, and `test_ofx` renders sparse keys and a key on
every frame to the same pixels. Both plug-ins get these keys from the shared model (`plan_preset`).

**Histories:** the stages that simulate a history from frame 0 (the Glitch receiver, Digital video's stream
with Datamosh) restart it every minute of the effect (`core/src/sim_segment.h`; whole seconds of the rounded
frame rate, so a minute or an hour of timecode is always a boundary): a frame depends only on the frames since
its minute started. The first minute is upstream's behaviour; the bound keeps an edit an hour into a timeline
as cheap as one at its start.

## One model for both plug-ins (`hostparams/`)
Everything above is implemented once, in the host-independent library `hostparams/` (`ntscrt::host`,
`ntscrt_host/params.h`, no OpenFX or Apple headers, tested on every platform in `tests/hostparams`): schema
parsing, value coercion, colours, JSON pointers, building the look and feedback, `enabled_if` / `visible_if`,
`applies` and Digital video formats, preset application (`plan_preset`: values, timeline keyframes, the
`screen_loop` key of our files, migration), the menu order, Save preset's files, the seed range, which core
caches an edit can affect and the rotating log file (`ntscrt_host/file_log.h`). The OpenFX plug-in calls it on
look-typed values; the FxPlug plug-in through `fxplug/model/param_model.h`, which adds only FxPlug's own parts
(stable numeric ids, control layout, menu-index / RGB values, the plug-in state). `test_hostparams_cross` checks
that every bundled preset builds the same look and feedback through both; `test_hostparams` checks the OpenFX
plug-in builds exactly what it built before the merge (`tests/hostparams/ofx_reference.json`).

The two plug-ins used to have separate models. Where they differed, the shared one keeps the OpenFX behaviour
(the released one, and upstream's where upstream has one), except where noted:

| Behaviour | OpenFX (0.4.0 / 0.5.0-dev) | FxPlug (never released) | Shared model |
|---|---|---|---|
| Look built from | an empty object | a hidden saved copy of the last preset, params written over it (kept non-schema keys and other CRT presets' stale shader params) | an empty object (upstream `AppState.lookDictionary` writes only its state). FxPlug's hidden *Base Look* / *Base Screen Loop* params stay (ids are never removed) but are no longer read |
| Screen Loop on / off | the `screenloop.enabled` param; feedback always passed | the Screen Loop menu: Off = no feedback, any other entry = feedback with the checkbox's value | the `screenloop.enabled` param; feedback always passed (`"enabled": false` when off, which the core treats as off). FxPlug's menu Off / (Custom) now set that param |
| Feedback `kind` / `version` | written first, then the schema constants | `kind` only if the base lacked it, then the constants | OpenFX's |
| Screen Loop file without `"enabled"` | turns Screen Loop on (the core's "missing means on") | schema default (off), worked around in the `.mm` | on; the workaround is gone |
| `enabled_if` / `visible_if` spelling | pointer string or `{path, equals}` for both | `enabled_if` only as a string, `visible_if` only as an object | both spellings for both |
| Condition evaluation | on the owning param's value; numbers equal to 1e-9; a path no param owns = true | on the built look; strict JSON equality; a missing value = hidden | OpenFX's (on param values, so a hidden owner still decides) |
| Choice values in presets | the option's value (numbers to 1e-9) or its label | the exact value only | OpenFX's |
| Numbers in presets | rounded (ints), not clamped (the host clamps to the control's range) | clamped to the schema range | clamped to the schema's `min` / `max` when it has them (what OpenFX hosts did anyway); no bundled preset is out of range |
| Unknown / out-of-range menu index | the default option | the last option | the default option |
| Colours | `#rrggbb`, `rrggbb`, 8 digits, 3 or 4 numbers, alpha kept | `#rrggbb` or 3 numbers | OpenFX's |
| `animatable` missing in the schema | true | true only for numbers and colours | true (the schema always says) |
| Feedback presets detected by | the bundled list's `kind` or the file's `kind` | the bundled list's `kind` | either |
| Preset menu order | released presets first (`released_preset_order`), then new ones alphabetically | the core's alphabetical list | OpenFX's (saved projects store the index) |
| Seed | 0…2147483647 | 0…1,000,000 | 0…2147483647 (`kSeedMax`), so no OpenFX project's seed is clamped |
| Save preset: `screen_loop` key, side file | key always, side file while the loop is on | both only while the menu isn't Off | OpenFX's |
| Temporal stages on (FxPlug schedules earlier frames) | — | any feedback object counted | Digital, Glitch, or Screen Loop **on** |
| Log rotation | `ntscrt-ofx.old.log` beside the log, whatever the log's name | `<stem>.old<ext>` | `<stem>.old<ext>` (the same for the default names) |
| Bundled presets read | through a core instance (a GPU device made and dropped at describe time, ~140 ms) | the same, with a Metal device | straight from `Resources/presets` (`bundled_presets_json`), no device |

