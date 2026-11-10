# Parameters, looks and presets

## Look JSON (per-frame input to the core)
The host passes the upstream NTSCRT **look preset format** (see `presets/*.json`) to
`ntscrt_render_*`, evaluated at the frame being rendered:

```jsonc
{
  "downscale": {"enabled": true, "method": "...", "preset": "...", "width": 320},
  "ntsc":      {"enabled": true, "settings": { /* ~63 ntsc-rs keys, ntsc-rs "version":1 preset format */ }},
  "shader":    {"enabled": true, "preset": "crt/crtglow_gauss.slangp", "params": {"NAME": 0.5}},
  "glitch":    {"enabled": false, "values": {"<GlitchParam id>": 0.0}},
  "digital":   {"enabled": false, "values": {"<DigitalParam id>": 0.0}},
  "crop":      {"enabled": false, "aspect": "4:3", "center": [0.5, 0.5], "zoom": 1.0},   // NTSCRT-Plugins extension
  "timeline":  { /* ignored: hosts keyframe instead */ },
  "view":      { /* ignored */ }
}
```
Screen Loop is passed separately as `feedback_json` (upstream `"kind": "ntscrt-video-feedback"` files in `presets/Screen Loop/`).

Missing keys = upstream defaults. Unknown keys are ignored. Bundled preset files load unchanged.

## Param schema (`core/params/schema.json`, returned by `ntscrt_param_schema_json()`)
Drives both host UIs. Hosts create one host param per entry, then build the look JSON by
writing each param's value at its JSON-Pointer `path`.

```jsonc
{
  "version": 1,
  "groups": [{"id": "ntsc", "label": "NTSC (TV)", "parent": null}],
  "params": [{
    "id": "ntsc.composite_noise.intensity",   // stable host param id (never rename after release)
    "group": "ntsc",
    "label": "Composite noise",
    "hint": "tooltip text",
    "type": "bool" | "int" | "float" | "choice" | "color",
    "default": 0.05, "min": 0, "max": 1, "display_min": 0, "display_max": 0.5, "step": 0.001,
    "options": [{"value": "x", "label": "X"}],   // choice only; value is written to JSON
    "path": "/ntsc/settings/composite_noise/intensity",  // RFC 6901 JSON Pointer into the look (or "fb:/..." for feedback_json)
    "animatable": true,                           // false = upstream "structure" params / enums / bools
    "enabled_if": "/ntsc/settings/composite_noise/enabled"  // optional: grey out when that path is false
  }]
}
```
Rules: ids are dotted `group.name`; schema order = UI order; CRT shader params are listed per
preset under group `crt.<preset id>` with `enabled_if` on `/shader/preset == <value>` expressed as
`"visible_if": {"path": "/shader/preset", "equals": "crt/crt-royale.slangp"}`.

Host extras (not in schema, each host adds them): **Preset** choice (bundled presets from
`ntscrt_bundled_presets_json`), **Load preset…/Save preset…** (file path), **Seed** (int), **Screen Loop preset**.
Choosing a preset writes all its values into the host params (so they can then be keyframed).
