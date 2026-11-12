# NTSC on the GPU (ntsc-rs port)

Status: **phase 1 of 4**, opt-in (`NTSCRT_NTSC_GPU=1`), Windows / D3D11 only. The Rust path (ntsc-rs through
`rust/ntscrs-capi`) stays the reference and the fallback forever.

At UHD the ntsc-rs step was ~38-48 ms of a ~55 ms frame and everything on the GPU ~2.5 ms. ntsc-rs has no GPU
implementation, so this is a port of its effect, pass by pass, to compute shaders, with ntsc-rs's arithmetic:
on the GPUs it targets the port's output is **bit-identical** to ntsc-rs's (measured, below).

| Phase | Scope |
|---|---|
| 1 (this) | Scaffolding, plane format, the general row IIR filter, CPU-made per-row tables, per-frame fallback, parity harness; every pass that is a row filter or per pixel / per row, **including the mandatory chroma modulation and all four demodulation filters**. |
| 2 | Composite / luma / chroma noise (Fbm simplex on the GPU from per-row seeds), snow (speckle event lists). |
| 3 | The VHS passes: head switching, tracking noise, chroma delay (`shift_row`), edge wave. |
| 4 | Screen Loop (the `howl` path's NTSC passes), host-GPU frames (`gpu-direct` with NTSC on), Metal. |

## Files

| File | What |
|---|---|
| `rust/ntscrs-capi/src/gpu_plan.rs` | The plan exports: settings -> pass list, coefficients, row tables (ntsc-rs's own code or exact copies of it). |
| `rust/ntscrs-capi/include/ntscrs_capi.h` | Their C declarations (`NtscGpu*` structs, `ntsc_gpu_plan`, `ntsc_gpu_row_seeds`, `ntsc_debug_process_yiq`, ...). |
| `core/stages/ntsc/gpu/ntsc_gpu.h/.cpp` | `NtscGpu` (the driver: textures, dispatches), `NtscGpuPlan`, the route rule. |
| `core/stages/ntsc/gpu/shaders/ntsc_gpu.slang` | The kernels. |
| `core/stages/ntsc/ntsc.cpp` | `NtscStage::gpu_plan()` / `debug_planes()`: the same settings handling as `process()`. |
| `core/src/ntscrt.cpp` `render_once` | The pipeline route. |
| `tests/ntsc/test_ntsc_gpu.cpp` | Parity harness, coverage report, fallback tests, GPU timing. |

## Data flow

```
render_once (no Screen Loop, NTSCRT_NTSC_GPU set)
  ntsc_gpu_route(dev)            env var, D3D11, not a software device  -> else Rust path
  NtscStage::gpu_plan(ctx, w, h) -> shim ntsc_gpu_plan: NtscGpuPlanInfo + passes + rows + out rows
  plan.supported()               every enabled stage implemented        -> else Rust path
  upload(img)                    the 8-bit picture after Digital (BGRA8)
  NtscGpu::run(tex, plan)        from_bgra8 -> pass kernels -> to_bgra8 -> BGRA8 texture
  -> Downscale -> Glitch -> CRT  (no CPU NTSC step, no second upload)
```

The decision is per frame. The timings line (`ntscrt_last_timings`, the OFX debug log) gets
` ntsc_route=gpu` or ` ntsc_route=cpu (<why>)` appended, only when the variable is set.

## Plane format

Y, I, Q: three `R32F` textures of `width x plane_rows`, plus a fourth `R32F` scratch plane (ntsc-rs's
`YiqView::scratch`: the modulated signal during demodulation, a plane copy for the vertical blend). They are
bound as `RWTexture2D<float>` u0..u3 for every kernel (R32F is the one format D3D11 FL11_0 / Metal tier 1 can
read from a writable texture) and pooled per stage (`NtscGpu::Impl::acquire`, released planes reused next frame).

`plane_rows` and the row order are exactly ntsc-rs's YIQ buffer (`yiq_fielding.rs`): `Upper` = ceil(h/2) rows
(image rows 0, 2, ...), `Lower` = max(h/2, 1) rows (1, 3, ...), `Both` = h rows, `InterleavedUpper` = the upper
field's rows then the lower field's (h rows), `InterleavedLower` the other way round. `Alternating` is Upper or
Lower by frame parity. Each field of an interleaved frame runs the effect with its own frame number (2f, 2f+1),
exactly as `apply_effect_to_all_fields`; on the GPU both fields go through each pass in one dispatch, and what
differs per field lives in the row table. All six `use_field` modes are implemented and tested.

## Tables (CPU -> GPU)

Everything random, 64-bit or `f64`, and every transcendental function, is computed on the CPU by the shim,
with ntsc-rs's code: ntsc-rs keeps `filter`, `random` and `VHSTapeSpeed::filter_params` private, so
`gpu_plan.rs` carries exact copies (same Rust, same operation order, same crates: `siphasher`, `rand`,
`rand_xoshiro` at ntsc-rs's versions), each naming the lines it reproduces. `process_impl` and the plan share
`effect_for()`, so both start from the same `NtscEffect`. Layouts below are C (`ntscrs_capi.h`), mirrored with
`static_assert`s in `ntsc_gpu.h`.

**`NtscGpuPlanInfo`** (72 bytes): `seed`, `field_frame[2]`, `field` (0 Upper .. 4 InterleavedLower),
`plane_rows`, `num_fields`, `field_first[2]`, `field_rows[2]`, `num_passes`, `unsupported` and `enabled` (bit
per `NTSC_ST_*` stage), `horizontal_scale`, `vertical_scale`.

**`NtscGpuPass`** (224 bytes), in `apply_effect_to_yiq_field` order: `stage` (`NTSC_ST_*`), `kind`
(`NTSC_GPU_K_*`), `planes` (bit 0 Y, 1 I, 2 Q), `mode`, `a`, `b`, `filter[3]` (one per plane):

| kind | what | parameters |
|---|---|---|
| `FILTER` | row IIR on `planes` | `filter[p]` |
| `LUMA_BOX` | input luma box filter | - |
| `CHROMA_INTO_LUMA` | modulation | row `xi` |
| `DEMOD` | `luma_into_chroma` | `mode` 0 box, 1 notch (`filter[0]`), 2 one-line comb, 3 two-line comb |
| `ROTATE_CONST` | chroma phase error | `a` = sin, `b` = cos |
| `ROTATE_ROWS` | chroma phase noise | row `phase_sin/cos` |
| `CHROMA_LOSS` | VHS chroma loss | row flag |
| `VERT_BLEND` | chroma vertical blend | per field |

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

**`NtscGpuRowSeed`** (16 bytes, one per plane row, phase 2; implemented: `ntsc_gpu_row_seeds`): `seed_lo/hi`
(the row's Xoshiro seed `Seeder(seed).mix(pass).mix(frame).mix(row)`), `noise_seed` (`rng.next_u32()`, the Fbm
seed) and `offset` (`rng.random::<f32>() * width`): everything `video_noise_line` draws before the noise itself,
for `composite_noise` (pass 0) and `plane_noise` (1 chroma I, 9 chroma Q, 10 luma). Tracking noise uses the
same function with `seeder.mix(2)` and its own row index (`index + cut_off_rows`): phase 3 adds that.

**`NtscGpuRowShift`** (16 bytes, one per plane row, phase 3, format fixed, export a stub): `shift` (pixels,
`shift_row` semantics), `boundary` (0 constant 0, 1 extend), `copy_start` (head switching's mid line: first
pixel taken from the shifted row; -1 whole row), `transient_intensity`.

**`NtscGpuSpeckle`** (16 bytes per transient, phases 2-3, format fixed, export a stub): `row`, `x0`, `count`,
`value_offset` into an `f32` array of the additive values. `row_speckles` draws a geometric distribution and a
fresh RNG per transient (`rng.jump()`) and an `f64` logistic: all of it, and the `cos * (1 - x/len)^2 * rand`
product per pixel, is computed on the CPU, so the GPU only adds `values[value_offset + i]` at `x0 + i`. Snow is
sparse (intensity ~1e-4 .. 1e-2 per pixel), so the lists are small.

## Arithmetic: why the GPU matches bit for bit

ntsc-rs runs its filters through `fearless_simd`: `filter_signal_in_place_fixed_size_simdx4` (every filter of
2-4 coefficients, i.e. all of them) uses `mul_add`, a real FMA on AVX2+FMA (x86-64-v3) and NEON, and so do
`rgb_to_yiq` / `yiq_to_rgb`. Everything else is plain scalar Rust (never contracted). The kernels follow that
operation by operation:

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

`mad()` is "fused or not, implementation-defined" in HLSL. `NtscGpu` probes it once per device
(`probe_fma`: `mad(1 + 2^-13, 1 + 2^-13, -(1 + 2^-12))` is 2^-26 only when fused). NVIDIA (and AMD, Apple)
fuse: the `mad` kernels are used. WARP doesn't: the `*_x` variants of the three kernels with multiply-adds
compute them in double precision (`double(a) * double(b)` is exact, the sum rounds to double then to float: a
double rounding that differs from a true FMA only when the double lands exactly halfway between two floats).
`NtscGpu::fma_mode()` says which ("mad", "double", or "mad-unfused" on a device with neither). Note that
ntsc-rs itself is only as exact as its CPU: on a pre-AVX2 x86 (fearless_simd's SSE4.2 level) `mul_add` is a
separate multiply and add, and the GPU would then differ from it by the amounts WARP showed before the probe
(below).

**Measured** (`test_ntsc_gpu`, 4 test pictures, 720x480 + 333x247 + 5x6 + a 0.5 render scale + 1920x1080 and
1280x720 for the preset case, frames 0/1/2/7/30, two seeds each):

| Device | Every pass alone (34 cases) | All passes together, 6 field modes x 4 sizes | Stage-by-stage YIQ planes | Preset *Mild VHS* |
|---|---|---|---|---|
| RTX 5090 (`mad`, fused) | max 0, 0 values differ | max 0, 0 differ | max abs diff 0 at every stage | max 0, 0 differ |
| WARP (`double`) | max 0, 0 differ | max 0, 0 differ | 0 | max 0, 0 differ |
| WARP with plain `mad` (not fused; before the probe) | max 1-3 levels, 0.0005-5.6% differ | - | 1e-7 .. 1.3e-5 (ringing's gain) | - |

Through the whole pipeline (`bench_transfer --dump`, `ntsc_gpu_passes` look with CRT, 3840x2160 and
1920x1080, 8-bit and float frames) the output with `NTSCRT_NTSC_GPU=1` is byte-identical to the output
without it; and the `default`, `clean_crt` and `ntsc_off` looks render byte-identically to the parent commit
(9221271) with the variable unset and with it set to 1 (fallback).

The tolerance in the tests is the HOST-GPU.md rule (each channel within 1/255, at most 0.2% of values
different), the same for every case; it holds with room to spare because the result is exact. The unfused
row shows why the probe exists: ntsc-rs truncates to 8 bits (no rounding), so a float error of one ulp flips a
level wherever a value sits on an exact level (flat areas of 8-bit sources do), and the near-Nyquist
Butterworth lowpass (`chroma_lowpass` *Light* at the house bandwidth scale 0.3: the cutoff clamps to the
sampling rate, both poles at z = -1) amplifies rounding differences along the row.

## The fallback rule

A frame takes the GPU route only when all of these hold, else it runs the Rust path exactly as without the
variable (byte-identical: tested):

1. `NTSCRT_NTSC_GPU` is `1` (or `force`); unset or `0` = off (the default in this phase: coverage is partial).
2. The device is D3D11. **Metal is off whatever the variable**: the kernels compile for Metal (CI builds the
   library: proof it compiles), but nothing has run there, and Metal compiles with fast math (Slang drops
   `precise` for Metal with a warning), which the bit parity above relies on. Phase 4 verifies it (Metal's
   `fma()` is fused, so `ExactFma` already maps to it).
3. Not a software device (WARP: vendor 0x1414 / device 0x8c), unless `NTSCRT_NTSC_GPU=force`: on WARP the
   kernels are ~2-3x slower than ntsc-rs itself (720x480: 6-8 ms vs 2-3 ms per frame).
4. Every stage the frame's settings turn on is implemented (`NtscGpuPlan::supported`: `info.unsupported == 0`);
   the reason names the stages (`not on the GPU yet: composite_noise,head_switching`).
5. Not the Screen Loop path (phase 4), the plan didn't throw.

One case is left to ntsc-rs on purpose although the GPU could do it: `chroma_vert_blend` on a field of one row
(pictures 1-3 rows tall, depending on the field mode). ntsc-rs slices two rows of that field's scratch buffer
(`ntsc.rs` line 1123) and panics, which fails the frame (`ntsc: frame processing failed (2)`); the plan marks
it unsupported so both routes behave alike. (An ntsc-rs bug; harmless at real sizes.)

## Coverage

Every ntsc-rs setting and where it runs on the GPU:

| Setting (preset key) | GPU |
|---|---|
| `random_seed`, the host's seed offset | now (CPU tables) |
| `use_field` (all 6 modes) | now |
| `filter_type` (constant-k, Butterworth) | now |
| `input_luma_filter` (none, box, notch) | now |
| `chroma_lowpass_in` (none, light, full) | now |
| `composite_preemphasis` (composite sharpening) | now |
| `video_scanline_phase_shift`, `_offset` | now |
| chroma modulation / `chroma_demodulation` (box, notch, 1-line comb, 2-line comb) | now (always runs) |
| `luma_smear` | now |
| `ringing` (`ringing_frequency`, `_power`, `_scale`) | now |
| `chroma_phase_error` | now |
| `chroma_phase_noise_intensity` | now |
| `vhs_settings` / `vhs_tape_speed` (SP, LP, EP filters and chroma delay) | now |
| `vhs_chroma_loss` | now |
| `vhs_sharpen_enabled` (`vhs_sharpen`, `_frequency`) | now |
| `vhs_chroma_vert_blend` | now |
| `chroma_lowpass_out` | now |
| `scale_settings` (`bandwidth_scale`, `vertical_scale`, `scale_with_video_size`), render scale | now (CPU side) |
| `composite_noise` (`_intensity`, `_frequency`, `_detail`) | phase 2 |
| `snow_intensity`, `snow_anisotropy` | phase 2 |
| `luma_noise` (`_intensity`, `_frequency`, `_detail`) | phase 2 |
| `chroma_noise` (`_intensity`, `_frequency`, `_detail`) | phase 2 |
| `head_switching` (`_height`, `_offset`, `_horizontal_shift`, `_start_mid_line`, `_mid_line_position`, `_mid_line_jitter`) | phase 3 |
| `tracking_noise` (`_height`, `_wave_intensity`, `_snow_intensity`, `_snow_anisotropy`, `_noise_intensity`) | phase 3 |
| `chroma_delay_horizontal`, `chroma_delay_vertical` | phase 3 |
| `vhs_edge_wave_enabled` (`vhs_edge_wave`, `_speed`, `_frequency`, `_detail`) | phase 3 |
| any of the above inside Screen Loop | phase 4 |

The house default look (no preset) uses head switching, tracking noise, composite / luma / chroma noise,
chroma delay and edge wave: it stays on ntsc-rs until phase 3.

Bundled presets (the 29 looks in `presets/`, 25 with NTSC on; `test_ntsc_gpu` prints this with
`NTSCRT_NTSC_GPU_VERBOSE=1`) that run entirely on the GPU:

| After phase | Presets |
|---|---|
| 1 | *Mild VHS* (1 of 25) |
| 2 | *Mild VHS* (every other preset also uses a phase-3 stage) |
| 3 | all 25 |
| 4 | the 5 Screen Loop presets too |

## Speed

`bench_transfer --formats rgba32f --looks ntsc_gpu_passes --frames 100 --warmup 20` (the default look with the
stages the GPU doesn't have yet turned off: 14 stages, every implemented one), RTX 5090 + Ryzen 9 9950X3D,
median of 3 runs of the per-frame medians, ms:

| Size | Route | Frame | `ntsc` phase | Frame, `NTSCRT_PROFILE=1` | `ntsc` phase, profiled |
|---|---|---|---|---|---|
| 3840x2160 | ntsc-rs | 56.9 | 43.2 | 59.8 | 39.6 |
| 3840x2160 | GPU | 18.7 | 0.2 (submission; the GPU work is waited for in `readback`, 9.7) | 15.4 | 3.5 |
| 1920x1080 | ntsc-rs | 16.7 | 12.6 | 22.5 | 12.3 |
| 1920x1080 | GPU | 5.2 | 0.1 | 5.7 | 1.35 |

Profiled phases of a GPU-route UHD frame (run 1): in 2.9, ntsc 3.5 (plan + tables on the CPU, the kernels),
upload 2.3, downscale + CRT 0.9, readback 0.6, out 4.6. The rest of the frame is now the float conversions
in and out (Resolve's float frames; `gpu-direct` host frames remove them in phase 4).

The GPU NTSC alone (`test_ntsc_gpu` "GPU time per stage": 30 frames queued, best of 3) is ~3.4 ms per UHD
frame and ~1.3 ms per 1080p frame for all 14 stages (the conversions and modulation alone ~0.6 / 0.3 ms;
each further stage ~0.1-0.4 ms at UHD, the measurements' noise is about +-0.2 ms).

On WARP (`NTSCRT_D3D11_WARP=1`, `NTSCRT_PROFILE=1`, 10 frames): `ntsc` 11.9 ms on ntsc-rs vs 27.6 ms on the
GPU route (`force`) at 1920x1080, 3.3 vs 7.8 ms at 720x480: hence the software-device rule.

One thread per row is enough so far but is the cost: a UHD interleaved frame has 2160 rows, i.e. 2160 threads
per filtered plane (68 warps on a GPU with 170 SMs), each walking 3840 samples. The arithmetic chain (four
dependent FMAs per sample, ~80k cycles per row) is far below the time measured; it is the memory latency
per sample that the few warps can't hide. The row filter therefore loads the next 16 samples while it filters
the current 16 (5.0 -> 3.4 ms per UHD frame). Splitting rows into blocks with carried state would break the
bit parity (a block's start state computed another way rounds differently). If later phases need more speed,
keep each row serial and make the memory access cheaper instead: a thread group stages a tile of rows x 64-128
columns in groupshared memory with coalesced loads, each thread filters its row in the tile, the tile goes back
coalesced; or fuse consecutive filters on the same plane into one kernel (one read and write per sample for
several filters).

## Debugging a parity failure

1. `test_ntsc_gpu` with `NTSCRT_NTSC_GPU_VERBOSE=1` prints a `PARITY` line per case (max level difference,
   mean, share of values different / more than one level different) and `STAGE` lines: the YIQ planes after
   each stage, ntsc-rs (`ntsc_debug_process_yiq` with the stages so far) against the GPU (the plan built with the
   same `stage_mask`), max abs difference per plane. The first stage that isn't 0 is the culprit; stages are
   compared as prefixes, so a later stage can't hide an earlier one.
2. `NTSCRT_NTSC_GPU_DUMP=<dir>` writes both sides' planes per stage
   (`<case>.<n>.<stage>.{rust,gpu}.f32`, Y then I then Q, `width x plane_rows` floats each): diff them to see
   where in the picture (a column = an IIR boundary or delay; a row = fielding or a row table; everything =
   coefficients or initial conditions).
3. `NtscStage::gpu_plan(ctx, w, h, plan, stage_mask)` and `NtscGpu::planes(tex, plan, num_passes)` take any
   subset of stages / any prefix of the GPU passes; `NtscStage::debug_planes` is ntsc-rs with a stage subset.
   The two modulation stages always run in ntsc-rs (they can't be turned off), so they are in every mask.
4. Run on WARP (`NTSCRT_D3D11_WARP=1`, CI's device) to see a non-fusing device (it takes the `double`
   kernels); differences of a level in flat areas with 1e-7-sized plane differences mean a multiply-add
   that isn't fused where ntsc-rs fuses (or the reverse).
5. Check the CPU you compare against: ntsc-rs fuses only on AVX2+FMA / NEON (`fearless_simd::Level`).

## What phases 2-4 must do

- **Phase 2 (noise).** Port `Fbm` / `Simplex1d` (`noise.rs`, `noise/simplex.rs`) to Slang: per row the seed
  and offset come from `ntsc_gpu_row_seeds`; check whether ntsc-rs's noise sampling uses `mul_add` (SIMD
  `sample_noise_1d`) and mirror it; noise uses `floor` and hashing, so float-exactness of the gradient math
  matters (compare with the stage-by-stage mode first). Snow: implement `ntsc_gpu_speckles` (CPU event lists,
  values computed on the CPU) and a kernel that adds them. Add the stages to `GPU_IMPLEMENTED` and to the
  coverage table; *Mild VHS* is still the only preset that is complete after phase 2.
- **Phase 3 (VHS).** `shift_row` / `shift_row_to` (`shift.rs`: linear interpolation with SIMD, boundary
  handling) as a per-pixel kernel from a row-shift table (`ntsc_gpu_row_shifts`): head switching (incl. the
  mid line copy and transient), tracking noise (shift from a CPU-sampled simplex row, plus its noise and
  speckles via the phase-2 machinery), edge wave (`sample_noise_2d`, CPU-side, one value per row), chroma
  delay. Chroma delay's **vertical** shift reads `src` rows from another row of the same plane (copy from
  scratch, like the vertical blend; note ntsc-rs's negative-offset branch reads row `offset - 1` for every
  destination row, which looks like an upstream bug: reproduce it, don't fix it). Tracking noise / head
  switching only touch the bottom rows of each field: the row table already says which field a row is in.
  The comb filters already read vertical neighbours within a field; any new vertical pass must use
  `field_first` / `field_rows` the same way (interleaved fields are separate pictures in ntsc-rs).
- **Phase 4.** The Screen Loop path (`h.chain_input` / `chain_input_begin` in `render_once`: today download ->
  ntsc-rs -> upload; with the GPU, NTSC on the camera texture directly, no download, no worker threads),
  host-GPU frames (`host_gpu.cpp`: NTSC on => `gpu-download` today; with GPU NTSC the picture can stay on the
  GPU: `gpu-direct`), and Metal (verify the bit parity with `fma()`; Slang ignores `precise` on Metal, so check
  whether `-fno-fast-math`-style compile options are needed for the `precise` sums). Then the default could
  flip to on.
- Every phase: extend `test_ntsc_gpu`'s cases, keep the tolerance rule per case, keep `NTSCRT_NTSC_GPU` unset
  = byte-identical, and add the CHANGELOG line.
