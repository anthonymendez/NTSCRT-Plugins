# Host GPU frames (OpenFX CUDA / Metal render)

Since 0.5.0 the OpenFX plug-in can take DaVinci Resolve's frames in GPU memory instead of CPU memory, so a
frame doesn't make a round trip through the CPU when no CPU stage needs it. This page is how it works, what it
covers and how it's tested. Code: `core/src/host_gpu.cpp` (entry points, routes), `core/src/host_gpu_cuda.cpp`
(CUDA ↔ Direct3D 11), `core/src/host_gpu_metal.mm` (Metal), `core/src/shaders/hostgpu_*.slang` (conversion
kernels), `ofx/ntscrt_ofx.cpp` (`render_frame`, `describe`).

## What Resolve offers (Windows, Resolve Studio 21.1.2, RTX 5090, CUDA mode)

- The host descriptor says `OfxImageEffectPropCudaRenderSupported` = true, `…CudaStreamSupported` = true,
  `…OpenCLRenderSupported` = false, `…MetalRenderSupported` = false (logged at load: `host GPU render: CUDA 1
  (streams 1), OpenCL 0, Metal 0`). Resolve's log shows its GPU processing in CUDA mode on the RTX 5090
  ("Initializing CUDA board manager"). So on this machine Resolve offers OFX plug-ins CUDA only.
- When the plug-in sets `OfxImageEffectPropCudaRenderSupported` (and stream support), every render on the
  Edit page (filter context) and on the Fusion page (general context) comes with
  `OfxImageEffectPropCudaEnabled` = 1 and a `cudaStream_t`: the source and output images are CUDA device
  pointers (RGBA 32-bit float, rows `width × 16` bytes, bottom row first; the Fusion page converts 8-bit and
  half-float tools to float for it). Images fetched for other times (Digital video, Screen Loop) are device
  pointers too. Resolve's stream is the same for all renders of a session.
- Resolve (its Fusion page in particular) keeps the plug-in's describe results while the plug-in file's path,
  size and modification time don't change (`OFXPluginCacheV2.xml`): a change of `NTSCRT_OFX_GPU` takes effect
  on the Fusion page only after the plug-in file changes. The Resolve test runner copies the bundle with a fresh
  modification time for that reason.

## Design: where the frame is at each step

The core's GPU stages run on its own Direct3D 11 device (Metal on macOS). The bridge registers two D3D11
textures per instance with the host's CUDA context (`cuGraphicsD3D11RegisterResource`) — one in the source's
format and size, one in the output's — and copies between them and Resolve's buffers with `cuMemcpy2DAsync` on
Resolve's stream. GPU to GPU, on the same adapter (the core checks the LUIDs match). The CUDA driver API is
loaded from `nvcuda.dll` at run time with prototypes declared in the code: no CUDA toolkit is needed to build,
nothing extra to install.

| step | Screen Loop on, NTSC on ntsc-rs, or Digital video on the CPU: `gpu-download` | everything else (the default look and Digital looks included, NTSC and Digital video on the GPU): `gpu-direct` |
|---|---|---|
| source | Resolve's CUDA buffer → registered D3D11 texture (GPU copy) | same |
| convert | `hostgpu_in/convert_in`: float → the pipeline's 8-bit picture (alpha mode, premultiplication, row order, crop) | same, plus the look's crop |
| CPU stages | the 8-bit picture comes down (¼ of the float frame's bytes), ntsc-rs / Digital video run, it goes up again | Digital video on the GPU: only its capture-size picture comes down (e.g. 320x180) and goes up again (docs/dev/DIGITAL-GPU.md) |
| NTSC on the GPU | (Screen Loop: on the camera texture, docs/dev/NTSC-GPU.md) | `NtscGpu::run` on the convert kernel's 8-bit picture (or Digital video's): the same bytes the CPU path gives ntsc-rs |
| Downscale, Glitch, CRT | GPU, as before | GPU |
| output | `hostgpu_out/merge_out`: CRT result + the source's alpha (from the source texture) → the output texture in Resolve's format and row order → Resolve's CUDA buffer (GPU copy) | same |

