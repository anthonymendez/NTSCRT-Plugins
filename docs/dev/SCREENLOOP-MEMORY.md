# Screen Loop on full-size Edit-page clips: fetches and memory

Screen Loop (video feedback, `core/stages/howlaround/`) renders frame N by running the loop from its window start
A(N) (`howl_engine.h`): with the default window B = 240, A(N) = 0 for N < 480, else (floor(N / B) - 1) * B. A cold
seek (a frame whose history the effect doesn't have) re-runs A(N)..N - 1, fetching each of those source frames
from the host (`ntscrt_render_args.fetch_source`). That is 240 to 479 frames, and it is what makes every frame the
same whatever order the host asks for them in (`tests/howlaround/test_howl_seek.cpp`).

## What went wrong in Resolve (2026-11-12)

A headless parity run stalled on an Edit-page clip with a Screen Loop preset in a 1080p project: the render of frame
1850 did not finish in 10 minutes and Resolve grew to 20-30 GB. Measured since (headless Resolve 21.1.2, RTX 5090,
62 GB RAM, `tests/resolve` harness, the plug-in's debug log):

1. **Declared frames are rendered up front and kept.** `getFramesNeeded` declared the loop's whole window (1440..1850
   for frame 1850; 0..N for every N < 480). Resolve calls it for the frame it renders and the dozen before it, renders
   every declared source frame *before* calling the plug-in's render, and keeps them. In a UHD project a sequential
   render from frame 0 reached 40 GB before its first frame (killed); a UHD cold seek likewise, before the plug-in
   fetched anything. At 1080p a sequential 300-frame render made 3827 `getFramesNeeded` calls.
2. **Resolve keeps every source frame a plug-in fetches.** Fetched on request instead (not declared), the frames come
   back with the same pixels (frame 1850 compared bit for bit with the declared run and with an in-order render
   1440..1850), but Resolve's private memory still grows by about 80 MB per distinct 1080p frame fetched and 200-330
   MB per UHD frame (1.5-2.5x a float RGBA frame), and doesn't give it back while the project is open (a second
   timeline, a second effect: still held; fetching the same frames again adds nothing). The plug-in holds one host
   image at a time and releases it as soon as it is converted (`temporal_held_max=1` in the debug log;
   `RenderCtx::fetch` / `release_source`), so this is Resolve's own cache, presumably bounded by *Preferences → Memory and
   GPU → Limit Resolve memory usage* (75% of RAM by default, 46 GB here); the tests stop Resolve at 40 GB, so whether
   it evicts cleanly at its limit was not observed. A second 1080p cold seek in the same session passed 40 GB.
   The stalled parity case was the 1080p cold seek (+32 GB) in a session whose Resolve already held 20-30 GB.
3. **Window starts re-ran their window during playback.** Playing (or exporting) across a window start W + B (the
   first frame whose anchor is W) re-ran the loop from W: 240 more source fetches every 240 frames past frame 480,
   and a stall of the whole re-run on that frame.
4. **A render at another size threw the history away.** Resolve renders the effect's frame for its viewer and
   thumbnails at a reduced render scale (0.11-0.16) in between the render queue's full-size frames, on the same
   effect instance. The loop kept one history, keyed by the scene size, so that render replaced it and the next
   full-size frame re-ran its window: in a CPU-path render of frames 1850..2149, frame 1870 came after a 215x122
   render of 1850 and re-ran 1440..1869 (430 fetches, 13.6 s, about 34 GB more in Resolve).

Resolve does not create a new effect instance per render (the render queue and the viewer use the one on the clip),
and its checkpoints survive between renders; history was lost only as in 3 and 4.

The core alone (`bench_screenloop`, generated sources) never holds more than one source frame and peaks at 1.3-2.3 GB
for a cold seek to 1850 (`tests/howlaround/bench_screenloop.cpp`, "at most N held at once", "peak memory").

## What the plug-in does now

- **Resolve: Screen Loop's history is not declared** (`NtscrtEffect::getFramesNeeded`, `ofx/ntscrt_ofx.cpp`): only the
  current frame; the loop fetches its history one frame at a time, only when it re-runs, and releases each at once.
  Other hosts still get the window (they may deliver only declared frames). Digital video's stream is still declared
  everywhere. `NTSCRT_OFX_DECLARE_HISTORY=1` declares it in Resolve too (A/B), `0` never declares any history.
