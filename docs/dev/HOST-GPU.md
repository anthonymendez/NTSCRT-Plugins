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

| step | Digital video or Screen Loop on, or NTSC on ntsc-rs: `gpu-download` | everything else (the default look included, NTSC on the GPU): `gpu-direct` |
|---|---|---|
| source | Resolve's CUDA buffer → registered D3D11 texture (GPU copy) | same |
| convert | `hostgpu_in/convert_in`: float → the pipeline's 8-bit picture (alpha mode, premultiplication, row order, crop) | same, plus the look's crop |
| CPU stages | the 8-bit picture comes down (¼ of the float frame's bytes), ntsc-rs / Digital video run, it goes up again | — |
| NTSC on the GPU | (Screen Loop: on the camera texture, docs/dev/NTSC-GPU.md) | `NtscGpu::run` on the convert kernel's 8-bit picture: the same bytes the CPU path gives ntsc-rs |
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
requires). The CPU waits only where it reads: the 8-bit picture for ntsc-rs / Digital video / Screen Loop, the
transparency flags in Clip mode, and fetches of other frames.

With NTSC on (since phase 4 of docs/dev/NTSC-GPU.md, the default on D3D11 hardware) the frame stays
`gpu-direct`: NTSC runs on the 8-bit picture `convert_in` writes, which is the picture the CPU path quantises
before ntsc-rs (0 values different, `test_host_gpu`), so no `from_bgra8` variant for the host's format is needed.
The plan's CPU tables are built on another thread while the copy and the convert kernel are queued. If the
NTSC GPU route fails for a frame (`ntsc_route=cpu (GPU error: ...)`) that frame becomes `gpu-download` with
ntsc-rs, as before.

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
  stream, padded rows, bottom row first) and compares `ntscrt_render_host_gpu` (default NTSC route) with
  `ntscrt_render_cpu` with `NTSCRT_NTSC_GPU=0` (ntsc-rs) over three frames each: default look (also cropped,
  at render scale 0.5, with holes in Clip mode, a source smaller than the frame), NTSC off, half float, 8-bit,
  premultiplied and straight alpha in Clip and Spill, render scale 0.5, crop, Digital video. 0 values differ
  except half-float output, and each case checks its route (`gpu-direct` with NTSC on the GPU,
  `gpu-cpuconvert` with holes, `gpu-download` with Digital video). Skipped without an NVIDIA driver (CI).
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
