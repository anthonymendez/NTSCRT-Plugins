# Parameters, looks and presets

## Look JSON (per-frame input to the core)
The host passes the upstream NTSCRT **look preset format** (see `presets/*.json`; synced with upstream
NTSCRT v0.20.0, look `"version"` 2) to `ntscrt_render_*`, evaluated at the frame being rendered:

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
    "animatable": true,                           // false = upstream "structure" params / enums / bools
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
the order a release shipped, then newer presets alphabetically (`ofx/ofx_params.cpp`
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

**Host time:** the timeline starts at the effect's first frame (OFX: the source clip's first frame, the frame
the core calls 0) and lasts `duration` seconds at the host's frame rate: `N = round(duration × fps)` frames,
frame `n` at `t = (n mod N) / (N - 1)`, so it **loops** until the effect ends, as upstream's playback does (the
timeline's own `fps` is ignored: it is upstream's rate for stills). Applying the preset clears every look
param's keyframes and writes host keyframes for the params that move: one per frame for numbers (exact at
every frame whatever interpolation the host uses between keys), one per change for bools and choices; params
that hold still get a plain value. At most 3600 frames are keyed (the last values hold after that); with an
unknown effect length, one pass. The FxPlug model offers the same (`Model::preset_keys`).