- **The run ahead** (`howl_engine.h`, "Running ahead"): from each window start W on, a second run of anchor W goes
  along with the one shown, on the same source pictures, one TV pass per frame, and takes over at W + B. Playing
  across a window start fetches nothing and re-runs nothing. Inside a cold seek's re-run it starts at the window
  start it passes, so the seek's 410 fetches are the only ones until the next-but-one window. It is the run a re-run
  would have made, so every picture is the same (`test_howl_loop` "playing across window starts", `test_howl_seek`
  "a far cold seek"). Cost: one extra TV pass (NTSC, Downscale, Glitch, the loop's CRT) per frame in the second half of
  each window's life, and the run-up (about 60 passes) on the window-start frame instead of on the frame B later. Off
  with checkpoints (a temporal CRT preset), never taken over from a history with stand-ins;
  `NTSCRT_SCREENLOOP_AHEAD=0` turns it off (A/B).
- **One loop per scene size** (`howlaround.cpp`, `Howlaround::Impl::Slot`; three kept, the least recently used
  dropped but never the biggest for a smaller one; Resolve used scales 1, 0.16, 0.11 and 0.056 in one session): a
  render at another render scale has its own history and leaves the full-size one alone. Each is the same
  deterministic loop, so no picture changes (`test_howl_seek` "a render at another scale in between"). Cost: the
  small loops' TV pictures and checkpoints while they are alive.
- Debug log: each render's `temporal_fetches=`, and with fetches `temporal_fetch_ms=` (time in the host's
  `fetchImage`), `temporal_held_max=`, `temporal_size=` (the fetched image's bounds and depth), `temporal_missing=`
  (fetches the host answered with no image).

## Numbers

Raw results on the dev PC: `C:/ntb/w16c-res/` (`slmeasure.py`, the headless-Resolve harness: a fresh timeline and
effect per case, Resolve's private memory sampled every 0.25 s, killed past 40 GB; `summarize.py` builds the table).
Edit-page clip (a 640x360 ProRes clip in a 1080p or UHD 30 fps project), PNG render queue job, each of the five
`presets/Screen Loop/` presets (they behave alike: ranges below). *seek* = frame 1850 alone (a cold seek), *seq* =
frames 0..299, *cross* = 1850..2149 (a cold seek, then across the window start 1920). Resolve memory = peak private
bytes during the job, growth from the job's start in brackets. Fetches = the full-size renders' temporal fetches.

| config | size | case | before: done; wall s; Resolve GB (growth); fetches | after: done; wall s; Resolve GB (growth); fetches |
|---|---|---|---|---|
| default | 1080p | seek | 5/5; 8.9-9.9; 37.1-37.3 (+32.1-32.4); 410 | 5/5; 9.4-10.4; 37.3-37.4 (+32.4-32.6); 410 |
| default | 1080p | seq | 5/5; 6.8-6.9; 5.9-6.1 (+1.0-1.2); 0 | 5/5; 3.4-4.3; 5.5-5.6 (+0.5-0.8); 0 |
| default | 1080p | cross | 5/5; 18.9-19.9; 38.0-38.3 (+31.7-32.0); 650 | 5/5; 11.3-12.9; 38.9-39.0 (+32.7-32.9); 410 |
| default | UHD | seek | 0/5, killed at 40 GB in 7-8 s | 0/5, killed at 40 GB in 7-8 s |
| default | UHD | seq | 4/5 (1 killed at 40 GB); 10.9-13.4; 10.9-11.0 (+4.1-4.3); 0 | 5/5; 9.8-13.4; 8.1-8.6 (+1.3-1.9); 0 |
| default | UHD | cross | 0/5, killed at 40 GB | 0/5, killed at 40 GB |
| cpu | 1080p | seek | 5/5; 10.4-12.4; 36.5 (+31.5-31.8); 410 | 5/5; 14.4-17.4; 36.6-36.8 (+31.7-32.0); 410 |
| cpu | 1080p | seq | 5/5; 12.8-13.4; 5.9-6.1 (+1.0-1.3); 0 | 5/5; 10.9-12.9; 5.2-5.5 (+0.4-0.6); 0 |
| cpu | 1080p | cross | 5/5; 28.4-43.4; 37.0-38.6 (+31.0-32.4); 650-1080 | 5/5; 27.4-32.4; 37.7-38.0 (+29.6-32.1); 410 |
| cpu | UHD | seek | 0/5, killed at 40 GB in 10-23 s | 0/5, killed at 40 GB in 10-18 s |
| cpu | UHD | seq | 5/5; 41.9-50.4; 10.3-10.4 (+3.7-4.3); 0 | 5/5; 36.4-45.4; 7.5-7.6 (+0.9-1.6); 0 |
| cpu | UHD | cross | 0/5, killed at 40 GB | 0/5, killed at 40 GB |

*before* = this build with `NTSCRT_OFX_DECLARE_HISTORY=1 NTSCRT_SCREENLOOP_AHEAD=0` (0.5.0-rc.5's behaviour; an
intermediate build with one loop per effect gave the same numbers except CPU *cross*, 410-840 fetches), *cpu* =
`NTSCRT_NTSC_GPU=0 NTSCRT_OFX_GPU=0 NTSCRT_FAST_TRANSFER=0`. Before, a 1080p *seq* made 3827 `getFramesNeeded`
calls (each declaring up to 300 frames), after 305. CPU *cross* before: 1080 fetches when a viewer render at
another scale came in between (problem 4). Every case held at most one host image at a time. The UHD cases that
don't finish are problem 2: Resolve keeps each fetched frame and a cold seek fetches 410.

The core alone (`bench_screenloop`, generated 8-bit sources, NTSC on the GPU / `NTSCRT_NTSC_GPU=0`): a cold seek to
1850 fetches 410 frames and holds one at a time, before and after. At 1080p it takes 3-4.5 s (GPU) / 10-13 s (CPU,
was 6-8 s), at UHD 9-12.5 s / 42-57 s (was 26-32 s): the run ahead's extra passes during the re-run, synchronous
on the CPU route. Playing on 300 frames after it (`--then 300`, across 1920): 0 fetches and worst frame 128 ms (was
240 fetches and a 1.9 s frame) at 1080p GPU; UHD GPU 0 fetches, worst 270 ms (was 240, 6.3 s); on the CPU route the
same fetches saved but no time saved in the core alone (12.8 s vs 10.3 s at 1080p: the re-run's passes overlap, the
run ahead's don't), the saving is Resolve's fetches. Frames 0..299 from a fresh effect: the same at 1080p GPU (2 s),
CPU 8-11 s (was 7-11 s: the run ahead from 240 on).

## What a plug-in can't change

A cold seek's history is N - A(N) source frames (240-479), each of which Resolve renders and keeps. Exact pictures
need all of them. The bound is the window: a shorter one fetches fewer frames, but **changes every frame from 2B
on** (they would see a shorter history than now), so it is not done; it is the user's call. Estimated from the
measured growth of Resolve's memory per distinct fetched frame (about 80 MB at 1080p, 200-330 MB at UHD):

| window B | frames fetched by a cold seek | Resolve memory, 1080p | Resolve memory, UHD |
|---|---|---|---|
| 240 (now) | 240-479 | 19-38 GB | 48-158 GB (beyond Resolve's limit: up to it) |
| 120 | 120-239 | 9.5-19 GB | 24-79 GB |
| 60 | 60-119 | 4.7-9.4 GB | 12-39 GB |
| 30 | 30-59 | 2.4-4.7 GB | 6-19 GB |

The cold-seek time scales the same way (about 10 s for 410 frames at 1080p, GPU route). The core reads B from
the feedback JSON's `window` (the tests use it); the plug-ins' controls don't set it. Not verified: how Resolve
behaves when its cache reaches its own limit (75% of RAM by default) during a UHD cold seek; the tests stop it at
40 GB, and on this 62 GB PC that limit is 46 GB.

## Preset changes and Inspector edits (2026-11-18)

User report on 0.5.0: changing Screen Loop presets in the Inspector lags Resolve far more than changing regular
presets. What it costs, step by step:

- **Every edit re-ran the loop.** Any param change (`NtscrtEffect::changedParam`, `host::cache_mask`) flushed
  `NTSCRT_CACHE_SCREENLOOP`, so the next render of the playhead frame N re-ran the loop from A(N): N - A(N) source
  frames fetched from the host (240-479 far in, N near the start), once for each render scale Resolve asks for.
  That held for edits that change nothing the loop shows: a preset writing the values the controls already have,
  choosing the preset that is already set, a Digital video edit (Digital is off while Screen Loop is on), and for
  going back to a preset tried a moment before.
- **The loop depends on almost the whole look**, not only its own settings: the crop shapes the room the camera
  films, and its TV picture runs through NTSC, Downscale, Glitch and the CRT shader (`HowlHooks`). So a CRT or
  Glitch edit has to re-run it; there is no "downstream-only" param apart from Digital video.
- **A Screen Loop preset writes 25 params, a look preset 288** (each echoed back as a plug-in edit). The writes
  are one edit block and the flush waits for the next render (`pending_flush_`), so a preset costs one re-run
  per render scale, not one per write.

Now:

- **The plug-in drops the history only when something it depends on changed** (`loop_flush_needed`): a param change
  marks it, and the next render compares the look without Digital video, plus the seed, with the last render's.
  The Screen Loop settings themselves are in the core's key for each loop (the feedback JSON), so their changes need
  no flush. With keyframes anywhere (before or now) it is dropped as before: earlier frames' looks could differ.
- **The core keeps the loops of the last few settings** (`Howlaround::Impl::Slot::parked`, four per render size,
  256 MB in all, `NTSCRT_SCREENLOOP_KEEP_MB` (0: none); the newest of each size is kept whatever its size: at UHD,
  where the TV pictures are scene-sized without Downscale, one loop alone passes the budget, which 0.5.1 threw
  away at once): a loop put aside keeps only its state at its last frame
  (`Engine::park`: its last checkpoint, no run ahead), so going back to a setting at the same playhead continues it,
  and after a render of another frame in between (Resolve renders a Fusion comp's current frame with each job)
  resumes from that checkpoint, at most 30 frames back.
  A temporal CRT preset in the loop (none of the bundled ones) keeps nothing across a settings change and resets
  the loop's CRT, as the flush did (`HowlHooks::tv_reset`).
- **A render the host aborts drops only the loop it was running** (`Howlaround::drop_current`, called by the core when
  a render's run met a source frame the host didn't give): 0.5.1 flushed all of Screen Loop on an abort
  (`NtscrtEffect::render_frame`), and headless Resolve aborts the render it started right after each preset change
  (log: "aborted by the host after 1-8 earlier frames"), so the kept loops were gone before they were needed.
- No picture changes: each kept loop is the same deterministic run (`test_howl_seek` "other settings in between,
  then back", `test_ofx` "Screen Loop: preset changes and Inspector edits ..." compares every step with a fresh
  instance's render).

Mock host (`test_ofx`, the sequence above with each plug-in write echoed back, renders of N at render scales 1 and
0.25; re-runs = renders that fetched earlier frames; `NTSCRT_BENCH_SL_SIZE=1920x1080 NTSCRT_BENCH_SL_FRAMES=1850`
for the timings):

| step (1080p) | frame 1850 before: re-runs, earlier frames fetched, ms | after | frame 100 before | after |
|---|---|---|---|---|
| Screen Loop preset B | 2, 820, 4386 | 2, 820, 4239 | 2, 200, 1202 | 2, 200, 1058 |
| the same preset again | 2, 820, 4239 | 0, 0, 34 | 2, 200, 1135 | 0, 0, 34 |
| back to preset A | 2, 820, 5548 | 0, 0, 39 | 2, 200, 1411 | 0, 0, 35 |
| Screen Loop zoom edit | 2, 820, 5683 | 2, 820, 5332 | 2, 200, 1357 | 2, 200, 1310 |
| Digital video edit | 2, 820, 5614 | 0, 0, 37 | 2, 200, 1339 | 0, 0, 35 |
| look preset (Clean CRT) | 2, 820, 6200 | 2, 820, 6337 | 2, 200, 1799 | 2, 200, 1480 |
| the same look preset again | 2, 820, 7091 | 0, 0, 41 | 2, 200, 1468 | 0, 0, 35 |

(*before* = this test against the 0.5.0 plug-in, `NTSCRT_TEST_OFX_BINARY`; ms = the two renders after the edit; the
re-runs left are the ones the edit needs.)

Headless Resolve 21.1.2 (2026-11-19, `C:/ntb/w18e-res/slpreset.py`): a Fusion comp on a clip timeline (the 640x360
parity clip resized to the project's 3840x2160 in the comp: new projects here are UHD), MediaIn -> BetterResize ->
NTSCRT -> MediaOut; each step sets inputs by script (a preset menu's SetInput runs the preset apply), then the render
queue renders the playhead frame P. Per step: earlier frames fetched and the plug-in's seconds for the renders of P
(Resolve also renders the comp's current frame with each job), from the verbose log. *0.5.0* = before, *0.5.1* = the
first fix, *now* = with the three fixes below (`bundle-fix5`).

| step | P = 100: 0.5.0 | 0.5.1 | now | P = 1850: 0.5.0 | 0.5.1 | now |
|---|---|---|---|---|---|---|
| Screen Loop preset B | 99 f, 6.5 s | 99 f, 4.8 s | 99 f, 6.5 s | 409 f, 21.9 s | 410 f, 21.8 s | 409 f, 24.0 s |
| the same preset again | 99 f, 6.5 s | 0, 0.1 s | 0, 0.2 s | 409 f, 22.1 s | 20 f, 1.0 s | 20 f, 1.1 s |
| back to preset A | 99 f, 6.5 s | 99 f, 4.8 s | **0, 0.2 s** | 429 f, 22.6 s | 410 f, 21.9 s | **20 f, 1.1 s** |
| zoom edit | 94 f, 6.4 s | 92 f, 4.5 s | 92 f, 4.6 s | 403 f, 21.3 s | 410 f, 21.1 s | 403 f, 21.3 s |
| CRT edit (in the loop) | 94 f, 6.2 s | 92 f, 4.5 s | 92 f, 4.8 s | 404 f, 21.4 s | 423 f, 21.2 s | 403 f, 21.4 s |
| look preset | 96 f, 5.5 s | 94 f, 5.2 s | 94 f, 5.3 s | 425 f, 23.9 s | 424 f, 22.8 s | 424 f, 23.5 s |
| the same look preset again | 94 f, 5.4 s | 0, 0.2 s | 0, 0.2 s | 424 f, 24.0 s | 20 f, 1.1 s | 20 f, 1.1 s |
| look preset, Screen Loop off | 0, 0.0 s | 0, 0.0 s | 0, 0.0 s | 0, 0.0 s | 0, 0.0 s | 0, 0.0 s |

(20 f at 1850: Resolve rendered the comp's current frame in between, so the loop comes back from its checkpoint at
1830.) What the log shows besides:

- Every write of a preset is echoed back during the apply itself (25 for a Screen Loop preset, 288 for a look,
  all "during our own edit"), so they cost nothing; the apply takes 14 ms (Screen Loop) / 165 ms (look).
- **Resolve aborts the render it starts right after each preset change** (with the newer setting it then renders
  again), after 1-8 earlier frames. 0.5.0 and 0.5.1 flushed all of Screen Loop on an abort, which in 0.5.1 also
  threw away the loops kept to come back to: why "back to preset A" still re-ran in 0.5.1. Fix: the core drops only
  the run that stood in pictures (`Howlaround::drop_current`; `test_ofx` "a render the host aborts drops only the
  loop it was running").
- **At UHD one loop's TV pictures pass the 256 MB set aside for kept loops** (without Downscale the TV picture is
  scene-sized: 33 MB each), so 0.5.1 dropped the kept loop at once. Fix: the newest of each size is always kept, and
  a kept loop holds only the frames it needs to go on (`Engine::park`).
- **A render of another frame in between moved a taken-back loop away from the playhead**, and 0.5.1 kept no
  checkpoint with it: fix, its last checkpoint stays (at most 30 frames re-run).
- The Digital video edit is not in the table: Resolve ignores a script's SetInput on a control its switch greys out.
- Resolve's memory: 8.4-8.6 GB at P = 100 and 11.7-12.0 GB at P = 1850 in all three builds (the fetched frames are
  the small clip resized; no change from these fixes). Resolve suite (15 tests) and parity for the screenloop and
  sizeloop groups (82 images per pair, all pairs equal) pass with these fixes; `--parity-quick` passes on 0.5.1.