The CPU path, for comparison (Resolve's CPU images): Resolve downloads the float frame, the core converts it to
8 bits on the CPU, uploads it, …, downloads the CRT's half-float result and converts it to float on the CPU,
then Resolve uploads it again.

In *Clip* mode a source with transparent pixels (a title, a transformed clip, a Fusion source smaller than the
frame) needs the CPU path's hole filling (`fill_transparent`'s pull-push in `to_bgra8`): a kernel checks each
row for transparent pixels (`hostgpu_flags/row_flags`, one GPU round trip) and if there are any the holes are
filled by kernels on the GPU, with the same bytes ([Hole filling on the GPU](#hole-filling-on-the-gpu)), so the
frame keeps its route (`gpu-direct`, or `gpu-download` when a CPU stage needs the picture); the timings say
` fill_route=gpu (auto: <GPU>, arith float)`. Third route, `gpu-cpuconvert`: with `NTSCRT_FILL_GPU=0`, where the
route's rule says no, or for a frame whose fill kernels fail (` fill_route=cpu (<why>)`), the float frame is
read into CPU memory and converted there, as on the CPU path (up to 0.5.0-rc.5, every such frame did). The
output still goes back GPU to GPU.

Ordering: `cuGraphicsMapResources` waits for the D3D11 work issued before it and `cuGraphicsUnmapResources`
makes later D3D11 work wait for the CUDA copies issued before it, so neither side blocks the CPU; the render
call returns without waiting for the final copy (Resolve's stream carries it, as `…CudaStreamSupported`
requires). The CPU waits only where it reads: the 8-bit picture for ntsc-rs / Digital video / Screen Loop, the
transparency flags in Clip mode, and fetches of other frames.

With NTSC on (since phase 4 of docs/dev/NTSC-GPU.md, the default on D3D11 hardware) the frame stays
`gpu-direct`: NTSC runs on the 8-bit picture `convert_in` writes, which is the picture the CPU path quantises
before ntsc-rs (0 values different, `test_host_gpu`), so no `from_bgra8` variant for the host's format is needed.
The plan's CPU tables are built on another thread while the copy and the convert kernel are queued. If the
NTSC GPU route fails for a frame (`ntsc_route=cpu (GPU error: ...)`) that frame becomes `gpu-download` with
ntsc-rs, as before.

Digital video (since docs/dev/DIGITAL-GPU.md) works the same way: its full-size steps (the scale-down to the
camera's size, the upscale back) run as kernels on that 8-bit picture and only the capture-size picture crosses
to the CPU for the codec, so a Digital look is `gpu-direct` (`digital_route=gpu`). It stays on the CPU, and the
frame `gpu-download`, with `NTSCRT_DIGITAL_GPU=0`, where NTSC runs on ntsc-rs (`digital_route=cpu (ntsc-rs needs
the picture on the CPU)`), and for a frame whose Digital GPU steps fail.

### Why not OpenCL

Resolve doesn't offer OpenCL render to plug-ins here (CUDA mode), and an OpenCL `cl_mem` from the host can't be
shared with the core's D3D11 device anyway: `cl_khr_d3d11_sharing` / `cl_nv_d3d11_sharing` only share D3D11
resources into a context created with `CL_CONTEXT_D3D11_DEVICE_KHR` naming that D3D11 device, and the host made
its context without it. The only way across would be through CPU memory, which is the CPU path. The entry point
returns `NTSCRT_ERR_HOST_GPU` for OpenCL and the plug-in doesn't offer it; Resolve in OpenCL mode (AMD / Intel
GPUs, or set by hand) hands over CPU images as before.

### Metal (macOS)

`host_gpu_metal.mm`: Resolve's images are `id<MTLBuffer>`s on its queue's device, which must be the core's
Metal device. The source is blitted into a core texture after an empty command buffer on Resolve's queue has
completed (a queue runs its buffers in commit order), the result blitted back and waited for. It compiles in CI
but has never run in Resolve on a Mac, so the plug-in only offers Metal with `NTSCRT_OFX_GPU=1`.


## Fallbacks

- `NTSCRT_OFX_GPU=0`: the plug-in doesn't offer GPU render; Resolve hands over CPU images (the CPU path, as in
  0.5.0-rc.3).
- No NVIDIA driver (`nvcuda.dll`), or the CUDA driver can't start: CUDA isn't offered.
- The core's D3D11 device on another adapter than Resolve's CUDA device (several GPUs), an unsupported format
  (16-bit integer), no GPU device, or any CUDA / D3D11 error before the output is written:
  `ntscrt_render_host_gpu` returns `NTSCRT_ERR_HOST_GPU` without writing, and the plug-in renders that frame on
  the CPU path, copying Resolve's GPU images through CPU memory (`ntscrt_host_gpu_read` / `_write`). The reason
  is logged once per instance (`GPU path unavailable, rendering on the CPU path (...)`).
- Every render's log line says which path it took: `path=cpu`, `path=gpu-direct`, `path=gpu-download`,
  `path=gpu-cpuconvert`, or `cuda->cpu-fallback path=cpu`, followed by its phases (`in= digital= ntsc= upload=
  downscale= glitch= crt= loop= readback= out=` in ms, the ones that ran; `NTSCRT_PROFILE=1` waits for the GPU
  after each GPU phase so they're separated). The clock is exclusive (`core/src/phase_times.h`): a phase inside
  another counts only as itself, so on the CPU path `readback` is the transfer and the wait for the GPU and
  `out` the conversion of the bands it read.
  `NTSCRT_OFX_LOG_VERBOSE=1` logs every render.

## Hole filling on the GPU

Code: `core/src/shaders/hostgpu_fill.slang` (kernels), `core/src/host_gpu_fill.cpp` (`hostgpu::Fill`, the route
rule), `host_gpu.cpp` (`convert_filled`). Reference and fallback: `fill_transparent` in `pixel_convert.cpp`,
unchanged.

What the CPU code computes (Clip mode, some pixel's alpha at or below 0.5/255): on to_bgra8's 8-bit picture, each
pixel weighted 1 when its alpha is above 0.5/255, else 0, a pyramid of levels down to 1 x 1, each pixel the
weighted mean of a 2 x 2 block of the level below (on odd sizes the edge pixel is counted twice) with weight
`min(1, sum)`; then from the top down, every pixel of weight < 1 takes the bilinear sample of the (finished)
coarser level; on the picture itself only transparent pixels change, to level 1's bilinear sample rounded to 8
bits. Nothing changes when every pixel is transparent. Every step but the order of the levels is per pixel, so
it maps onto one dispatch per level:

| CPU step | kernel |
|---|---|
| to_bgra8's colour and weight of each pixel + the first level | `fill_down0` (from the host's source texture: convert_in's arithmetic, then the 2 x 2 means) |
| the other levels | `fill_down`, one dispatch per level (RGBA32F: r, g, b, weight) |
| the push, coarse to fine | `fill_up`, one dispatch per level (into a second texture per level: RW textures are only readable as R32F) |
| the picture | `convert_fill`: convert_in, transparent pixels from level 1 (none when the top level's weight is 0), cut to the look's crop |

At UHD that is 12 levels, 25 dispatches, ~45 MB of level textures (kept per instance).

**Exactness: byte-identical.** The weights are 0 or 1 at every level (`min(1, a whole number)`), so each mean
is a sum of up to four floats divided by 1, 2, 3 or 4, and the push only copies a sample where the weight is 0.
The CPU (MSVC `/fp:precise`, SSE2: nothing fused) rounds each product and each sum to float, in a fixed order.
The kernels do the same with the Digital video port's tools (DIGITAL-GPU.md): every value `precise` and through
`A.mul` / `A.add`, plain float where the probe (`fill_probe`, the same three tests as Digital video's) says the
device rounds a precise product and sum to nearest without fusing them, double-precision `*_x` kernels
otherwise. No GPU division: `/2` and `/4` are multiplications (exact), `/3` (`div3`) takes the float nearest
`s / 3` among the five around `s * float(1/3)`, by residuals `s - 3q` computed exactly (TwoSum: `2q + q` and its
rounding error; the difference of two floats within a factor 2 is exact; the residual is a small multiple of
`q`'s last place); there are no ties. The 8-bit values are `k / 255.f` from a table the CPU makes, not a GPU
division. The colour of each pixel is convert_in's (`div` with the Newton step, `q8`). Bilinear weights are
quarters (exact).

Measured (`test_fill_gpu`, 175 cases: none / all transparent / single pixels / borders / a large hole / noise /
a soft ramp / stripes / NaN alpha, at 1x1, 2x1, 1x2, 2x2, 3x3, 5x1, 1x7, 17x9, 64x64, 65x33, 333x197, 1001x3,
3x1001, 241x135 and 1080p, UHD once; float, half and 8-bit sources, straight and premultiplied, both row
orders, crops, sources smaller than the frame, moved, partly or wholly outside it): RTX 5090 0 bytes different,
plain and double kernels. WARP: the fill itself 0 bytes different (checked against `fill_transparent` run on
WARP's own convert_in picture); 4 premultiplied soft-alpha cases differ from the CPU path by one level in 3-215
bytes, all in convert_in's division (WARP doesn't fuse `mad`, so `div`'s Newton step isn't exact there): a
property of the existing conversion kernel, not of the fill, and host GPU frames never run on WARP (the bridge
needs the host's GPU). `test_host_gpu`: Clip-mode frames with holes in every format, straight and premultiplied,
crop, render scale, moved and smaller sources, Digital video: `gpu-direct` (`gpu-download` with
`NTSCRT_DIGITAL_GPU=0`), 0 values different from the CPU path (float and 8-bit; half: the usual output rounding).

**The rule** (`fill_gpu_route`, decided per instance on its first frame with holes, again when `NTSCRT_FILL_GPU`
changes or the device is replaced; `GpuRoute` / `gpu_route_on` as the NTSC and Digital video routes): `0` = the
CPU code (`gpu-cpuconvert`); not on Metal unless `force` (compiled, unverified, fast math); not on a software
device (WARP) unless `force`; not where the probe says `inexact` unless `force`. A GPU error takes the CPU code
for that frame, three in a row for the instance. `NTSCRT_FILL_GPU_ARITH=float` / `double` picks the kernels
whatever the probe says (the test runs both).

**Speed** (UHD float frame with transparent corners in Clip mode, `test_host_gpu`'s `NTSCRT_HOSTGPU_BENCH=1`
case, `NTSCRT_PROFILE=1`, RTX 5090 + Ryzen 9 9950X3D, ms per frame, frames 9-11; raw: `C:/ntb/w15b-fillbench-profile2.txt`
on the dev PC):

| | `in=` (holes) | `in=` (the same frame opaque) | path |
|---|---|---|---|
| GPU fill (default) | 1.2-1.6 | 0.9 | `gpu-direct` |
| `NTSCRT_FILL_GPU=0` (as 0.5.0-rc.5) | 98-102 (+ 2.0 `upload=`) | 0.9 | `gpu-cpuconvert` |

So the fill's kernels cost ~0.3-0.7 ms of GPU time at UHD; the CPU fill (`fill_transparent`, then
single-threaded) and the float frame's download had cost ~100 ms. The check for holes (`row_flags` and its 34 KB
download at UHD) still costs a GPU round trip per Clip-mode frame, as before.

## Output vs the CPU path

The conversion kernels do the CPU path's arithmetic (`to_bgra8`, `merge_alpha`): the same 8-bit rounding (round
half up), the same alpha rules, and the CRT's half-float result read the same way. Two GPU habits had to be
worked around to get the same bits: shader compilers fuse `v * 255 - floor(...)` into one multiply-add, which
skips the rounding of the product the CPU does (values exactly halfway between two 8-bit levels then rounded
down: in Resolve a few such values per 1080p Fusion frame, each a one-level step the CRT shader spreads over a few
pixels); and GPU division is only accurate to a few units in the last place, so divisions get one Newton step
on an exact (fused) residual. With those, the GPU path writes exactly the CPU path's values, except for
half-float hosts: the GPU rounds float to half to nearest even, the CPU half up, one half-float step on a few
values.

Tolerance, used by every test below: each channel within 1/255 (+1e-5) of the CPU path's value, and at most
0.2% of the values different at all. Measured: 0 differences on float and 8-bit frames, ≤ 0.016/255 on
≤ 0.03% of the values of half-float frames (`test_host_gpu`), 0 in Resolve (`--gpu-ab`).

One difference is Resolve's, not the plug-in's: with GPU render offered, Resolve's Fusion page hands the
plug-in float images whatever its tools' depth (with CPU images it hands over the tools' own 8-bit or
half-float), so on 8-bit and half-float comps the effect now gets and writes float, like on the Edit page
(the CRT shader's half-float output instead of 8 bits). Edit-page clips are float either way.

## Tests

- `test_host_gpu` (core): the test acts as a CUDA host (device memory of the primary context, a non-blocking
  stream, padded rows, bottom row first) and compares `ntscrt_render_host_gpu` (default NTSC route) with
  `ntscrt_render_cpu` with `NTSCRT_NTSC_GPU=0` (ntsc-rs) over three frames each: default look (also cropped,
  at render scale 0.5, with holes in Clip mode, a source smaller than the frame), NTSC off, half float, 8-bit,
  premultiplied and straight alpha in Clip and Spill, render scale 0.5, crop, Digital video (on and off, with
  and without NTSC, formats, *Webcam (low quality)*, crop, render scale, holes; the CPU reference also with
  `NTSCRT_DIGITAL_GPU=0`), Clip mode with holes in every format (straight and premultiplied, crop, render
  scale, moved and smaller sources). 0 values differ except half-float output, and each case checks its route
  (`gpu-direct` with NTSC and Digital video on the GPU and with holes, `fill_route=gpu`; `gpu-cpuconvert` with
  `NTSCRT_FILL_GPU=0`; `gpu-download` with `NTSCRT_DIGITAL_GPU=0`). Premultiplied 8-bit and half-float output in
  Clip mode differs from the CPU path by up to one 8-bit level / half-float step on 1-27% of the values
  (`merge_out`'s rounding of RGB × alpha, the same with the CPU fill; Resolve's GPU images are float, where it's
  exact): there the test checks the GPU fill's output is the CPU fill's on the same path, value for value.
  Skipped without an NVIDIA driver (CI).
- `test_fill_gpu` (core, runs in CI on WARP): the fill kernels against `to_bgra8` + `fill_transparent`, byte for
  byte ([Hole filling on the GPU](#hole-filling-on-the-gpu)).
- `test_ofx` "GPU render (fake CUDA host)": the mock OpenFX host hands the plug-in CUDA images
  (`kOfxImageEffectPropCudaEnabled`, a stream) and the same frames are compared with CPU-image renders: default
  look, NTSC off, alpha in both modes, render scale 0.5, a render window smaller than the frame, Digital video
  (temporal fetches from GPU images). Float only (the mock host's format).
- Resolve: `run_resolve_tests.py --headless --bundle <bundle> --gpu-ab` renders the same frames (default look,
  NTSC off, Clean CRT, 8-bit and half-float Fusion sources, a title in Clip and Spill mode, the effect on an
  Edit-page clip) in a session with `NTSCRT_OFX_GPU=0` and one with the GPU path, as 16-bit PNGs, and compares
  them; it fails if the GPU session took the CPU path anywhere. The whole suite passes with the GPU path live.
- Speed: `run_resolve_tests.py --headless --bundle <bundle> --only bench` (render-queue jobs of 300 frames at
  UHD and 1080p, 60 fps, three looks; one `BENCH {...}` JSON line per job).

## Speed (Windows, RTX 5090, Resolve Studio 21.1.2, 2026-11-11)

`run_resolve_tests.py --only bench`: the effect on an Edit-page Fusion composition clip (animated gradient,
noise and title at the timeline's size), one render-queue job of 300 frames to H.264 (NVENC), 60 fps
projects. *Wall* is the job's time, two runs each (the same clip without the effect takes 2.85 s at UHD,
1.35 s at 1080p). *ms* is the plug-in's own time per render (median / p95 of ~307 renders): on the GPU path
it doesn't include the GPU work Resolve's stream runs after the render call returns, so compare wall times
across paths. Baseline: v0.5.0-rc.3 (plus only the test-preset hook the bench uses), CPU path: this build
with `NTSCRT_OFX_GPU=0`.

| UHD (3840×2160) | baseline | CPU path | GPU path |
|---|---|---|---|
| default look (ntsc-rs on): wall s | 27.9 / 27.4 | 28.4 / 27.4 | 21.9 / 21.4 |
| ms median / p95 | 73 / 77 | 72-76 / 78 | 62 / 65-68 |
| Clean CRT: wall s | 11.9 / 11.9 | 11.9 / 11.9 | 2.9 / 2.3 (= no effect) |
| ms median / p95 | 22 / 25 | 22 / 25 | 1.8 / 2.5-2.9 |
| default, NTSC off: wall s | 12.4 / 11.9 | 12.4 / 12.4 | 2.9 / 2.4 (= no effect) |

| 1080p | baseline | CPU path | GPU path |
|---|---|---|---|
| default look: wall s | 8.85 / 8.88 | 8.86 / 8.85 | 7.35 / 7.36 |
| Clean CRT: wall s | 4.85 / 4.86 | 4.86 / 4.87 | 2.36 / 2.34 |
| default, NTSC off: wall s | 4.85 / 4.85 | 4.84 / 4.85 | 2.36 / 2.35 |

Phases at UHD, ms per render (`NTSCRT_PROFILE=1`, which waits for the GPU after each phase):

| phase | CPU path, default | GPU path, default | CPU path, NTSC off | GPU path, NTSC off |
|---|---|---|---|---|
| in (CPU: float → 8 bit on the CPU; GPU: copy + convert, + the 8-bit download when ntsc-rs is on) | 8.4 | 8.8 | 9.0 | 1.2 |
| ntsc-rs (CPU) | 49.1 | 47.6 | — | — |
| upload (8-bit picture) | 2.9 | 2.8 | 2.6 | — |
| Downscale / Glitch / CRT | 0.2 / 0.7 / 1.6 | 0.2 / 0.7 / 1.5 | 0.2 / 0.3 / 0.4 | 0.1 / 0.2 / 0.4 |
| out (CPU: half-float readback + conversion; GPU: merge + copy) | 10.7 | 0.3 | 10.5 | 0.2 |

With ntsc-rs off nothing is left but the GPU stages: the effect costs nothing measurable in the job's wall
time. With ntsc-rs on (the default look) ntsc-rs's ~48 ms on the CPU at UHD dominates; the GPU path saves the
output readback and conversion (~11 ms of the plug-in's time) and Resolve's own float transfers to and from
CPU memory (the rest of the ~20 ms per frame the wall time drops by), while the input side is a wash (the
8-bit download costs about what the CPU conversion did).

## Speed with the fast transfers (both branches combined, 2026-11-12)

The same bench (`--only bench`, 300 frames, H.264 NVENC, 60 fps), 3 interleaved runs per build (5 for the UHD
GPU-path rows), separate builds of each: A = v0.5.0-rc.3 (+ the bench's test-preset hook), B = fast transfers
only, C = host GPU frames only, D = both (and D with each switch off). *ms/frame* = (median wall − the median
no-effect job, 2.86 s UHD / 1.34 s 1080p) / 300. Resolve reports job ends in steps of about 0.5 s (≈1.7 ms per
frame here), so differences under ~2 ms/frame are noise; the GPU-path *Clean CRT* jobs finish a step *before*
the no-effect job (shown as 0).

| UHD, ms/frame (wall s) | A | B | C | D | D, `NTSCRT_OFX_GPU=0` | D, `NTSCRT_FAST_TRANSFER=0` |
|---|---|---|---|---|---|---|
| default look | 83.5 (27.9) | 70.0 (23.9) | 60.0 (20.9) | 55.0 (19.4) | 68.3 (23.4) | 63.4 (21.9) |
| Clean CRT | 31.7 (12.4) | 15.0 (7.4) | 0 (2.35) | 0 (2.35) | 15.0 (7.4) | 0 (2.35) |
| default, NTSC off | 31.7 (12.4) | 15.0 (7.4) | 0 (2.35) | 0 (2.35) | 15.0 (7.4) | 0 (2.36) |

| 1080p, ms/frame (wall s) | A | B | C | D | D, `NTSCRT_OFX_GPU=0` | D, `NTSCRT_FAST_TRANSFER=0` |
|---|---|---|---|---|---|---|
| default look | 23.4 (8.36) | 21.8 (7.87) | 18.3 (6.84) | 18.4 (6.86) | 21.8 (7.87) | 18.4 (6.86) |
| Clean CRT | 11.7 (4.85) | 6.7 (3.36) | 3.4 (2.35) | 3.4 (2.35) | 8.4 (3.85) | 3.3 (2.34) |
| default, NTSC off | 10.0 (4.35) | 6.7 (3.36) | 3.3 (2.34) | 3.4 (2.35) | 6.7 (3.35) | 3.4 (2.35) |

The gains don't add up: once the frame stays on the GPU there is little transfer left to speed up. With ntsc-rs
off, GPU frames alone remove the effect's cost. With it on (default look, UHD) the fast transfers still save
~5 ms per frame on the GPU path (the banded 8-bit download, buffers kept between frames: D 19.4 s, range
19.35-20.4 over 5 runs, vs C 20.9 s, 18.9-21.9); at 1080p it's within noise. The CPU path (AMD / Intel, macOS,
`NTSCRT_OFX_GPU=0`) gets the fast transfers' full gain. Raw results: `C:/ntb/w7s-bench/` on the test machine.

## Speed with NTSC on the GPU (phase 4, 2026-11-12)

The same bench (`--only bench --bench-repeat 3 --bench-looks default,clean-crt,ntsc-off,mild-vhs`, 300 frames,
H.264 NVENC, 60 fps), one build (agent-x/ntsc-gpu-4), three Resolve sessions: **rc.4** = `NTSCRT_NTSC_GPU=0`
(ntsc-rs: 0.5.0-rc.4's behaviour), **default** (NTSC on the GPU, host GPU frames), **CPU frames** = the default
with `NTSCRT_OFX_GPU=0` (Resolve's CPU images, NTSC on the GPU inside: what AMD / Intel GPUs in OpenCL mode
get, if they pass the exactness check). Median wall of 3 jobs; *ms/frame* = (median wall − the session's
no-effect job: 2.35 s UHD, 1.35 s 1080p) / 300, in Resolve's ~0.5 s steps (≈1.7 ms per frame); *plug-in ms* =
the median of the plug-in's own time per render (~307 renders per job).

| UHD | rc.4: wall s (ms/frame) | default | CPU frames | plug-in ms rc.4 / default / CPU frames |
|---|---|---|---|---|
| default look | 17.87 (51.7) | **2.84 (1.6)** | 8.36 (20.0) | 50.5 / 3.2 / 13.2 |
| *Mild VHS* | 13.37 (36.7) | **2.85 (1.7)** | 7.86 (18.4) | 36.0 / 2.0 / 11.4 |
| *Clean CRT* | 2.35 (0) | 2.36 (0) | 7.86 (18.4) | 1.8 / 1.8 / 10.0 |
| default, NTSC off | 2.84 (1.6) | 2.35 (0) | 7.86 (18.4) | 1.8 / 1.8 / 10.0 |

| 1080p | rc.4: wall s (ms/frame) | default | CPU frames | plug-in ms rc.4 / default / CPU frames |
|---|---|---|---|---|
| default look | 6.85 (18.4) | **2.35 (3.3)** | 3.85 (8.4) | 15.7 / 1.5 / 4.4 |
| *Mild VHS* | 5.35 (13.4) | **2.35 (3.3)** | 3.86 (8.4) | 11.2 / 1.3 / 3.8 |
| *Clean CRT* | 2.35 (3.4) | 2.35 (3.3) | 3.86 (8.4) | 1.2 / 1.2 / 3.2 |
| default, NTSC off | 2.34 (3.3) | 2.35 (3.3) | 3.86 (8.4) | 1.2 / 1.2 / 3.3 |

The default look now costs what *Clean CRT* costs: at UHD the job ends one step after the no-effect job (rc.4:
15.5 s after it), at 1080p in the same step as *Clean CRT* (the 1080p jobs' ~1 s over the no-effect job is
there for every look, *Clean CRT* included). Routes: every render of the default session was `gpu-direct` with
`ntsc_route=gpu` (307-308 per job), rc.4's NTSC jobs `gpu-download` with `ntsc_route=cpu (NTSCRT_NTSC_GPU=0)`,
the CPU-frames session `cpu` with `ntsc_route=gpu`; one `NTSC route:` line per instance, `gpu (auto: NVIDIA
GeForce RTX 5090, fma mad, CPU AVX2+FMA)`. With CPU frames the effect's cost is the transfers (as *Clean CRT*:
the NTSC passes add ~1-3 ms of the plug-in's time).

Phases with `NTSCRT_PROFILE=1` (a wait after each GPU phase and each NTSC pass, so slower; plug-in ms median
7.7 UHD / 5.1 1080p for the default look), mean ms per render, default session:

| phase | UHD default look | UHD *Mild VHS* | 1080p default look | 1080p *Mild VHS* |
|---|---|---|---|---|
| in (copy + convert kernel) | 1.05 | 1.09 | 0.65 | 0.60 |
| ntsc (CPU tables + the passes, each waited for) | 6.62 | 2.17 | 4.47 | 1.42 |
| Downscale / Glitch / CRT | 0.11 / 0.61 / 1.59 | 0.10 / 0.19 / 0.38 | 0.09 / 0.62 / 0.71 | 0.09 / 0.19 / 0.29 |
| out (merge + copy) | 0.20 | 0.17 | 0.20 | 0.18 |

Raw results: `C:/ntb/w11x-res/bench{A,B,C,P}/bench.jsonl` and `C:/ntb/w11x-resolve.log` on the test machine.

## CPU frames: the conversions (2026-11-12)

What AMD / Intel GPUs, macOS and `NTSCRT_OFX_GPU=0` get (`ntscrt_render_cpu`). The profile first
(`NTSCRT_PROFILE=1 bench_transfer`, UHD float, default look, ms): convert in 2.8, convert out 4.3, readback 0.5
(with the profile's waits; without them `readback` is mostly the wait for the GPU stages, ~4 ms with NTSC on).
Both conversions run on the thread pool at close to the memory bandwidth (UHD float: 133 MB read and 33 MB
written in, 66 MB read and 133 MB written out), so there was little to win there; a Clip-mode frame with
transparent pixels was another matter: `fill_transparent` ran on one thread (~80 ms at UHD). Changes, none of
which changes a byte (`bench_transfer --dump` of 36 cases, sizes UHD / 1080p / 333x197, formats float / half /
16-bit / 8-bit, default / NTSC off / *Clean CRT*, opaque and `--alpha`: identical to v0.5.0-rc.5's;
`test_core_convert` with `NTSCRT_TEST_EXHAUSTIVE=1`, `test_core_bitdepth`):

- RGBA16F results (16-bit and float hosts) are read back without their alpha (`facade/pack_rgb`: 6 bytes a pixel
  instead of 8, the host's alpha comes from the source; every half's bits kept, checked for all 65536):
  `from_rgb16f_rows` converts from that.
- `fill_transparent` runs each level's rows on the thread pool (the levels only read the level before them) and no
  longer zero-fills or single-thread scans its buffers.
- The SIMD kernels' 8 / 16-bit rounding (`round4`) is `floor(p) + (p - floor(p) >= 0.5)` in float (exactly
  `uint32_t(double(p) + 0.5)`) instead of four double conversions.

Not done: uploading in the host's format with the conversion in a kernel (a UHD float frame is 133 MB over PCIe
instead of 33 MB: slower), the GPU fill for CPU frames (it would need the picture's alpha uploaded and the fill
route's exactness check on AMD / Intel GPUs, which nobody has measured).

`bench_transfer --sizes 3840x2160,1920x1080 --formats rgba32f,rgba16f,rgba8 --looks default,ntsc_off,clean_crt
--frames 120 --warmup 20` (and `--alpha`), v0.5.0-rc.5 (`main`, fedb8e8) against this, three interleaved runs
each, median of the three medians, ms per frame (RTX 5090, Ryzen 9 9950X3D; no other benchmark ran meanwhile;
raw: `C:/ntb/w15b-bench/` on the dev PC). `in / rb / out` = convert in / readback / convert out.

Opaque sources:
| size | format | look | before | after | change | before in / rb / out | after in / rb / out |
|---|---|---|---|---|---|---|---|
| 3840x2160 | rgba32f | default | 10.77 | 10.25 | -5% | 2.05 / 3.97 / 4.24 | 2.10 / 4.06 / 3.59 |
| 3840x2160 | rgba32f | ntsc_off | 7.92 | 7.30 | -8% | 2.02 / 1.41 / 4.27 | 2.02 / 1.50 / 3.62 |
| 3840x2160 | rgba32f | clean_crt | 7.93 | 7.21 | -9% | 2.01 / 1.47 / 4.28 | 1.89 / 1.54 / 3.60 |
| 3840x2160 | rgba16f | default | 9.16 | 9.07 | -1% | 1.16 / 4.00 / 3.40 | 1.15 / 4.10 / 3.24 |
| 3840x2160 | rgba16f | ntsc_off | 6.07 | 5.94 | -2% | 1.12 / 1.41 / 3.36 | 1.07 / 1.49 / 3.22 |
| 3840x2160 | rgba16f | clean_crt | 6.13 | 6.02 | -2% | 1.13 / 1.45 / 3.38 | 1.08 / 1.54 / 3.24 |
| 3840x2160 | rgba8 | default | 6.89 | 6.93 | +1% | 0.86 / 4.15 / 1.24 | 0.85 / 4.16 / 1.24 |
| 3840x2160 | rgba8 | ntsc_off | 3.80 | 3.69 | -3% | 0.82 / 1.56 / 1.23 | 0.80 / 1.51 / 1.22 |
| 3840x2160 | rgba8 | clean_crt | 3.77 | 3.74 | -1% | 0.81 / 1.55 / 1.22 | 0.81 / 1.54 / 1.22 |
| 1920x1080 | rgba32f | default | 3.33 | 3.29 | -1% | 0.34 / 1.60 / 0.90 | 0.36 / 1.56 / 0.84 |
| 1920x1080 | rgba32f | ntsc_off | 2.00 | 1.98 | -1% | 0.29 / 0.68 / 0.90 | 0.31 / 0.67 / 0.84 |
| 1920x1080 | rgba32f | clean_crt | 2.11 | 2.00 | -5% | 0.29 / 0.72 / 0.91 | 0.32 / 0.66 / 0.84 |
| 1920x1080 | rgba16f | default | 3.33 | 3.23 | -3% | 0.32 / 1.59 / 0.92 | 0.31 / 1.53 / 0.92 |
| 1920x1080 | rgba16f | ntsc_off | 1.83 | 1.80 | -2% | 0.30 / 0.67 / 0.72 | 0.29 / 0.62 / 0.74 |
| 1920x1080 | rgba16f | clean_crt | 1.90 | 1.85 | -3% | 0.30 / 0.70 / 0.73 | 0.29 / 0.66 / 0.74 |
| 1920x1080 | rgba8 | default | 2.59 | 2.58 | -0% | 0.34 / 1.44 / 0.34 | 0.34 / 1.44 / 0.34 |
| 1920x1080 | rgba8 | ntsc_off | 1.25 | 1.22 | -3% | 0.24 / 0.53 / 0.33 | 0.24 / 0.52 / 0.33 |
| 1920x1080 | rgba8 | clean_crt | 1.23 | 1.25 | +1% | 0.24 / 0.52 / 0.33 | 0.24 / 0.53 / 0.33 |

With `--alpha` (transparent and half-transparent areas, Clip mode: the fill):
| size | format | look | before | after | change | before in / rb / out | after in / rb / out |
|---|---|---|---|---|---|---|---|
| 3840x2160 | rgba32f | default | 97.69 | 36.19 | -63% | 83.85 / 5.39 / 4.87 | 25.23 / 4.80 / 4.10 |
| 3840x2160 | rgba32f | ntsc_off | 94.24 | 33.51 | -64% | 85.72 / 15.88 / 4.83 | 25.64 / 2.28 / 4.10 |
| 3840x2160 | rgba32f | clean_crt | 91.61 | 33.43 | -64% | 82.93 / 16.36 / 4.57 | 25.38 / 2.30 / 4.12 |
| 3840x2160 | rgba16f | default | 89.80 | 30.82 | -66% | 79.19 / 5.33 / 3.78 | 20.48 / 4.83 / 3.36 |
| 3840x2160 | rgba16f | ntsc_off | 85.94 | 27.41 | -68% | 78.21 / 16.69 / 3.85 | 20.25 / 2.25 / 3.35 |
| 3840x2160 | rgba16f | clean_crt | 86.45 | 27.49 | -68% | 55.05 / 16.86 / 3.93 | 20.38 / 2.28 / 3.37 |
| 3840x2160 | rgba8 | default | 59.28 | 25.62 | -57% | 50.23 / 5.41 / 1.82 | 17.06 / 4.82 / 1.77 |
| 3840x2160 | rgba8 | ntsc_off | 67.43 | 23.82 | -65% | 50.27 / 2.41 / 1.84 | 18.18 / 2.30 / 1.76 |
| 3840x2160 | rgba8 | clean_crt | 81.93 | 24.10 | -71% | 69.72 / 2.35 / 1.87 | 18.56 / 2.31 / 1.75 |
| 1920x1080 | rgba32f | default | 21.39 | 9.65 | -55% | 17.37 / 1.90 / 1.03 | 6.20 / 1.72 / 0.91 |
| 1920x1080 | rgba32f | ntsc_off | 22.48 | 8.51 | -62% | 18.32 / 1.01 / 1.02 | 6.20 / 0.82 / 0.91 |
| 1920x1080 | rgba32f | clean_crt | 21.77 | 8.56 | -61% | 18.84 / 1.02 / 1.03 | 6.23 / 0.81 / 0.91 |
| 1920x1080 | rgba16f | default | 23.38 | 10.11 | -57% | 18.78 / 1.85 / 1.01 | 6.55 / 1.71 / 0.98 |
| 1920x1080 | rgba16f | ntsc_off | 22.06 | 9.15 | -59% | 19.34 / 0.96 / 0.95 | 6.87 / 0.85 / 0.75 |
| 1920x1080 | rgba16f | clean_crt | 21.78 | 9.14 | -58% | 19.16 / 0.96 / 0.91 | 6.91 / 0.84 / 0.75 |
| 1920x1080 | rgba8 | default | 22.40 | 9.90 | -56% | 19.24 / 1.68 / 0.53 | 6.94 / 1.65 / 0.47 |
| 1920x1080 | rgba8 | ntsc_off | 21.23 | 8.77 | -59% | 19.21 / 0.80 / 0.51 | 6.93 / 0.75 / 0.42 |
| 1920x1080 | rgba8 | clean_crt | 21.18 | 8.75 | -59% | 19.09 / 0.79 / 0.50 | 6.89 / 0.76 / 0.43 |
