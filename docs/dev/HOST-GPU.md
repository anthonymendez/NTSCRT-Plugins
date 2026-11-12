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

| step | default look (ntsc-rs on): `gpu-download` | ntsc-rs, Digital video, Screen Loop off: `gpu-direct` |
|---|---|---|
| source | Resolve's CUDA buffer → registered D3D11 texture (GPU copy) | same |
| convert | `hostgpu_in/convert_in`: float → the pipeline's 8-bit picture (alpha mode, premultiplication, row order, crop) | same, plus the look's crop |
| CPU stages | the 8-bit picture comes down (¼ of the float frame's bytes), ntsc-rs / Digital video run, it goes up again | — |
| Downscale, Glitch, CRT | GPU, as before | GPU |
| output | `hostgpu_out/merge_out`: CRT result + the source's alpha (from the source texture) → the output texture in Resolve's format and row order → Resolve's CUDA buffer (GPU copy) | same |

The CPU path, for comparison (Resolve's CPU images): Resolve downloads the float frame, the core converts it to
8 bits on the CPU, uploads it, …, downloads the CRT's half-float result and converts it to float on the CPU,
then Resolve uploads it again.

Third route, `gpu-cpuconvert`: in *Clip* mode a source with transparent pixels (a title, a Fusion source smaller
than the frame) needs the CPU path's hole filling (pull-push in `to_bgra8`); a kernel checks each row for
transparent pixels (`hostgpu_flags/row_flags`, one GPU round trip) and if there are any the float frame is read
into CPU memory and converted there, as on the CPU path. The output still goes back GPU to GPU.

Ordering: `cuGraphicsMapResources` waits for the D3D11 work issued before it and `cuGraphicsUnmapResources`
makes later D3D11 work wait for the CUDA copies issued before it, so neither side blocks the CPU; the render
call returns without waiting for the final copy (Resolve's stream carries it, as `…CudaStreamSupported`
requires). The CPU waits only where it reads: the 8-bit picture for ntsc-rs, the transparency flags in Clip
mode, and fetches of other frames.

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
  stream, padded rows, bottom row first) and compares `ntscrt_render_host_gpu` with `ntscrt_render_cpu` over
  three frames each: default look, NTSC off, half float, 8-bit, premultiplied and straight alpha in Clip and
  Spill, render scale 0.5, crop, a source smaller than the frame, Digital video. Skipped without an NVIDIA
  driver (CI).
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
