# Digital video on the GPU (the full-size steps)

Status: **on by default** on Windows / D3D11 hardware devices ([the rule](#the-rule)), Metal compiled and off.
The Digital video stage's two full-size steps, the capture scale-down and the upscale back to the frame's size,
run as compute kernels, **byte-identical** to the CPU code; the camera, the codecs and the chroma upsample stay
on the CPU, at the capture size, unchanged (1:1 with upstream's Swift). With host GPU frames a Digital look no
longer downloads the full picture: it is `path=gpu-direct`, and only the capture-size picture (e.g. 320x180)
crosses to the CPU and back. `NTSCRT_DIGITAL_GPU=0` brings back the CPU code, which stays the reference and the
per-frame fallback.

## The profile first

Before any change, `NTSCRT_PROFILE=1 bench_transfer --looks digital:<format>,preset:<look>` with the step clock
this work added (`digital_ms=` in the timings note: `Renderer::Profile`). Means per frame, ms, RTX 5090 + Ryzen 9 9950X3D, the same synthetic source every frame (the bench's), 60 fps project (so a 15 fps format codes one frame in
four; the means include the frames that code nothing). Raw: `profile_before*.md` (see [Results](#results)).

UHD (1080p in the raw files). `median` is the whole frame's median, `digital` the stage's phase, `full upload` the full CPU picture's upload, `ntsc` the NTSC stage (on the GPU; the `digital:` looks are the default look, NTSC on, plus the format; the presets have NTSC off).

| case | median | conv_in | digital | scale | download | earlier | camera | codec | deblock | upsample | upload | upscale | full upload | ntsc | gpu |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 3840x2160 rgba32f default | 13.86 | 3.12 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.20 | - |
| 3840x2160 rgba32f digital:webcam | 26.18 | 2.83 | 9.78 | 9.06 | 0.00 | 0.00 | 0.30 | 0.06 | 0.00 | 0.04 | 0.00 | 0.61 | 2.33 | 5.35 | 0 |
| 3840x2160 rgba32f digital:video_call | 26.04 | 2.87 | 9.92 | 9.04 | 0.00 | 0.00 | 0.21 | 0.23 | 0.01 | 0.04 | 0.00 | 0.66 | 2.14 | 5.36 | 0 |
| 3840x2160 rgba32f digital:minidv | 33.79 | 3.20 | 16.93 | 12.71 | 0.00 | 0.00 | 1.66 | 0.35 | 0.00 | 0.22 | 0.00 | 0.57 | 2.35 | 5.39 | 0 |
| 3840x2160 rgba32f digital:early_web | 26.18 | 2.87 | 9.88 | 9.11 | 0.00 | 0.00 | 0.05 | 0.14 | 0.00 | 0.04 | 0.00 | 0.63 | 2.28 | 5.32 | 0 |
| 3840x2160 rgba32f digital:cd_rom | 25.35 | 2.82 | 9.05 | 8.34 | 0.00 | 0.00 | 0.03 | 0.03 | 0.00 | 0.02 | 0.00 | 0.61 | 2.33 | 5.37 | 0 |
| 3840x2160 rgba32f digital:datamosh | 28.41 | 3.04 | 11.98 | 10.29 | 0.00 | 0.00 | 0.18 | 0.54 | 0.00 | 0.09 | 0.00 | 0.62 | 2.23 | 5.35 | 0 |
| 3840x2160 rgba32f preset:Webcam (low quality) | 19.82 | 2.73 | 10.42 | 9.37 | 0.00 | 0.00 | 0.24 | 0.10 | 0.00 | 0.06 | 0.00 | 0.61 | 2.34 | 0.00 | 0 |
| 3840x2160 rgba32f preset:Cheap camcorder | 21.27 | 2.96 | 11.49 | 10.39 | 0.00 | 0.00 | 0.36 | 0.15 | 0.00 | 0.07 | 0.00 | 0.63 | 2.23 | 0.00 | 0 |
| 3840x2160 rgba32f preset:Chunky webcam | 19.97 | 2.68 | 10.43 | 9.32 | 0.00 | 0.00 | 0.24 | 0.35 | 0.00 | 0.06 | 0.00 | 0.60 | 2.30 | 0.00 | 0 |
| 3840x2160 rgba32f preset:CD-ROM (low quality) | 18.94 | 2.90 | 9.34 | 8.62 | 0.00 | 0.00 | 0.09 | 0.04 | 0.00 | 0.02 | 0.00 | 0.61 | 2.23 | 0.00 | 0 |
| 3840x2160 rgba8 default | 9.54 | 1.63 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.30 | - |
| 3840x2160 rgba8 digital:webcam | 21.57 | 1.16 | 9.96 | 9.15 | 0.00 | 0.00 | 0.30 | 0.06 | 0.00 | 0.03 | 0.00 | 0.57 | 2.35 | 5.32 | 0 |
| 3840x2160 rgba8 digital:video_call | 21.22 | 1.13 | 9.71 | 8.99 | 0.00 | 0.00 | 0.22 | 0.20 | 0.01 | 0.04 | 0.00 | 0.55 | 2.28 | 5.34 | 0 |
| 3840x2160 rgba8 digital:minidv | 28.12 | 1.27 | 16.18 | 12.73 | 0.00 | 0.00 | 1.67 | 0.37 | 0.00 | 0.24 | 0.00 | 0.58 | 2.39 | 5.33 | 0 |
| 3840x2160 rgba8 digital:early_web | 21.89 | 1.21 | 10.00 | 9.10 | 0.00 | 0.00 | 0.05 | 0.13 | 0.00 | 0.03 | 0.00 | 0.56 | 2.30 | 5.58 | 0 |
| 3840x2160 rgba8 digital:cd_rom | 20.85 | 1.17 | 9.18 | 8.47 | 0.00 | 0.00 | 0.03 | 0.03 | 0.00 | 0.02 | 0.00 | 0.57 | 2.35 | 5.43 | 0 |
| 3840x2160 rgba8 digital:datamosh | 23.83 | 1.20 | 12.15 | 10.24 | 0.00 | 0.00 | 0.24 | 0.61 | 0.00 | 0.08 | 0.00 | 0.59 | 2.32 | 5.37 | 0 |
| 3840x2160 rgba8 preset:Webcam (low quality) | 16.61 | 1.17 | 10.47 | 9.48 | 0.00 | 0.00 | 0.25 | 0.11 | 0.00 | 0.06 | 0.00 | 0.55 | 2.25 | 0.00 | 0 |
| 3840x2160 rgba8 preset:Cheap camcorder | 18.12 | 1.25 | 11.82 | 10.48 | 0.00 | 0.00 | 0.36 | 0.15 | 0.00 | 0.08 | 0.00 | 0.57 | 2.38 | 0.00 | 0 |
| 3840x2160 rgba8 preset:Chunky webcam | 16.98 | 1.16 | 10.68 | 9.46 | 0.00 | 0.00 | 0.25 | 0.34 | 0.00 | 0.06 | 0.00 | 0.53 | 2.19 | 0.00 | 0 |
| 3840x2160 rgba8 preset:CD-ROM (low quality) | 15.46 | 1.17 | 9.28 | 8.57 | 0.00 | 0.00 | 0.10 | 0.03 | 0.00 | 0.02 | 0.00 | 0.60 | 2.33 | 0.00 | 0 |

- The capture scale-down (`Renderer::scale`: separable Lanczos-5 over the full picture, then the aliasing's
  point samples, then One field) is **85-90 % of the stage**: 8.3-12.7 ms per UHD frame, 2.7-5.3 ms at 1080p,
  more the wider the capture (MiniDV's 720 columns).
- The upscale (nearest neighbour back to the frame's size) is 0.5-0.7 ms at UHD; the upload of the full CPU
  picture the stage forces (instead of the direct conversion into upload memory) another ~2.3 ms.
- The codec itself never dominates: `codec` (encode + decode) is at most 0.6 ms a frame (Datamosh), `camera`
  (sensor noise, CCD response, noise reduction, sharpening, Y'CbCr) at most 1.7 ms (MiniDV: 720x406 at 30 fps,
  a capture every other frame), deblocking ~0.01 ms, the chroma upsample (`render`, capture size) at most 0.24 ms.
  All capture-size work: it stays on the CPU (no shader port of the codec).

## What runs where

```
render_once, Digital video on, no Screen Loop (Digital is off under Screen Loop, as upstream)
  digital_gpu_on(inst)          the instance's route (gpu_route_on: decided once, again when NTSCRT_DIGITAL_GPU
                                changes or the device is replaced; off after 3 GPU errors in a row)
  NTSC on ntsc-rs?              then the CPU code (ntsc-rs needs the picture on the CPU anyway)
  the 8-bit picture on the GPU  direct (host GPU frame's convert kernel, or the CPU frame converted into upload
                                memory), as with NTSC on the GPU
  DigitalStage::process_gpu
    DigitalGpu::scale           scale_h (W x H -> cw x H, RGBA32F) + scale_v (-> cw x ch BGRA8: Lanczos rows,
                                aliasing, One field)  ->  download cw x ch x 4 bytes
    Renderer::code              the CPU code from here: stream / checkpoints / re-runs, capture() (camera),
                                codec::step / video1::step, render() (deblock, chroma upsample) -> cw x ch BGRA8
    DigitalGpu::upscale         upload cw x ch, upscale kernel (-> W x H BGRA8)
  -> NTSC on the GPU (on that texture) -> Downscale -> Glitch -> CRT
  a GPU error (not a lost device): DigitalStage::process on the CPU picture for this frame (the same frame:
  the codec's state is the same whether its step ran or not)
```

| | CPU code (`NTSCRT_DIGITAL_GPU=0`) | GPU route |
|---|---|---|
| capture scale-down (`Renderer::scale`) | CPU, full picture | kernels `scale_h`, `scale_v` |
| camera (`capture`), codec, deblock, chroma upsample (`render`) | CPU, capture size | the same CPU code (`Renderer::code`) |
| upscale (`Renderer::upscale`) | CPU, full picture | kernel `upscale` |
| full picture across the bus | CPU frames: up once; host GPU frames: down and up (`gpu-download`) | CPU frames: up once (straight into upload memory, as without Digital); host GPU frames: never (`gpu-direct`) |
| capture-size picture across the bus | — | down and up once per frame (230 KB at 320x180) |
| earlier frames (a re-run after a seek, Datamosh history) | fetched and scaled on the CPU | the same (CPU source frames; byte-identical scale, so the two can mix) |

Not moved, on purpose: the chroma upsample + Y'CbCr -> BGRA (`render`) works at the capture size (at most 0.24 ms)
and the codec's planes (three float planes) are more bytes to move than its BGRA result; the codec and camera
are capture-size and must stay 1:1 with upstream's Swift.

## Exactness

The target was the NTSC port's: byte-identical to the CPU code, and it is, for every step.

- **Arithmetic.** `renderer.cpp` builds with `/fp:precise` (`-ffp-contract=off`): every product and sum is
  rounded to float on its own, in a fixed order. In the kernels every value is `precise` and goes through
  `A.mul` / `A.add` (`IArith`): `FloatArith` where the device rounds a `precise` float product and sum to nearest
  and doesn't fuse them (the probe), `DoubleArith` (the `*_x` kernels) elsewhere: a float product is exact in a
  double, and a double sum rounded to float is the float sum (53 >= 2 x 24 + 2 bits), so it reproduces float
  arithmetic exactly whatever the device does with floats. The reverse of the NTSC port's lesson (there the CPU
  fuses, here it mustn't), with the same tools: `precise`, a probe, double variants.
- **The probe** (`probe_round`, once per device): `x*x - z` (a product halfway between two floats, rounded to
  even, not fused into the subtraction), `x*y - w` (rounded to nearest, not toward zero) and `(1 + v) - 1` (sums
  round to nearest), with the operands from the constant buffer. `float` when all three are right, else
  `probe_round_x` on the double kernels (`double`), else `inexact` (the route stays off unless `force`).
  Measured: RTX 5090 `float`, WARP `float`. `NTSCRT_DIGITAL_GPU_ARITH=double` forces the double kernels (the
  test runs them on both devices: also byte-identical).
- **No sin(), no division, no doubles in the plain kernels.** The Lanczos taps (`makeTaps`: sin, the
  normalisation division, double centres), the aliasing coordinates (doubles), its 3x3 mean's `/ 9` (a table of
  `float(s) / 9` for every sum) and the upscale maps (doubles) are made on the CPU by the same code the CPU path
  runs (`Renderer::scalePlan`, `ninths`, `upscaleMaps`) and uploaded as structured buffers when the sizes or
  the optics change (per axis: e.g. 320 x 122 weights for UHD -> 320).
- **Rounding.** `std::round` (half away from zero) is spelled out (`round_byte`: HLSL `round()` is half to
  even); the clamp to 0..255 as the CPU's. Bytes come out of UNORM8 texels as `round(v * 255)`.
- **Readable RW textures.** The kernels read only SRVs: the first pass writes its float rows to an RGBA32F
  texture that the second reads as a `Texture2D` (no typed UAV loads, so no R32F requirement arises).
- **Denormals.** D3D11 flushes them; none occur (the smallest Lanczos tails are ~1e-9, sums of whole numbers
  and of products of such).

Measured (`tests/digital/test_digital_gpu.cpp`): 220 scale cases on the RTX 5090 and 202 on WARP, plain and
double kernels, 0 bytes different; every upscale case 0; the pipeline (below) 0 bytes different on every frame
of every case, on the RTX 5090 and on WARP (`NTSCRT_D3D11_WARP=1`, `force`). Golden baselines are unchanged.

## The rule

`digital_gpu_route` (`core/stages/digital/gpu/digital_gpu.cpp`), decided per instance on its first Digital
frame like `ntsc_gpu_route`, with the same switch values and the same helpers (`core/gpu/gpu_route.h`:
`gpu_route_env`, `gpu_adapter_description`, `gpu_is_software`; `GpuRoute` / `gpu_route_on` /
`gpu_route_failed` in `core/src/ntscrt.cpp` for both stages):

1. `NTSCRT_DIGITAL_GPU=0`: the CPU code.
2. Not Metal, unless `force`: the kernels compile for Metal (CI builds the library) but the route is unverified
   there, and Metal compiles with fast math (Slang drops `precise`).
3. Not a software device (WARP), unless `force`: the CPU code is faster there.
4. Not where the probe says `inexact`, unless `force`.
5. Per frame: not when NTSC runs on ntsc-rs (`digital_route=cpu (ntsc-rs needs the picture on the CPU)`: the
   picture comes to the CPU anyway); not under Screen Loop (Digital video doesn't run there at all).

The timings line (`ntscrt_last_timings`, the OpenFX log) gets ` digital_route=gpu (auto: NVIDIA GeForce RTX 5090,
arith float)` or ` digital_route=cpu (<why>)` on every frame Digital video runs, then ` digital_ms=scale:..,
download:..,earlier:..,camera:..,codec:..,deblock:..,upsample:..,upload:..,upscale:.. digital_captures=N` (on the
GPU route `scale` / `upscale` are the kernels' submission, their GPU time with `NTSCRT_PROFILE=1`, and `download`
includes the wait for them). The OpenFX plug-in logs `instance #N Digital video route: <route> (<why>)` when an
instance's route text changes. With host GPU frames the route shows in the path: `gpu-direct` on the GPU route,
`gpu-download` with the CPU code.

What falls back to the CPU code: the whole stage with `NTSCRT_DIGITAL_GPU=0`, on Metal, on WARP, on an
`inexact` GPU, with NTSC on ntsc-rs (`NTSCRT_NTSC_GPU=0`, or wherever NTSC's own rule picks ntsc-rs), without a
GPU device; a single frame on a GPU error (after 3 in a row, the instance); earlier frames for a re-run always
(their scale-down runs on the CPU from the source cache: byte-identical, so they mix with GPU frames).

## Files

| File | What |
|---|---|
| `core/stages/digital/gpu/shaders/digital_gpu.slang` | The kernels: `probe_round[_x]`, `scale_h[_x]`, `scale_v[_x]`, `upscale`. |
| `core/stages/digital/gpu/digital_gpu.h/.cpp` | `DigitalGpu` (tables, textures, dispatches, the probe), `digital_gpu_route`, `NTSCRT_DIGITAL_GPU`. |
| `core/stages/digital/renderer.h/.cpp` | `Renderer::code` (the CPU part between the pictures), `scalePlan`, `ninths`, `upscaleMaps`, `Profile`. |
| `core/stages/digital/digital.cpp` | `DigitalStage::process_gpu` (and `process`, from the same settings and capture size). |
| `core/gpu/gpu_route.h/.cpp` | The route switches' values and device facts, shared with the NTSC port. |
| `core/src/ntscrt.cpp` | `digital_gpu_on`, the pipeline (`render_once`: direct picture, the Digital texture, fallbacks, notes). |
| `tests/digital/test_digital_gpu.cpp` | Kernel parity (sizes, aliasing, fields, alpha, plain / double kernels, GPU and WARP), pipeline parity, the switch, GPU errors. |
| `tests/hostgpu/test_host_gpu.cpp` | Host GPU frames with Digital video: `gpu-direct`, 0 values different from the CPU path's CPU code. |
| `tests/howlaround/test_howl_seek.cpp` | A Digital look under Screen Loop: as without Digital, on either route, seeks included. |
| `tests/resolve/parity.py` | `--parity-digital`; `NTSCRT_DIGITAL_GPU=0` in the CPU sessions, the `digital_route` of every case checked. |

## Tests

- `test_digital_gpu` (ctest): the probe on each device; `DigitalGpu::scale` against `Renderer::scale` for 11 size
  pairs (UHD -> 320x180, 1080p -> 720x406 / 1280x720 / 240x136, 1280x720 -> 480x270, 333x197 -> 320x190, 161x97 ->
  320x194 (upsampling), 17x9 -> 16x16, 1x1 -> 16x16, 64x48 -> 64x48 (copy), 1001x3 -> 320x16), aliasing 0 / 0.2 /
  0.5 / 0.74 / 1, fields 0 / 1, opaque and random alpha, the plain and the double kernels, on this PC's GPU and on
  WARP; `upscale` for 6 size pairs. Pipeline: `ntscrt_render_cpu` on the GPU route against
  `NTSCRT_DIGITAL_GPU=0`, frames 0-5, 5 again, a seek back to 2, a jump to 30, 31, 32 (fetching earlier frames
  from a moving source): every Digital video format at 241x135 8-bit, 1920x1080 float with the CRT shader on and
  161x97 half; the 4 bundled Digital looks at 640x360 float; every Digital setting alone (24 values) and
  Downscale off (capture at the picture's size, capped at 1280); crop 4:3, render scale 0.5, NTSC on ntsc-rs
  (`digital_route=cpu`), Datamosh in long sequences with seeks (two keyframe intervals); GPU errors (the frame
  takes the CPU code, three in a row the instance; another switch value decides again); the switch's reasons.
  Byte equality everywhere; on Metal the kernel cases only report (route off there).
- `test_host_gpu`: 11 Digital cases (NTSC off / on the GPU, formats, *Webcam (low quality)* at 8-bit and float,
  crop, render scale, holes, a source smaller than the frame, `NTSCRT_DIGITAL_GPU=0`), 0 values different (but
  half-float output's rounding), each case's `path=` and `digital_route=` checked.
- `test_howl_seek`: a Digital look (aliasing, Datamosh) under Screen Loop renders what the look without Digital
  renders, played and after cold seeks, with `NTSCRT_DIGITAL_GPU=0` and unset; no `digital_route=` (the stage
  doesn't run).
- Resolve: `run_resolve_tests.py --headless --bundle <bundle> --parity --parity-digital` (docs/TESTING.md).

## Results

RTX 5090, Ryzen 9 9950X3D, 2026-11-12, branch `agent-z/digital-gpu`. Raw files (not in the repo):
`C:/ntb/w13z-results/` on the dev PC (`bench-core/`, `profile_*.txt`/`.md`, `resolve-bench-*/`, `parity-digital/`).

### Core: `bench_transfer`, CPU code vs GPU route

Three interleaved runs of `bench_transfer --sizes 3840x2160,1920x1080 --formats rgba32f,rgba8 --looks <these>
--frames 120 --warmup 20` with and without `NTSCRT_DIGITAL_GPU=0` (60 fps; the table is the median of the three medians): `NTSCRT_DIGITAL_GPU=0` (the CPU code, as `main` before this work) against the default.
`ntscrt_render_cpu`, ms per frame; `default` (no Digital) for reference. The `digital:` looks are the default
look (NTSC on, on the GPU) with the format applied; the presets have NTSC off. "GPU frames": frames of the 120
that logged `digital_route=gpu`.

| look | size | format | CPU code ms | GPU route ms | speed-up | GPU frames |
|---|---|---|---|---|---|---|
| default | 3840x2160 | rgba32f | 11.42 | 11.34 | 1.01x | - |
| default | 3840x2160 | rgba8 | 7.13 | 7.14 | 1.00x | - |
| default | 1920x1080 | rgba32f | 3.26 | 3.28 | 0.99x | - |
| default | 1920x1080 | rgba8 | 2.56 | 2.49 | 1.03x | - |
| digital:cd_rom | 3840x2160 | rgba32f | 22.41 | 12.16 | 1.84x | 120 |
| digital:cd_rom | 3840x2160 | rgba8 | 18.00 | 7.86 | 2.29x | 120 |
| digital:cd_rom | 1920x1080 | rgba32f | 7.08 | 3.74 | 1.90x | 120 |
| digital:cd_rom | 1920x1080 | rgba8 | 6.40 | 2.98 | 2.15x | 120 |
| digital:datamosh | 3840x2160 | rgba32f | 25.23 | 13.13 | 1.92x | 120 |
| digital:datamosh | 3840x2160 | rgba8 | 20.87 | 8.81 | 2.37x | 120 |
| digital:datamosh | 1920x1080 | rgba32f | 8.76 | 4.58 | 1.91x | 120 |
| digital:datamosh | 1920x1080 | rgba8 | 8.13 | 3.76 | 2.16x | 120 |
| digital:early_web | 3840x2160 | rgba32f | 23.25 | 12.30 | 1.89x | 120 |
| digital:early_web | 3840x2160 | rgba8 | 18.98 | 7.80 | 2.43x | 120 |
| digital:early_web | 1920x1080 | rgba32f | 7.52 | 3.82 | 1.97x | 120 |
| digital:early_web | 1920x1080 | rgba8 | 6.82 | 3.06 | 2.23x | 120 |
| digital:minidv | 3840x2160 | rgba32f | 30.17 | 15.38 | 1.96x | 120 |
| digital:minidv | 3840x2160 | rgba8 | 25.29 | 10.99 | 2.30x | 120 |
| digital:minidv | 1920x1080 | rgba32f | 12.43 | 6.66 | 1.86x | 120 |
| digital:minidv | 1920x1080 | rgba8 | 11.61 | 5.80 | 2.00x | 120 |
| digital:video_call | 3840x2160 | rgba32f | 23.34 | 12.27 | 1.90x | 120 |
| digital:video_call | 3840x2160 | rgba8 | 18.93 | 7.91 | 2.39x | 120 |
| digital:video_call | 1920x1080 | rgba32f | 7.53 | 3.79 | 1.98x | 120 |
| digital:video_call | 1920x1080 | rgba8 | 6.83 | 3.00 | 2.27x | 120 |
| digital:webcam | 3840x2160 | rgba32f | 23.32 | 12.14 | 1.92x | 120 |
| digital:webcam | 3840x2160 | rgba8 | 18.91 | 7.87 | 2.40x | 120 |
| digital:webcam | 1920x1080 | rgba32f | 7.56 | 3.81 | 1.98x | 120 |
| digital:webcam | 1920x1080 | rgba8 | 6.91 | 3.01 | 2.30x | 120 |
| preset:CD-ROM (low quality) | 3840x2160 | rgba32f | 18.08 | 7.70 | 2.35x | 120 |
| preset:CD-ROM (low quality) | 3840x2160 | rgba8 | 15.01 | 4.82 | 3.12x | 120 |
| preset:CD-ROM (low quality) | 1920x1080 | rgba32f | 5.46 | 2.26 | 2.42x | 120 |
| preset:CD-ROM (low quality) | 1920x1080 | rgba8 | 5.05 | 1.66 | 3.05x | 120 |
| preset:Cheap camcorder | 3840x2160 | rgba32f | 20.42 | 8.28 | 2.47x | 120 |
| preset:Cheap camcorder | 3840x2160 | rgba8 | 17.30 | 5.19 | 3.33x | 120 |
| preset:Cheap camcorder | 1920x1080 | rgba32f | 6.95 | 2.52 | 2.75x | 120 |
| preset:Cheap camcorder | 1920x1080 | rgba8 | 6.42 | 2.06 | 3.12x | 120 |
| preset:Chunky webcam | 3840x2160 | rgba32f | 19.59 | 8.46 | 2.32x | 120 |
| preset:Chunky webcam | 3840x2160 | rgba8 | 16.36 | 5.33 | 3.07x | 120 |
| preset:Chunky webcam | 1920x1080 | rgba32f | 6.58 | 2.76 | 2.39x | 120 |
| preset:Chunky webcam | 1920x1080 | rgba8 | 6.12 | 2.25 | 2.72x | 120 |
| preset:Webcam (low quality) | 3840x2160 | rgba32f | 19.29 | 8.30 | 2.32x | 120 |
| preset:Webcam (low quality) | 3840x2160 | rgba8 | 16.18 | 5.26 | 3.07x | 120 |
| preset:Webcam (low quality) | 1920x1080 | rgba32f | 6.43 | 2.70 | 2.39x | 120 |
| preset:Webcam (low quality) | 1920x1080 | rgba8 | 6.04 | 2.27 | 2.67x | 120 |

*Webcam (low quality)* at UHD float: 19.3 -> 8.3 ms (the default look: 11.3). The `digital:` looks are now 0.5-4 ms above the
default look they are built on (was 4-19 ms); the rest is the camera and codec (MiniDV's 720-wide capture
codes every other frame: ~2 ms a frame on average).

### Profile after (`NTSCRT_PROFILE=1`, the GPU route, means, UHD)

`scale` / `upscale` are now the kernels' GPU time; `download` / `upload` the capture-size picture's transfers
(`download` includes the wait for the kernels and for the frame's upload).

| case | median | conv_in | digital | scale | download | earlier | camera | codec | deblock | upsample | upload | upscale | full upload | ntsc | gpu |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 3840x2160 rgba32f default | 13.97 | 2.97 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.44 | - |
| 3840x2160 rgba32f digital:webcam | 15.16 | 3.01 | 1.41 | 0.89 | 0.10 | 0.00 | 0.36 | 0.06 | 0.00 | 0.03 | 0.01 | 0.19 | 0.00 | 5.08 | 60 |
| 3840x2160 rgba32f digital:video_call | 15.55 | 3.18 | 1.49 | 0.88 | 0.13 | 0.00 | 0.26 | 0.23 | 0.01 | 0.03 | 0.01 | 0.19 | 0.00 | 5.28 | 60 |
| 3840x2160 rgba32f digital:minidv | 18.47 | 3.17 | 4.36 | 0.95 | 0.29 | 0.00 | 1.69 | 0.35 | 0.00 | 0.23 | 0.06 | 0.26 | 0.00 | 5.31 | 60 |
| 3840x2160 rgba32f digital:early_web | 15.09 | 2.93 | 1.33 | 0.88 | 0.10 | 0.00 | 0.05 | 0.16 | 0.00 | 0.03 | 0.01 | 0.17 | 0.00 | 5.17 | 60 |
| 3840x2160 rgba32f digital:cd_rom | 15.25 | 3.07 | 1.24 | 0.91 | 0.11 | 0.00 | 0.03 | 0.04 | 0.00 | 0.03 | 0.01 | 0.17 | 0.00 | 5.33 | 60 |
| 3840x2160 rgba32f digital:datamosh | 16.04 | 2.94 | 2.23 | 0.88 | 0.14 | 0.00 | 0.21 | 0.60 | 0.00 | 0.10 | 0.02 | 0.21 | 0.00 | 5.17 | 60 |
| 3840x2160 rgba32f preset:Webcam (low quality) | 8.48 | 2.83 | 1.70 | 0.89 | 0.11 | 0.00 | 0.28 | 0.12 | 0.00 | 0.06 | 0.01 | 0.18 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba32f preset:Cheap camcorder | 8.50 | 2.85 | 1.33 | 0.89 | 0.13 | 0.00 | 0.41 | 0.17 | 0.00 | 0.08 | 0.02 | 0.18 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba32f preset:Chunky webcam | 9.09 | 2.94 | 1.82 | 0.90 | 0.11 | 0.00 | 0.29 | 0.39 | 0.00 | 0.06 | 0.01 | 0.19 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba32f preset:CD-ROM (low quality) | 8.12 | 2.88 | 1.21 | 0.92 | 0.10 | 0.00 | 0.11 | 0.04 | 0.00 | 0.03 | 0.01 | 0.17 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba8 default | 9.26 | 1.58 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 0.00 | 5.16 | - |
| 3840x2160 rgba8 digital:webcam | 10.56 | 1.67 | 1.21 | 0.88 | 0.10 | 0.00 | 0.33 | 0.06 | 0.00 | 0.04 | 0.01 | 0.16 | 0.00 | 5.15 | 60 |
| 3840x2160 rgba8 digital:video_call | 10.55 | 1.62 | 1.25 | 0.89 | 0.11 | 0.00 | 0.24 | 0.21 | 0.01 | 0.04 | 0.01 | 0.17 | 0.00 | 5.12 | 60 |
| 3840x2160 rgba8 digital:minidv | 13.28 | 1.67 | 3.86 | 0.93 | 0.28 | 0.00 | 1.72 | 0.38 | 0.00 | 0.21 | 0.05 | 0.24 | 0.00 | 5.13 | 60 |
| 3840x2160 rgba8 digital:early_web | 10.49 | 1.65 | 1.18 | 0.85 | 0.09 | 0.00 | 0.06 | 0.15 | 0.00 | 0.03 | 0.01 | 0.16 | 0.00 | 5.13 | 60 |
| 3840x2160 rgba8 digital:cd_rom | 10.42 | 1.57 | 1.24 | 0.94 | 0.09 | 0.00 | 0.03 | 0.03 | 0.00 | 0.03 | 0.01 | 0.15 | 0.00 | 5.11 | 60 |
| 3840x2160 rgba8 digital:datamosh | 11.39 | 1.63 | 2.16 | 0.93 | 0.14 | 0.00 | 0.25 | 0.64 | 0.00 | 0.09 | 0.02 | 0.21 | 0.00 | 5.05 | 60 |
| 3840x2160 rgba8 preset:Webcam (low quality) | 5.66 | 1.72 | 1.63 | 0.90 | 0.08 | 0.00 | 0.28 | 0.12 | 0.00 | 0.06 | 0.01 | 0.15 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba8 preset:Cheap camcorder | 5.53 | 1.69 | 1.37 | 0.89 | 0.12 | 0.00 | 0.42 | 0.17 | 0.00 | 0.09 | 0.02 | 0.17 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba8 preset:Chunky webcam | 5.59 | 1.63 | 1.67 | 0.87 | 0.08 | 0.00 | 0.28 | 0.35 | 0.00 | 0.06 | 0.01 | 0.15 | 0.00 | 0.00 | 60 |
| 3840x2160 rgba8 preset:CD-ROM (low quality) | 5.02 | 1.49 | 1.25 | 0.97 | 0.10 | 0.00 | 0.10 | 0.04 | 0.00 | 0.02 | 0.01 | 0.14 | 0.00 | 0.00 | 60 |

The capture scale-down is 0.85-0.95 ms of GPU time at UHD (was 8.3-12.7 ms of CPU time), the upscale ~0.01 ms
of submission (0.15-0.26 ms with the capture's upload), the full picture's separate upload is gone (`full upload` 0).

### Resolve: `run_resolve_tests.py --headless --only bench`

Edit page, one 300-frame render job per look (H.264, NVIDIA), host GPU frames (CUDA), `--bench-looks
default,webcam-lq,cheap-camcorder`; `NTSCRT_DIGITAL_GPU=0` session against the default session. Wall time of the
job, and the plug-in's own median / p95 ms per frame from its log.

| size | look | CPU code: job s | plug-in ms (median / p95) | path | GPU route: job s | plug-in ms | path |
|---|---|---|---|---|---|---|---|
| UHD | (no effect) | 2.86 | - | - | 2.86 | - | - |
| UHD | default | 3.34 | 2.9 / 3.7 | gpu-direct | 3.35 | 3.1 / 3.7 | gpu-direct |
| UHD | Webcam (low quality) | 7.35 | 16.9 / 18.1 | gpu-download | 2.86 | 3.2 / 3.9 | gpu-direct |
| UHD | Cheap camcorder | 7.86 | 17.3 / 18.7 | gpu-download | 2.84 | 3.2 / 4.3 | gpu-direct |
| 1080p | (no effect) | 1.36 | - | - | 1.35 | - | - |
| 1080p | default | 2.34 | 1.5 / 1.9 | gpu-direct | 2.35 | 1.4 / 1.8 | gpu-direct |
| 1080p | Webcam (low quality) | 4.35 | 6.7 / 7.4 | gpu-download | 2.35 | 2.2 / 2.9 | gpu-direct |
| 1080p | Cheap camcorder | 4.37 | 7.5 / 8.7 | gpu-download | 2.85 | 2.1 / 3.4 | gpu-direct |

Every frame of the GPU session logged `digital_route=gpu` (`Digital video route: gpu (auto: NVIDIA GeForce RTX
5090, arith float)`), of the CPU session `digital_route=cpu (NTSCRT_DIGITAL_GPU=0)`. A UHD render of *Webcam (low
quality)* is now as fast as the clip without the effect. (A `Digital video/<format>` look can't be benchmarked
this way: the dev copy's test preset file only takes bundled looks.)

### Resolve parity

PARITY_SECTION

