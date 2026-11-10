# Contract changes log
Append one entry per change to a Wave-0 contract file: date, agent, file, change, reason.

- 2026-11-10, agent A, docs/params.md: `look["shader"]["preset"]` is the upstream preset **id**
  (`glow_gauss`, `royale`, … from Presets.swift), not a .slangp path — that's what every bundled preset
  stores. The schema's `crt.preset` option values changed accordingly (each option carries its `slangp`).
  Affects agent D (CrtStage) and the hosts.
- 2026-11-10, agent A, docs/params.md (+ stages.h comment now stale): `look["crop"]` uses upstream's keys
  `ratio`/`x`/`y`/`scale` (+ legacy `position`), plus `enabled`; a crop object without `enabled` is on
  (upstream only saves a crop that's on). Wave-0 `aspect`/`center`/`zoom` are still accepted.
- 2026-11-10, agent A, docs/params.md: "missing keys = upstream defaults" made concrete — notably ntsc and
  glitch default **on** when their key/`enabled` is absent (upstream AppState), ntsc settings overlay the
  house defaults. `NtscStage::active({})` is true.
- 2026-11-10, agent A, docs/params.md: schema additions — top-level `constants` and `digital_formats`;
  optional param fields (`unit`, `percent`, `scale`, `encode: "number"` for 0/1-stored bools, `structure`,
  `position`, `neutral`, `integral`, option `applies`/`legacy`/`hint`/`slangp`); hosts write only params
  whose `visible_if` holds; choice options append-only and ids unique after `[^A-Za-z0-9]`→`_`
  (coordinator rule; the generator enforces both).
- 2026-11-10, agent A, docs/params.md: feedback_json gains `"enabled"` (`fb:/enabled`, schema
  `screenloop.enabled`) and the schema exposes upstream's `fb:/seconds`. Affects agent D (Howlaround::active
  should treat `enabled: false` as off) and the hosts.
- 2026-11-10, agent A, rust/ntscrs-capi: added `ntsc_process_frame_ex` (ntsc-rs scale_factor + seed offset);
  the upstream C API is unchanged. ntsc uses ctx.render_scale as ntsc-rs's scale_factor only when
  `scale_with_video_size` is off (upstream has no equivalent: it always passes 1,1), and adds ctx.seed to
  `random_seed`.
