# NTSC on the GPU (ntsc-rs port)

Status: **all 4 phases done; on by default** on Windows / D3D11 hardware devices (the rule: [The default
rule](#the-default-rule)), Metal off. Every ntsc-rs pass runs on the GPU, bit-identical to ntsc-rs: all 25
bundled presets with NTSC on, the house look and the 5 Screen Loop presets render entirely on it, on the CPU
path, on host GPU frames (`gpu-direct`) and inside Screen Loop. `NTSCRT_NTSC_GPU=0` brings back ntsc-rs
(0.5.0-rc.4). The Rust path (ntsc-rs through `rust/ntscrs-capi`) stays the reference and the fallback forever.

At UHD the ntsc-rs step was ~38-48 ms of a ~55 ms frame and everything on the GPU ~2.5 ms. ntsc-rs has no GPU
implementation, so this is a port of its effect, pass by pass, to compute shaders, with ntsc-rs's arithmetic:
on the GPUs it targets the port's output is **bit-identical** to ntsc-rs's (measured, below).

| Phase | Scope |
|---|---|
| 1 | Scaffolding, plane format, the general row IIR filter, CPU-made per-row tables, per-frame fallback, parity harness; every pass that is a row filter or per pixel / per row, **including the mandatory chroma modulation and all four demodulation filters**. |
| 2 (done) | Composite / luma / chroma noise (Fbm simplex on the GPU from per-row seeds), snow (speckle event lists). |
| 3 (done) | The VHS passes: head switching, tracking noise, chroma delay (`shift_row`), edge wave. |
| 4 (done) | Screen Loop (the frame's and every history pass's NTSC on the camera texture), host-GPU frames (`gpu-direct` with NTSC on), default on, the plan / table cache, the route in the log. Metal: still off (unverified). |

## Files

| File | What |
|---|---|
| `rust/ntscrs-capi/src/gpu_plan.rs` | The plan exports: settings -> pass list, coefficients, row tables, speckle lists (ntsc-rs's own code or exact copies of it). |
| `rust/ntscrs-capi/src/gpu_noise.rs` | Exact copy of ntsc-rs's gradient noise (`noise.rs`, `noise/*`), for the CPU-sampled shift noise. |
| `rust/ntscrs-capi/include/ntscrs_capi.h` | Their C declarations (`NtscGpu*` structs, `ntsc_gpu_plan`, `ntsc_gpu_row_seeds`, `_row_shifts`, `_speckles`, `_simd_lanes`, `ntsc_debug_process_yiq`, ...). |
| `core/stages/ntsc/gpu/ntsc_gpu.h/.cpp` | `NtscGpu` (the driver: textures, tables, dispatches), `NtscGpuPlan`, the route rule. |
| `core/stages/ntsc/gpu/shaders/ntsc_gpu.slang` | The kernels. |
| `core/stages/ntsc/ntsc.cpp` | `NtscStage::gpu_plan()` / `gpu_tables()` / `debug_planes()`: the same settings handling as `process()`. |
| `core/src/ntscrt.cpp` | `ntsc_gpu_on` (the instance's route), `ntsc_gpu_run` (plan, tables, passes, fallback), `render_once` (the pipeline, Screen Loop hooks). |
| `tests/ntsc/test_ntsc_gpu.cpp` | Parity harness, coverage report, fallback / error / cache tests, GPU and table timing. |
| `tests/howlaround/test_howl_seek.cpp` | Screen Loop: the default (GPU) byte-identical to `NTSCRT_NTSC_GPU=0` for the 5 presets, play and cold seeks. |
| `tests/hostgpu/test_host_gpu.cpp` | Host GPU frames with NTSC on: `gpu-direct`, 0 values different from the CPU path with ntsc-rs. |

## Data flow

```
render_once, NTSC on (every frame)
  ntsc_gpu_on(inst)                the instance's route: ntsc_gpu_route(dev) once (again when NTSCRT_NTSC_GPU
                                   changes or the device is replaced), off after 3 GPU errors in a row
  no Screen Loop:
    the source goes straight to the GPU (DirectSrc) unless Digital video is on: the host GPU frame's convert
    kernel (gpu-direct) or the CPU picture converted into upload memory (CPU path); else the CPU picture
    ntsc_gpu_run:
      NtscStage::gpu_plan(.., false)   -> shim ntsc_gpu_plan (or the cached plan: same frame again)
      plan.supported()                 every enabled stage implemented          -> else ntsc-rs
      async NtscStage::gpu_tables      row seeds, row shifts, speckle lists (CPU, while the picture is made)
      picture()                        the 8-bit picture (BGRA8 texture)
      NtscGpu::run(tex, plan)          from_bgra8 -> pass kernels -> to_bgra8 -> BGRA8 texture
    -> Downscale -> Glitch -> CRT      (no CPU NTSC step, no second upload)
  Screen Loop:
    camera = Howlaround::camera(upload(picture))   history passes (chain_input / chain_input_begin):
                                                   ntsc_gpu_run on the camera texture, Downscale, Glitch
    ntsc_gpu_run(camera) -> Downscale -> Glitch -> CRT, after_frame
  any step failing (not a lost device): ntsc-rs on the CPU picture for this frame / pass, exactly as before
```

The decision is per instance, the fallback per frame (and per Screen Loop pass). The timings line
(`ntscrt_last_timings`, the OpenFX log) gets ` ntsc_route=gpu (<why>)` (plus ` ntsc_tables_ms=<CPU time of
the plan and tables>`, ` ntsc_cached=1` when the cache kept them, and with `NTSCRT_PROFILE=1` ` ntsc_gpu_ms=<GPU
time of the passes>` and ` ntsc_gpu=<pass>:<ms>,...`) or ` ntsc_route=cpu (<why>)` on every frame with NTSC
on, and Screen Loop frames ` ntsc_loop=gpu:<history passes>,cpu:<passes>`. The OpenFX plug-in writes
`instance #N NTSC route: <route> (<why>)` to its log when an instance's route text changes (once per
instance, unless a setting or a failure changes it), e.g. `NTSC route: gpu (auto: NVIDIA GeForce RTX 5090,
fma mad, CPU AVX2+FMA)`.

## Plane format

Y, I, Q: three `R32F` textures of `width x plane_rows`, plus a fourth `R32F` scratch plane (ntsc-rs's
`YiqView::scratch`: the modulated signal during demodulation, a plane copy for the vertical blend and for every
shift). They are bound as `RWTexture2D<float>` u0..u3 for every kernel (R32F is the one format D3D11 FL11_0 /
Metal tier 1 can read from a writable texture) and pooled per stage (`NtscGpu::Impl::acquire`, released planes
reused next frame).

`plane_rows` and the row order are exactly ntsc-rs's YIQ buffer (`yiq_fielding.rs`): `Upper` = ceil(h/2) rows
(image rows 0, 2, ...), `Lower` = max(h/2, 1) rows (1, 3, ...), `Both` = h rows, `InterleavedUpper` = the upper
field's rows then the lower field's (h rows), `InterleavedLower` the other way round. `Alternating` is Upper or
Lower by frame parity. Each field of an interleaved frame runs the effect with its own frame number (2f, 2f+1),
exactly as `apply_effect_to_all_fields`; on the GPU both fields go through each pass in one dispatch, and what
differs per field lives in the row tables. All six `use_field` modes are implemented and tested.

## Tables (CPU -> GPU)

Everything random, 64-bit or `f64`, every transcendental function and every division, is computed on the CPU
by the shim, with ntsc-rs's code: ntsc-rs keeps `filter`, `random`, `noise`, `VHSTapeSpeed::filter_params` and
the pass functions of `ntsc.rs` private, so `gpu_plan.rs` / `gpu_noise.rs` carry exact copies (same Rust, same
operation order, same crates: `siphasher`, `rand`, `rand_xoshiro`, `fearless_simd` at ntsc-rs's versions),
each naming the lines it reproduces. (D3D11 division is not correctly rounded, so no kernel divides.)
`process_impl` and the plan share `effect_for()`, so both start from the same `NtscEffect`. Layouts below are
C (`ntscrs_capi.h`), mirrored with `static_assert`s in `ntsc_gpu.h`.

**`NtscGpuPlanInfo`** (72 bytes): `seed`, `field_frame[2]`, `field` (0 Upper .. 4 InterleavedLower),
`plane_rows`, `num_fields`, `field_first[2]`, `field_rows[2]`, `num_passes`, `unsupported` and `enabled` (bit
per `NTSC_ST_*` stage), `horizontal_scale`, `vertical_scale`.

**`NtscGpuPass`** (224 bytes), in `apply_effect_to_yiq_field` order: `stage` (`NTSC_ST_*`), `kind`
(`NTSC_GPU_K_*`), `planes` (bit 0 Y, 1 I, 2 Q), `mode`, `a`, `b`, `pad[2]`, `filter[3]` (one per plane):

| kind | what | parameters |
|---|---|---|
| `FILTER` | row IIR on `planes` | `filter[p]` |
| `LUMA_BOX` | input luma box filter | - |
| `CHROMA_INTO_LUMA` | modulation | row `xi` |
| `DEMOD` | `luma_into_chroma` | `mode` 0 box, 1 notch (`filter[0]`), 2 one-line comb, 3 two-line comb; `pad[0]` = 1: some fields have one row (notch on those rows, comb on the others) |
| `ROTATE_CONST` | chroma phase error | `a` = sin, `b` = cos |
| `ROTATE_ROWS` | chroma phase noise | row `phase_sin/cos` |
| `CHROMA_LOSS` | VHS chroma loss | row flag |
| `VERT_BLEND` | chroma vertical blend | per field |
| `NOISE` | `video_noise_line` (composite, luma, chroma I / Q, tracking noise) on `planes` | `mode` = row-seed pass, `a` = frequency, `b` = intensity * 0.25, `pad[0]` = octaves, `pad[1]` = 1: intensity per row (tracking noise's row shifts) |
| `SPECKLES` | add the stage's speckle events to Y (snow, tracking noise, head switching's transient) | the stage's speckle list |
| `SHIFT_ROWS` | `shift_row` per row on `planes` (head switching incl. its mid line, tracking noise, edge wave) | the stage's row shifts |
| `CHROMA_DELAY` | `chroma_delay` on I, Q | `a` = horizontal shift, `mode` = vertical offset in rows, `pad[0]` = 1: rows copied (no horizontal shift) |

**`NtscGpuFilter`** (64 bytes): `num[4]`, `den[4]` (without the leading 1; `den[3]` = 0), `zi[4]`, `len`
(2..4; ntsc-rs's maximum is 4 coefficients = 3 state variables), `delay`, `initial` (0 zero, 1 first sample),
`initial_value`. `zi` = `initial_condition_into(1.0)`: with a value of 1 every `* value` in ntsc-rs's
initial-condition code is exact, so `zi[k] * v` on the GPU is ntsc-rs's single rounding for steady state `v`.

**`NtscGpuRow`** (32 bytes, one per plane row): `src_row` (source image row: fielding), `field_row` (the row's
index in its field: ntsc-rs's per-row `index`), `field_first`, `field_rows`, `xi` (`chroma_phase_shift` with the
field's frame number), `flags` (bit 0: chroma loss), `phase_sin`, `phase_cos` (phase noise rotation, from
`Seeder(seed).mix(4).mix(frame).mix(row)`).

**`NtscGpuOutRow`** (8 bytes, one per output row): plane rows `a`, `b`; `a == b` copies, else
`(row b + row a) * 0.5` (`DeinterlaceMode::Bob`).

**`NtscGpuRowSeed`** (16 bytes, one per plane row; `ntsc_gpu_row_seeds(pass)`): `seed_lo/hi` (the row's
Xoshiro seed `Seeder(seed).mix(pass).mix(frame).mix(row as u64)`), `noise_seed` (`rng.next_u32()`, the Fbm seed)
and `offset` (`rng.random::<f32>() * width`): everything `video_noise_line` draws before the noise itself, for
`composite_noise` (pass 0) and `plane_noise` (1 chroma I, 9 chroma Q, 10 luma). Pass 3
(`NTSC_GPU_SEEDS_TRACKING`) is tracking noise's: its seeder is `Seeder(seed).mix(3).mix(frame).mix(2)` (the `2`
an `i32`, as ntsc-rs's literal), only the affected rows (the bottom `round(height * vertical_scale)` of each
field) are filled and their index is `index + cut_off_rows`; the other rows are zero. The driver turns a pass's
seeds into the kernel's `NoiseRow {seed, offset, intensity, active}`.

**`NtscGpuRowShift`** (16 bytes, one per plane row; `ntsc_gpu_row_shifts(stage)`): `shift` (pixels, `shift_row`
semantics), `boundary` (0 constant 0, 1 extend, -1 `NTSC_GPU_SHIFT_NONE`: row untouched), `copy_start` (head
switching's mid line: the first pixel taken from the shifted row; -1 the whole row), `intensity` (head
switching: the mid line's transient intensity, for information; its values are speckles. Tracking noise: the
row's noise-line intensity, `intensity_scale^2 * noise_intensity * 4 * 0.25`). Head switching: shift
`(horiz_shift * ((index + offset) / num_rows).powf(1.5) + rand - 0.5) * horizontal_scale` per row; the mid
line's jitter and `copy_start` (a `copy_start` past the width leaves the row alone, as ntsc-rs returns there).
Tracking noise: `sample_noise_1d(Simplex 0.5)` of the field (gpu_noise.rs) times the row's `index / num_rows`,
the wave intensity, `0.25`, the horizontal scale. Edge wave: `sample_noise_2d(Fbm)` per row at `(offset,
frame * speed)` (gpu_noise.rs), `/ 0.022 * intensity * 0.5 * horizontal_scale`, boundary extend.

**`NtscGpuSpeckle`** (16 bytes per transient; `ntsc_gpu_speckles(stage)` + an `f32` value array): `row`, `x0`
(clamped to the row), `count`, `value_offset`. `row_speckles` draws an `f64` logistic, a geometric distribution
and a fresh RNG per transient (`rng.jump()`): all of it, and the `cos * (1 - x/len)^2 * rand` product per pixel,
runs on the CPU (a copy of `row_speckles` that records the values instead of adding them), so the GPU only adds
`values[value_offset + i]` at `x0 + i`, in list order. Snow: `Seeder(seed).mix(6).mix(frame).mix(row)` per row;
tracking noise: its row seeds' seeder with `snow_intensity * intensity_scale^2`; head switching's transient:
`(1 - x / (16 * horizontal_scale))^3 * intensity` after `copy_start`. The rows are generated on a rayon pool of
the physical cores (as ntsc-rs's own pool); events come out in row order and, within a row, in ntsc-rs's order
(non-decreasing `x0`). Too little room: the call returns 3 with the counts (the driver grows its buffers).

## Arithmetic: why the GPU matches bit for bit

ntsc-rs runs its filters through `fearless_simd`: `filter_signal_in_place_fixed_size_simdx4` (every filter of
2-4 coefficients, i.e. all of them) uses `mul_add`, a real FMA on AVX2+FMA (x86-64-v3) and NEON, and so do
`rgb_to_yiq` / `yiq_to_rgb`, the noise (`simplex_1d`, Fbm's octave sum) and `shift_row`'s SIMD blocks.
Everything else is plain scalar Rust (never contracted). The kernels follow that operation by operation:

- where ntsc-rs fuses, the kernels fuse: `y = fma(num0, x, z0)`, `z[k] = fma(-den[k], y, fma(num[k+1], x,
  z[k+1]))` (the SIMD lanes, `z[3] = 0`), and the colour matrices' `fma(r, c0, fma(g, c1, b * c2))`;
- elsewhere every value is `precise` (no compiler fusion or reordering), sums in ntsc-rs's order (e.g. the box
  filters' running sums, the comb filters' `cur * 0.5 + prev * 0.25 + next * 0.25`);
- the demodulation's three products are exact (multipliers 0, +-1, +-0.5) and at most two are non-zero, so the
  SIMD order (centre, left, right) and the scalar order (centre, right, left) agree;
- constants are ntsc-rs's `f32`s (`16/255`, `1/255` passed from C++; the decimal matrix constants round to the
  same floats);
- input bytes are recovered from the UNORM texture (`round(v * 255)`) and normalised as ntsc-rs does
  (`i32 -> f32 * (1/255)`); output is `(v * 255).min(255).max(0)` truncated, like `from_norm`.

Phases 2-3 add:

- **Noise** (`noise_add`): ntsc-rs evaluates `add_noise_1d` `N` lanes at a time (`N` = `S::f32s::N` of its SIMD
  level: 8 on AVX2, 4 on SSE4.2 / NEON; the shim exports it, `ntsc_gpu_simd_lanes`, and the plan carries it).
  Lane `i` of chunk `k` samples the coordinate `offset + i`, then `+= N` `k` times, each addition rounded; the
  kernel reproduces that chain in closed form (`chain_add`: adding a power of two no larger than the ulp is
  exact inside a binade, so the steps up to the next power of two are one exact multiply-add and only the
  crossing step rounds, as ntsc-rs's does) instead of `k` additions. Then Fbm (`x * frequency`, `x * 2` per
  octave, `fma(sample, amplitude, result)`), `simplex_1d` (`floor`, the ESGTSA hash, the gradient's
  `copysign` - negative when `h & 8 == 0` -, `fma(-x0, x0, 1)`, `fma(t40 * g0, x0, t41 * g1 * x1)`) and the add
  `row + noise * intensity` (a separate multiply and add, as ntsc-rs). No denormals occur there (D3D11 flushes
  them; ntsc-rs on the CPU doesn't), and the 2D simplex of the edge wave, where they could, is CPU-side.
- **Shifts** (`shift_rows`, `chroma_delay`): `shift_row` / `shift_row_to` compute each output pixel from two
  input pixels with a formula that depends on where the pixel is: inside the SIMD blocks of `simd_safe_range`
  `(left - right).mul_add(frac, right)`, at the scalar edges `left * frac + right * (1 - frac)`; the kernel
  computes the same block range (with `N`) and uses the same formula per pixel. Both functions are pure
  functions of the input row (verified against `shift.rs` itself on 20000 random rows, shifts and boundaries),
  so in place or not doesn't matter; the kernels read a scratch copy of the plane.
- **`mad()` must be `precise`**: a `mad()` whose result isn't `precise` may be split into a multiply and an add
  by the driver (measured on the RTX 5090: `shift_row`'s blend came out unfused, 1 ulp off, until the result
  was `precise`). `MadFma::fma3` now stores into a `precise` variable.
- **Speckles** (`add_speckles`): one thread per pixel adds, in the row's event order, the values of the events
  that cover it (events start at non-decreasing `x0` and are at most `ix.x` long, so a binary search finds the
  first candidate): the same sums in the same order as ntsc-rs's in-place `+=`.

`mad()` is "fused or not, implementation-defined" in HLSL. `NtscGpu` probes it once per device
(`probe_fma`: `mad(1 + 2^-13, 1 + 2^-13, -(1 + 2^-12))` is 2^-26 only when fused). NVIDIA (and AMD, Apple)
fuse: the `mad` kernels are used. WARP doesn't: the `*_x` variants of the kernels with multiply-adds
(`from_bgra8`, `to_bgra8`, `row_filter`, `noise_add`, `shift_rows`, `chroma_delay`) compute them in double
precision (`double(a) * double(b)` is exact, the sum rounds to double then to float: a double rounding that
differs from a true FMA only when the double lands exactly halfway between two floats). `NtscGpu::fma_mode()`
says which ("mad", "double", or "mad-unfused" on a device with neither). Note that ntsc-rs itself is only as
exact as its CPU: on a pre-AVX2 x86 (fearless_simd's SSE4.2 level) `mul_add` is a separate multiply and add,
and the GPU would then differ from it by the amounts WARP showed before the probe (below).

**Measured** (`test_ntsc_gpu`; the 4 test pictures; frames 0/1/2/7/31 x seeds 0/99/4e9 for the full effect,
frames 0/1/7 x 2 seeds per single stage, frames 0/1/30 for the presets):

| Device | Every stage alone, each mode (77 cases) | All stages, 6 field modes x 5 sizes (720x480, 333x247, 0.5 render scale, 5x6, 1920x1080) | Stage-by-stage YIQ planes (all 22 stages, 3 field modes) | 25 presets + house look (720x480, 1920x1080, 0.5 scale) |
|---|---|---|---|---|
| RTX 5090 (`mad`, fused) | 77 exact: max 0, 0 values differ | 30 exact | max abs diff 0 at every stage | 26 exact |
| WARP (`double`) | 77 exact | 30 exact | 0 at every stage | 26 exact |
| Metal (Apple M-series, CI, report only; phase 1's passes) | - | max 1 level, 0.19% of values different | plane diffs <= 6e-8 | - |
| WARP with plain `mad` (not fused; phase 1, before the probe) | max 1-3 levels, 0.0005-5.6% differ | - | 1e-7 .. 1.3e-5 (ringing's gain) | - |

Metal (`macos` CI job, report-only, the route is off there): phase 1 measured max 1 level, 0.19% of values
different, plane differences <= 6e-8 for its passes, i.e. not bit-exact (Slang drops `precise` for Metal and
Metal compiles with fast math). The phase 2-3 kernels compile and run on Metal in CI (`test_ntsc_gpu` passes there, report-only), but its numbers for the new stages were not captured: ctest prints the report-only messages only when a test fails, so phase 4 should run `test_ntsc_gpu -s` (or with `NTSCRT_NTSC_GPU_VERBOSE=1`) on a Mac to get them.

Through the whole pipeline (`bench_transfer --dump`, 3840x2160 and 1920x1080, `rgba32f` and `rgba8` frames) the
output with `NTSCRT_NTSC_GPU=1` is byte-identical to the output without it for the house (`default`) look,
*Tracking Noise up*, *High Chroma static*, *Mild VHS*, *Obliterated*, *Medium VHS* and `ntsc_gpu_passes`; and
the `default`, `clean_crt` and `ntsc_off` looks render byte-identically to phase 1 (ffab3f9) with the variable
unset and with it set to 1 with a stage forced back to ntsc-rs (fallback).

The tolerance in the tests is the HOST-GPU.md rule (each channel within 1/255, at most 0.2% of values
different), the same for every case; it holds with room to spare because the result is exact. The unfused
row shows why the probe exists: ntsc-rs truncates to 8 bits (no rounding), so a float error of one ulp flips a
level wherever a value sits on an exact level (flat areas of 8-bit sources do), and the near-Nyquist
Butterworth lowpass (`chroma_lowpass` *Light* at the house bandwidth scale 0.3: the cutoff clamps to the
sampling rate, both poles at z = -1) amplifies rounding differences along the row.

## The default rule

An instance's NTSC frames take the GPU route when all of these hold (`ntsc_gpu_route`, decided on its first
NTSC frame); otherwise they run the Rust path exactly as 0.5.0-rc.4 did (byte-identical: tested):

1. `NTSCRT_NTSC_GPU` is unset, `auto`, `1` or `force` (`0` = ntsc-rs always).
2. The device is D3D11. **Metal is off whatever the variable**: the kernels compile and run on Metal (CI),
   but Metal compiles with fast math (Slang drops `precise` for Metal with a warning), which the bit parity
   above relies on, and the parity of the phase 2-3 kernels was never measured there.
3. Not a software device (WARP: vendor 0x1414 / device 0x8c), unless `force`: on WARP the kernels are slower
   than ntsc-rs itself.
4. The device does ntsc-rs's fused multiply-adds exactly: the fused-`mad` probe passes (`fma mad`) or the
   device has doubles (`fma double`, the `*_x` kernels), unless `force` (`mad-unfused` would differ from
   ntsc-rs by a level here and there).

The CPU: ntsc-rs fuses its multiply-adds only on AVX2+FMA (fearless_simd's x86-64-v3 level, which requires FMA)
and NEON. On an older x86 CPU (its SSE4.2 level) ntsc-rs's `mul_add` is a separate multiply and add, so there
the GPU (always fused, with that CPU's SIMD width from `ntsc_gpu_simd_lanes`) differs from that machine's
ntsc-rs by up to a level in places, as WARP-with-plain-`mad` did before the probe. **The GPU is used there
anyway**: the project's references (golden images, the bundled presets' look) come from AVX2+FMA machines, so
on such a CPU neither route reproduces them bit for bit; the GPU's arithmetic is the one the references were
made with (fused), and such CPUs (pre-2013 Intel, pre-2015 AMD) are where ntsc-rs is slowest. The reason in the log says so (`CPU without AVX2+FMA (its ntsc-rs doesn't fuse: may differ by a
level)`); `NTSCRT_NTSC_GPU=0` keeps such a machine on its own ntsc-rs. (Not measured: no such CPU here.)

Per frame, a frame on an instance whose route is on still takes ntsc-rs when:

5. A stage its settings turn on isn't implemented (`NtscGpuPlan::supported`: `info.unsupported == 0`): since
   phase 3 every stage is, so this is one degenerate case (below) and `NTSCRT_NTSC_GPU_CPU_STAGES=<stage>,...`
   (debugging: those stages count as unsupported; the reason then names them, `not on the GPU yet:
   head_switching`).
6. The plan / tables / kernels throw (bad settings, a kernel that doesn't compile, a driver error) and the
   device isn't lost: that frame (or Screen Loop pass) on ntsc-rs, `ntsc_route=cpu (GPU error: ...)`. After
   **3 such frames in a row** the instance stays on ntsc-rs (`GPU error 3 frames in a row (last: ...): ntsc-rs
   for this instance from now on`, logged once); a changed `NTSCRT_NTSC_GPU` decides again. A lost device
   throws on as before: `render_impl` replaces the device and renders the frame again (the new device's
   instance decides its route again).

One case is left to ntsc-rs on purpose although the GPU could do it: `chroma_vert_blend` on a field of one row
(pictures 1-3 rows tall, depending on the field mode). ntsc-rs slices two rows of that field's scratch buffer
(`ntsc.rs` line 1123) and panics, which fails the frame (`ntsc: frame processing failed (2)`); the plan marks
it unsupported so both routes behave alike. (An ntsc-rs bug; harmless at real sizes.) The comb filters'
fallback to the notch on a 1-row field (`luma_into_chroma`, ntsc.rs 488-495) is per field, which phase 1 left to
ntsc-rs when the fields of a picture disagreed (an interleaved picture 3 rows tall); the GPU does it per row now.

**Chroma delay's negative vertical offset is not an upstream bug.** The phase-1 notes suspected that
`chroma_delay`'s `Ordering::Less` branch (ntsc.rs 993-1016) reads row `offset - 1` for every destination row.
It indexes `(offset - 1) * width..offset * width` into `i_src_part`, the slice `split_at_mut((dst_row_idx + 1) *
width)` returns after the destination row, so the row it reads is `dst_row_idx + offset`: a correct upward
move. The GPU implements that (`chroma_delay`: row `dst + offset` of the same field, 0 where
`dst >= max(height, offset) - offset`) and is exact against ntsc-rs for offsets -1, -2, -3, -300 (tests
`chroma_delay_h*_v-*`). ntsc-rs quirks the port does reproduce: the vertical blend's 1-row panic (above), the
comb filters' per-field notch fallback, head switching's mid line leaving its row unshifted when `copy_start`
lands past the width, tracking noise's and snow's speckles starting 64 pixels left of the row, the edge wave's
seed not mixing the frame number (only its noise coordinate moves with it).

## Coverage

Every ntsc-rs setting and where it runs on the GPU:

| Setting (preset key) | GPU |
|---|---|
| `random_seed`, the host's seed offset | yes (CPU tables) |
| `use_field` (all 6 modes) | yes |
| `filter_type` (constant-k, Butterworth) | yes |
| `input_luma_filter` (none, box, notch) | yes |
| `chroma_lowpass_in` (none, light, full) | yes |
| `composite_preemphasis` (composite sharpening) | yes |
| `video_scanline_phase_shift`, `_offset` | yes |
| chroma modulation / `chroma_demodulation` (box, notch, 1-line comb, 2-line comb) | yes (always runs; the comb's 1-row-field notch per field) |
| `composite_noise` (`_intensity`, `_frequency`, `_detail`) | yes (phase 2: per-pixel Fbm on the GPU, row seeds on the CPU) |
| `snow_intensity`, `snow_anisotropy` | yes (phase 2: speckle values on the CPU) |
| `head_switching` (`_height`, `_offset`, `_horizontal_shift`, `_start_mid_line`, `_mid_line_position`, `_mid_line_jitter`) | yes (phase 3: row shifts and the transient on the CPU) |
| `tracking_noise` (`_height`, `_wave_intensity`, `_snow_intensity`, `_snow_anisotropy`, `_noise_intensity`) | yes (phase 3: shift noise and speckles on the CPU, noise lines on the GPU) |
| `luma_smear` | yes |
| `ringing` (`ringing_frequency`, `_power`, `_scale`) | yes |
| `luma_noise` (`_intensity`, `_frequency`, `_detail`) | yes (phase 2) |
| `chroma_noise` (`_intensity`, `_frequency`, `_detail`) | yes (phase 2) |
| `chroma_phase_error` | yes |
| `chroma_phase_noise_intensity` | yes |
| `chroma_delay_horizontal`, `chroma_delay_vertical` | yes (phase 3) |
| `vhs_settings` / `vhs_tape_speed` (SP, LP, EP filters and chroma delay) | yes |
| `vhs_edge_wave_enabled` (`vhs_edge_wave`, `_speed`, `_frequency`, `_detail`) | yes (phase 3: row shifts from CPU-sampled 2D noise) |
| `vhs_chroma_loss` | yes |
| `vhs_sharpen_enabled` (`vhs_sharpen`, `_frequency`) | yes |
| `vhs_chroma_vert_blend` | yes (not on a 1-row field: ntsc-rs panics there, left to it) |
| `chroma_lowpass_out` | yes |
| `scale_settings` (`bandwidth_scale`, `vertical_scale`, `scale_with_video_size`), render scale | yes (CPU side) |
| any of the above inside Screen Loop | yes (phase 4) |

Bundled presets (the 29 looks in `presets/`, 25 with NTSC on; `test_ntsc_gpu` prints this with
`NTSCRT_NTSC_GPU_VERBOSE=1`) that run entirely on the GPU:

| After phase | Presets |
|---|---|
| 1 | *Mild VHS* (1 of 25) |
| 2 | *Mild VHS* (every other preset also uses a phase-3 stage) |
| 3 | all 25, and the house look |
| 4 (now) | the 5 Screen Loop presets too (the frame and every history pass), by default |

## Speed (phases 2-3, opt-in route)

`bench_transfer --sizes 3840x2160,1920x1080 --formats rgba32f,rgba8 --looks "default,preset:Tracking Noise up,preset:High
Chroma static,preset:Mild VHS" --frames 100 --warmup 20`, with and without `NTSCRT_NTSC_GPU=1`, with and without
`NTSCRT_PROFILE=1`; RTX 5090 + Ryzen 9 9950X3D, median of 3 runs of the per-frame medians, ms. `default` is the
house look (every stage but snow), *Tracking Noise up* the VHS-heavy preset (tracking noise 79 rows, head
switching), *High Chroma static* the noise-heavy one (composite noise 0.25, tracking-noise snow 0.13). Every
frame took the GPU route (100/100) on the GPU rows; the pictures are byte-identical to the ntsc-rs rows'
(`--dump`, above).

| Look | Size | Format | Frame, ntsc-rs | Frame, GPU | `ntsc` phase, ntsc-rs | `ntsc` phase, GPU | Frame, profiled, ntsc-rs | Frame, profiled, GPU | `ntsc`, profiled, ntsc-rs | `ntsc`, profiled, GPU | Tables (CPU), GPU route |
|---|---|---|---|---|---|---|---|---|---|---|---|
| house | 3840x2160 | rgba32f | 53.9 | **13.0** | 42.7 | 0.50 | 54.2 | 15.7 | 42.4 | 5.57 | 1.53 |
| house | 3840x2160 | rgba8 | 50.0 | **9.0** | 42.7 | 0.46 | 49.8 | 11.2 | 42.6 | 5.38 | 1.68 |
| house | 1920x1080 | rgba32f | 15.7 | **3.9** | 12.6 | 0.34 | 16.2 | 6.2 | 12.7 | 3.28 | 0.71 |
| house | 1920x1080 | rgba8 | 15.0 | **3.2** | 12.5 | 0.31 | 15.7 | 5.4 | 12.7 | 3.24 | 0.75 |
| Tracking Noise up | 3840x2160 | rgba32f | 54.2 | **13.3** | 42.8 | 0.53 | 54.5 | 15.3 | 42.6 | 5.42 | 1.56 |
| Tracking Noise up | 3840x2160 | rgba8 | 49.8 | **9.0** | 42.6 | 0.43 | 50.2 | 11.3 | 42.8 | 5.39 | 1.68 |
| Tracking Noise up | 1920x1080 | rgba32f | 15.6 | **3.9** | 12.4 | 0.34 | 16.3 | 6.1 | 12.4 | 3.29 | 0.72 |
| Tracking Noise up | 1920x1080 | rgba8 | 15.7 | **3.2** | 12.8 | 0.32 | 16.4 | 5.5 | 12.9 | 3.28 | 0.74 |
| High Chroma static | 3840x2160 | rgba32f | 47.9 | **12.3** | 36.4 | 0.32 | 48.3 | 14.4 | 36.4 | 4.37 | 1.40 |
| High Chroma static | 3840x2160 | rgba8 | 43.6 | **8.3** | 36.9 | 0.28 | 43.6 | 10.1 | 36.7 | 4.22 | 1.56 |
| High Chroma static | 1920x1080 | rgba32f | 13.7 | **3.6** | 10.5 | 0.24 | 14.0 | 5.4 | 10.6 | 2.52 | 0.64 |
| High Chroma static | 1920x1080 | rgba8 | 12.9 | **2.9** | 10.6 | 0.21 | 13.3 | 4.7 | 10.6 | 2.46 | 0.67 |
| Mild VHS | 3840x2160 | rgba32f | 39.2 | **11.1** | 27.8 | 0.12 | 39.4 | 12.2 | 27.6 | 2.33 | 0.07 |
| Mild VHS | 3840x2160 | rgba8 | 34.4 | **7.0** | 28.1 | 0.10 | 34.9 | 8.1 | 28.1 | 2.30 | 0.06 |
| Mild VHS | 1920x1080 | rgba32f | 11.5 | **3.0** | 8.4 | 0.07 | 12.0 | 4.2 | 8.6 | 1.20 | 0.04 |
| Mild VHS | 1920x1080 | rgba8 | 10.9 | **2.4** | 8.6 | 0.06 | 11.4 | 3.5 | 8.8 | 1.26 | 0.03 |

Unprofiled, the GPU route's `ntsc` phase is the CPU time to submit (the GPU work is waited for in `readback`);
profiled it is the plan, the tables and the kernels with a wait after each pass. "Tables" is
`ntsc_tables_ms` (the CPU time of the plan and its tables, measured in the tables' thread while the picture
uploads, so it is mostly hidden: the upload takes ~2.0 ms at UHD). Without the upload running beside it the
house look's tables take 0.52 ms at UHD and 0.39 ms at 1080p (`test_ntsc_gpu` STAGETIME); most of it is
tracking noise's speckles (an `rng.jump()` per transient) and the four noise passes' row seeds (~0.07 ms
each). Snow is the expensive table: snow intensity 0.6 (no bundled preset uses snow) brings the UHD tables to
2.5 ms. None of the tables can be cached across frames (all of them are seeded by the frame number).

Profiled phases of a GPU-route UHD frame (house look, rgba32f): in 2.5, ntsc 5.6, upload 2.0, downscale + CRT
0.6, readback 0.5, out 4.3. The rest of the frame is now the float conversions in and out (Resolve's float
frames; `gpu-direct` host frames remove them in phase 4).

GPU time per pass with `NTSCRT_PROFILE=1` (a wait after each pass, so every pass includes ~0.07-0.12 ms of
submission latency; house look, UHD, ms): vhs_tape_speed 0.39, vhs_edge_wave 0.36, tracking_noise 0.30,
chroma_noise 0.29, chroma_delay 0.24, luma_into_chroma 0.24, chroma_vert_blend 0.24, chroma_lowpass_in 0.21,
head_switching 0.19, luma_smear 0.19, luma_filter 0.19, chroma_lowpass_out 0.18, composite_sharpening 0.17,
from_bgra8 0.17, luma_noise 0.16, composite_noise 0.16, ringing 0.16, vhs_sharpen 0.15, to_bgra8 0.14,
chroma_into_luma 0.13, chroma_phase_error 0.13, chroma_phase_noise 0.12, vhs_chroma_loss 0.12; total 4.6.

Throughput without the per-pass waits (`test_ntsc_gpu` "GPU time per stage": 30 frames queued, best of 3; the
house look + snow 0.6, every stage): **3.0 ms per UHD frame, 1.06 ms per 1080p frame** for all 22 stages
(phase 1: 3.4 / 1.3 ms for its 14). The conversions and modulation alone take 0.44 / 0.15 ms; each further
stage at UHD: composite_noise +0.05, snow +0.11, head_switching +0.02, tracking_noise +0.08, luma_noise +0.10,
chroma_noise +0.13, chroma_delay +0.19, vhs_edge_wave +0.28 (three plane copies and shifts), the phase-1
filters +0.08..0.25 each (the measurements' noise is about +-0.05 ms).

On WARP (`NTSCRT_D3D11_WARP=1`, `force`): all 22 stages take 227 ms per UHD frame and 61 ms per 1080p frame
(the `double` kernels), against ~13 ms of ntsc-rs at 1080p: hence the software-device rule.

One thread per row is enough so far but is the IIR filters' cost: a UHD interleaved frame has 2160 rows, i.e.
2160 threads per filtered plane (68 warps on a GPU with 170 SMs), each walking 3840 samples. The arithmetic
chain (four dependent FMAs per sample, ~80k cycles per row) is far below the time measured; it is the memory
latency per sample that the few warps can't hide. The row filter therefore loads the next 16 samples while it
filters the current 16 (5.0 -> 3.4 ms per UHD frame in phase 1). Splitting rows into blocks with carried state
would break the bit parity (a block's start state computed another way rounds differently). The phase 2-3
passes are per pixel (noise, shifts, chroma delay, speckles), and the row-table passes are dispatched only over
the rows they touch (head switching and tracking noise: the bottom rows of each field). If later phases need
more speed, keep each row serial and make the memory access cheaper instead: a thread group stages a tile of
rows x 64-128 columns in groupshared memory with coalesced loads, each thread filters its row in the tile, the
tile goes back coalesced; or fuse consecutive filters on the same plane into one kernel.

## Debugging a parity failure

1. `test_ntsc_gpu` with `NTSCRT_NTSC_GPU_VERBOSE=1` prints a `PARITY` line per case (max level difference,
   mean, share of values different / more than one level different), a `SUMMARY` per test and `STAGE` lines:
   the YIQ planes after each stage, ntsc-rs (`ntsc_debug_process_yiq` with the stages so far) against the GPU
   (the plan built with the same `stage_mask`), max abs difference per plane. The first stage that isn't 0 is
   the culprit; stages are compared as prefixes, so a later stage can't hide an earlier one.
   `NTSCRT_NTSC_GPU_STAGE_SETTINGS=<json>` puts settings over the stage-by-stage case's (e.g. one stage alone).
2. `NTSCRT_NTSC_GPU_DUMP=<dir>` writes both sides' planes per stage
   (`<case>.<n>.<stage>.{rust,gpu}.f32`, Y then I then Q, `width x plane_rows` floats each): diff them to see
   where in the picture (a column = an IIR boundary or delay; a row = fielding or a row table; everything =
   coefficients or initial conditions; scattered 1-ulp values in a few rows = a multiply-add fused on one
   side only).
3. `NtscStage::gpu_plan(ctx, w, h, plan, stage_mask)` and `NtscGpu::planes(tex, plan, num_passes)` take any
   subset of stages / any prefix of the GPU passes (the planes before the demodulation, e.g. right after a
   shift, can be checked against a CPU model in a few lines); `NtscStage::debug_planes` is ntsc-rs with a
   stage subset. The two modulation stages always run in ntsc-rs (they can't be turned off), so they are in
   every mask.
4. Run on WARP (`NTSCRT_D3D11_WARP=1`, CI's device) to see a non-fusing device (it takes the `double`
   kernels); differences of a level in flat areas with 1e-7-sized plane differences mean a multiply-add
   that isn't fused where ntsc-rs fuses (or the reverse).
5. Check the CPU you compare against: ntsc-rs fuses only on AVX2+FMA / NEON (`fearless_simd::Level`), and its
   SIMD width (`ntsc_gpu_simd_lanes`) decides the noise coordinates and the shift formulas.

## Phase 4: Screen Loop, host GPU frames, default on

**Screen Loop.** Upstream's order with the loop on is crop -> upload -> camera -> NTSC -> upload -> Downscale
-> Glitch -> CRT, and the loop's own history passes (`HowlHooks::chain_input` / `chain_input_begin`: the
run-up, the frames before a seek) run the same chain on each pass's camera picture. Before phase 4 every one
of them downloaded the camera picture, ran ntsc-rs (history passes two at a time on worker threads, each on
its own `NtscStage`) and uploaded the result. Now `ntsc_gpu_run` runs on the camera texture itself (BGRA8, the
scene's format): `from_bgra8` reads the same bytes the download gave ntsc-rs, so the result is the same; the
pass's plan uses the pass's look and counters (`at(p.ntsc_frame - 1, pin)`, as ntsc-rs got), its own plan
object (`howl_plan`, so the frame's cache entry survives), tables built synchronously (no upload to hide them
behind). `chain_input_begin` queues the pass's NTSC on the GPU at once (it reads the camera before Howlaround
reuses it) and leaves Downscale and Glitch to the second half, so they still run in frame order. A pass that
can't take the GPU (route off, an error) takes the old CPU job, so the two kinds can mix in one run. Checkpoints
hold TV pictures only and need nothing new. Proof (`test_howl_seek` "NTSC on the GPU renders what ntsc-rs
renders"): the 5 Screen Loop presets (window 8 frames, default look), 24 frames played and cold seeks to frames
23, 17 and 9 (history re-rendered, every pass on the GPU: `ntsc_loop=gpu:N,cpu:0`) on fresh instances, each
frame byte-identical to `NTSCRT_NTSC_GPU=0` playing from frame 0; the existing seek = play tests pass on the
default route unchanged; `bench_transfer --dump` with `screenloop:7 copies slow luma` and `screenloop:22 copies
bottom left` is byte-identical at UHD and 1080p, float and 8-bit.

**Host GPU frames.** NTSC no longer keeps a frame off the direct source (`DirectSrc`): with NTSC on and
Digital video and Screen Loop off, `render_once` takes the direct picture (host GPU: `convert_in` on the host's
texture, `gpu-direct`; CPU path: the host picture converted straight into upload memory) and runs NTSC on it.
That 8-bit picture is the one the CPU path quantises before ntsc-rs (the convert kernel is exact against
`to_bgra8`: docs/dev/HOST-GPU.md), so no `from_bgra8` variant for the host's format is needed. The plan's size
is known before the picture (the look's crop rectangle), so the tables are built while the copy and convert are
queued. When the GPU route fails for a frame, the CPU picture is made the usual way (`gpu-download` on host GPU
frames) and ntsc-rs runs on it. Proof: `test_host_gpu` (default look, also cropped, at render scale 0.5, with
holes in Clip mode, a source smaller than the frame, 8-bit, half float: `gpu-direct` / `gpu-cpuconvert`, 0
values different from the CPU path with ntsc-rs except half-float output's known rounding).

**The plan / table cache.** `NtscStage::gpu_plan` keeps a one-entry cache in the `NtscGpuPlan` it fills: key =
ntsc-rs's effective settings JSON, the NTSC frame counter, the seed offset, width, height, scale, stage mask
and `NTSCRT_NTSC_GPU_CPU_STAGES`. A hit (a paused timeline rendering the same frame again, an Inspector edit of
a non-NTSC parameter) returns at once and `gpu_tables` keeps the tables (`ntsc_cached=1` in the timings); any
failure clears the key. Measured (`test_ntsc_gpu` with `NTSCRT_NTSC_GPU_VERBOSE=1`, UHD, the house look + snow
0.6): 6.9 ms of plan and tables on a miss, 0.04 ms on a hit (the house look alone: 0.3-1.5 ms, below).

**`NTSCRT_PROFILE=1`**: the `ntsc` phase then includes the GPU time of the passes (a wait after each), and the
timings add ` ntsc_gpu_ms=<sum of the passes>` and ` ntsc_gpu=<pass>:<ms>,...`.

### Phase 4 numbers (RTX 5090 + Ryzen 9 9950X3D, 2026-11-12)

**Pixels.** Nothing changes by design, and nothing changed: every self-golden baseline passes unchanged
(`ctest` with `NTSCRT_GOLDEN_STRICT=1`, none regenerated); `bench_transfer --dump` (20 frames, the last one
compared) is byte-identical between `NTSCRT_NTSC_GPU=0` and the default for the house look, *Mild VHS*,
*Tracking Noise up*, *High Chroma static*, *Webcam (low quality)* (Digital video), `screenloop:7 copies slow
luma` and `screenloop:22 copies bottom left`, at 3840x2160 and 1920x1080, `rgba32f` and `rgba8` (28 of 28),
and the first five also against main's build before phase 4 (ntsc-rs; 20 of 20); in Resolve `--gpu-ab` (the
default route in both sessions, host GPU frames against CPU frames) 0 values differ on every checked image.

**Core** (`bench_transfer --sizes 3840x2160,1920x1080 --formats rgba32f,rgba8 --frames 100 --warmup 20`, the
CPU path (`ntscrt_render_cpu`, OpenFX layout), median of 3 runs of the per-frame medians, ms; "profiled" =
`NTSCRT_PROFILE=1`, a wait after each GPU phase and each NTSC pass; "tables" = `ntsc_tables_ms`, the CPU time
of the plan and its tables on their thread, while the picture converts / uploads beside them):

| Look | Size | Format | Frame, ntsc-rs | Frame, default | saved | `ntsc` phase, ntsc-rs / default | profiled frame, ntsc-rs / default | profiled `ntsc`, ntsc-rs / default | GPU time of the passes (profiled) | tables |
|---|---|---|---|---|---|---|---|---|---|---|
| house | 3840x2160 | rgba32f | 55.0 | **11.2** | 43.8 | 43.8 / 0.32 | 55.0 / 13.7 | 43.5 / 5.32 | 4.50 | 2.23 |
| house | 3840x2160 | rgba8 | 50.0 | **7.1** | 42.9 | 43.5 / 0.30 | 50.0 / 9.4 | 43.4 / 5.25 | 4.45 | 1.00 |
| house | 1920x1080 | rgba32f | 15.4 | **3.3** | 12.1 | 12.4 / 0.23 | 15.8 / 5.6 | 12.4 / 3.25 | 2.61 | 0.50 |
| house | 1920x1080 | rgba8 | 15.4 | **2.5** | 12.9 | 12.5 / 0.21 | 15.4 / 4.8 | 12.5 / 3.25 | 2.63 | 0.41 |
| Mild VHS | 3840x2160 | rgba32f | 39.9 | **9.4** | 30.5 | 28.7 / 0.09 | 40.5 / 10.4 | 29.0 / 2.21 | 1.73 | 0.06 |
| Mild VHS | 3840x2160 | rgba8 | 35.0 | **5.1** | 29.9 | 28.8 / 0.07 | 35.4 / 6.2 | 28.8 / 2.22 | 1.74 | 0.05 |
| Mild VHS | 1920x1080 | rgba32f | 11.5 | **2.5** | 9.0 | 8.4 / 0.06 | 11.6 / 3.7 | 8.3 / 1.20 | 0.98 | 0.04 |
| Mild VHS | 1920x1080 | rgba8 | 10.8 | **1.7** | 9.2 | 8.6 / 0.06 | 10.8 / 2.9 | 8.3 / 1.19 | 0.97 | 0.03 |
| Tracking Noise up | 3840x2160 | rgba32f | 54.8 | **11.2** | 43.6 | 43.5 / 0.33 | 55.5 / 13.7 | 43.7 / 5.34 | 4.50 | 2.23 |
| Tracking Noise up | 3840x2160 | rgba8 | 49.9 | **7.1** | 42.8 | 43.4 / 0.33 | 50.1 / 9.3 | 43.4 / 5.20 | 4.44 | 1.05 |
| Tracking Noise up | 1920x1080 | rgba32f | 15.6 | **3.3** | 12.3 | 12.6 / 0.23 | 16.3 / 5.5 | 12.7 / 3.20 | 2.60 | 0.47 |
| Tracking Noise up | 1920x1080 | rgba8 | 15.0 | **2.5** | 12.5 | 12.4 / 0.21 | 16.3 / 4.8 | 12.5 / 3.24 | 2.60 | 0.39 |
| Webcam (low quality) (Digital, NTSC off) | 3840x2160 | rgba32f | 19.4 | 19.2 | - | - | 19.7 / 19.6 | - | - | - |
| Webcam (low quality) | 3840x2160 | rgba8 | 16.2 | 16.0 | - | - | 16.5 / 16.5 | - | - | - |
| Webcam (low quality) | 1920x1080 | rgba32f | 6.4 | 6.2 | - | - | 6.5 / 6.5 | - | - | - |
| Webcam (low quality) | 1920x1080 | rgba8 | 5.9 | 5.9 | - | - | 6.1 / 6.0 | - | - | - |

Every NTSC frame of the default rows took the GPU route (100/100), the Digital look none (NTSC off: unchanged,
as expected). Against phase 3's opt-in route (13.0 / 9.0 / 3.9 / 3.2 ms for the house look) the default is
faster again because NTSC no longer keeps the frame off the direct source: the host picture is converted
straight into upload memory (no CPU picture, no separate upload). Where the UHD house-look frame's 11.2 ms go
now (unprofiled phases): float -> 8-bit conversion into upload memory 2.1, NTSC submission 0.3, readback (with
the wait for the GPU: NTSC ~3 ms, Downscale, Glitch, CRT) 4.4, half-float -> float conversion out 4.3: the CPU
path's float conversions are most of what is left, which host GPU frames remove (in Resolve below).

**Screen Loop** (`bench_screenloop --play 60 --seek 600`, the default look, a moving 8-bit source generated per
fetch; median of 3 runs; "seek" = a fresh instance rendering frame 600 cold: the run-up and the window's
history, 360 source fetches of which the bench's own source generation takes 1.4 s at 1080p, 4.2-6.7 s at
UHD):

| Preset | Size | play, ms/frame: ntsc-rs / default | first frame (run-up), ms | seek to 600, ms | next frame after the seek, ms |
|---|---|---|---|---|---|
| 7 copies slow luma | 1920x1080 | 18.2 / **4.0** | 1756 / 779 | 6333 / **3208** | 19.8 / 5.3 |
| 7 copies slow luma | 3840x2160 | 62.6 / **12.4** | 5779 / 1050 | 24846 / **10032** | 66.9 / 14.0 |
| Bottom to top | 1920x1080 | 18.2 / **3.9** | 1307 / 681 | 5798 / **3121** | 19.3 / 5.4 |
| Bottom to top | 3840x2160 | 60.7 / **12.3** | 3818 / 884 | 22600 / **9891** | 60.9 / 14.0 |

**Resolve** (docs/dev/HOST-GPU.md "Speed with NTSC on the GPU": 300-frame render-queue jobs, RTX 5090, CUDA
mode, 3 runs): the default look at UHD **17.87 s -> 2.84 s** per job (51.7 -> 1.6 ms per frame over the
2.35 s no-effect job; the plug-in's own time 50.5 -> 3.2 ms), *Mild VHS* 13.37 -> 2.85 s, at 1080p 6.85 ->
2.35 s (= *Clean CRT*); with CPU frames (`NTSCRT_OFX_GPU=0`, NTSC still on the GPU) 8.36 s at UHD. Every
render took `gpu-direct` with `ntsc_route=gpu`.


## What's next

- **Metal.** Still off. Verify the bit parity with `fma()` on Apple GPUs; Slang ignores `precise` on Metal, so
  check whether `-fno-fast-math`-style compile options are needed for the `precise` sums (phase 1's report-only
  numbers: max 1 level, 0.19% of values different), and run `test_ntsc_gpu -s` (or with
  `NTSCRT_NTSC_GPU_VERBOSE=1`) on a Mac for the phase 2-3 stages' numbers. Then the rule's Metal line goes.
- **Other GPUs.** Only the RTX 5090 (fused `mad`) and WARP (`double`) are measured. AMD and Intel GPUs fuse
  `mad` too as far as known, which the probe checks per device, but the parity tests have not run on them.
- **Performance.** All 22 stages take ~3 ms of GPU time at UHD on the RTX 5090; the IIR filters (one thread
  per row) are most of it. The phase-1 ideas (groupshared row tiles, fusing consecutive filters on a plane)
  still apply; the shifts each copy their plane to the scratch plane first (~0.03 ms each at UHD), which a
  kernel that reads the row into groupshared memory could avoid. The CPU tables (0.3-1.5 ms at UHD, snow the
  expensive one) are hidden behind the upload / convert; with heavy snow they could become the frame's
  critical path.
- Every change: extend `test_ntsc_gpu`'s cases, keep the tolerance rule per case, keep `NTSCRT_NTSC_GPU=0`
  byte-identical to ntsc-rs, and add the CHANGELOG line.
