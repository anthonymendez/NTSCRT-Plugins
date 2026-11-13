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
   1440..1850), but Resolve's private memory still grows by about 80 MB per distinct 1080p frame fetched and about
   330 MB per UHD frame (2.5x a float RGBA frame), and doesn't give it back while the project is open (a second
   timeline, a second effect: still held; fetching the same frames again adds nothing). The plug-in holds one host
   image at a time and releases it as soon as it is converted (`temporal_held_max=1` in the debug log;
   `RenderCtx::fetch` / `release_source`), so this is Resolve's own cache. It is capped by Resolve's *Preferences →
   Memory and GPU → Limit Resolve memory usage* (75% of RAM by default, 46 GB here); the tests stop Resolve at 40 GB,
   so whether it evicts cleanly at its limit was not observed.
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

See the before/after table in the commit / report (`C:/ntb/w16c-res/` on the dev PC). Cold seek: the number of source
frames fetched is unchanged (410 for frame 1850: exactness needs all of them).

## What a plug-in can't change

A cold seek's history is N - A(N) source frames (240-479), each of which Resolve renders and keeps. Exact pictures
need all of them. The bound is the window: a shorter one would fetch fewer frames but **changes every picture after
frame 2B** (and the 2B frames that match upstream's sequential render), so it is not done; it's the user's call. Per
cold seek, measured growth of Resolve's memory per fetched frame (80 MB at 1080p, 330 MB at UHD):

| window B | frames fetched by a cold seek | Resolve memory, 1080p | Resolve memory, UHD |
|---|---|---|---|
| 240 (now) | 240-479 | 19-38 GB | 79-158 GB (Resolve's limit) |
| 120 | 120-239 | 10-19 GB | 40-79 GB |
| 60 | 60-119 | 5-10 GB | 20-39 GB |
| 30 | 30-59 | 2.4-4.7 GB | 10-19 GB |

(The feedback JSON's `window` already sets it; the Resolve UI has no control for it.)
