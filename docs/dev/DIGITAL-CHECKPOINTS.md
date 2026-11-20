# Digital video after a jump: checkpoints of the codec state

Digital video (`core/stages/digital/`) simulates a real codec: a predicted frame is coded against the decoder's
picture of the frame before, so frame N depends on every capture since the stream's last keyframe that arrived
(`Renderer`, `renderer.cpp`). A host renders frames in any order, so a render that isn't the next frame of the
stream the effect holds re-runs the stream from that keyframe, fetching each earlier source frame from the host
(`ntscrt_render_args.fetch_source`) and coding it. That is what makes a frame the same however it is reached
(`tests/digital`). How far back that goes, per format (30 fps clip):

| format | codes | keyframe | a cold render of a frame re-runs |
|---|---|---|---|
| Webcam (2005), MiniDV | every frame alone | every frame | 1 earlier frame (lost blocks are patched from it) |
| Video call | 15 fps | every 4 s (60 captures) | up to 59 captures (118 frames back) |
| Early web video | 15 fps | every 5 s (75 captures) | up to 74 captures (148 frames back) |
| CD-ROM (Video 1) | 12 fps | every 1.25 s (15 captures) | up to 14 captures |
| Datamosh | 30 fps | every 1 s, every one dropped | back to the start of the minute (`core/src/sim_segment.h`: up to 1,799 frames) |

Before this change the stage kept only the stream's current state, plus (since 0.5.0) a snapshot at each keyframe
a datamosh drops. So stepping back one frame re-ran the whole GOP (Datamosh: 30 captures per step), jumping back
over frames already seen re-ran them again, and three things threw away what it had:

- **A render at another render scale** (Resolve's viewer and thumbnails, 0.11-0.25, in between the full-size
  frames: docs/dev/SCREENLOOP-MEMORY.md) went on from the full-size stream's state with its smaller pictures, or
  replaced it. That was also a correctness bug: the stream key held the capture size (the Downscale width, the
  same at every scale) but not the frames' size, so the full-size frames after such a render were coded from a
  reduced-scale picture and differed from an export (`test_digital_seek` "seeks, steps back, other render scales
  ..." fails on 0.5.3 there).
- **A render the host aborts** (Resolve cancels a render it no longer needs, right after an edit): the plug-in
  flushed the whole stage (`NTSCRT_CACHE_DIGITAL`) so the stream built on stand-ins wasn't reused, and the
  snapshots went with it.
- **Another structure setting in between** (frame rate, codec, color, keyframe interval: a new stream key).

## What it does now

`Renderer` (`renderer.h`, `renderer.cpp`):

- **Checkpoints.** While a stream is coded, the codec's state after every 15th capture (`CheckpointConfig::every`)
  and after each keyframe a datamosh drops is kept, with the run's settings fingerprints since its keyframe (as the
  old snapshots were). Pictures are never changed once a codec step made them (each step makes new ones), so a
  checkpoint shares them with the stream: taking one copies no pixels. Not taken at a keyframe that arrives (the
  run restarts there anyway) nor from a run in which a stand-in took a missing frame's place.
- **Resuming.** A render that can't simply go on (the same frame, or the next few) finds where a run from scratch
  would start (the last keyframe that arrived, as before; `frames_needed.h` mirrors it) and goes on instead from
  the latest of: the stream's own state, the latest checkpoint at or before the frame, the trail (below), whose
  settings fingerprints still hold for every capture since its keyframe. A keyframed setting edited before the
  frame makes them fail, so it re-runs (`test_digital_seek` "an edited keyframe ..."). Without keyframes the
  check is one fingerprint per capture. The stream also goes on from its own state when the frame is more than
  two GOPs ahead with no keyframe that arrived in between (Datamosh; it used to re-run from the keyframe).
- **The trail.** A re-run also keeps the states of the up to 14 captures just before the frame it was for (the
  frames since the checkpoint were coded anyway), so stepping back one frame at a time after a jump re-runs
  nothing until the step before that checkpoint. It replaces the last re-run's, within a quarter of the budget.
- **One stream per frame size.** The stream key holds the frames' size; when another key comes in (a render at
  another render scale, other structure settings), the stream being coded is kept as a checkpoint at its last
  capture, so going back to it goes on from there. Each size codes its own stream, the same as an export at that
  size would.
- **Missing frames.** A render that met an earlier frame the host didn't give (an abort) drops only the stream's
  state at the end (the next render re-runs, from a checkpoint made before the miss); the plug-in no longer flushes
  the stage on an abort (`ofx/ntscrt_ofx.cpp`, `render_frame`).
- **Only the frame asked for is shown**: the captures before it in a re-run are coded but no longer turned back
  into a picture (`render()`, the deblocking filter and the color conversion), and a state restored at exactly the
  frame is shown with its own deblocking step (it used to take the quality's unsqueezed step: a frame shown
  straight from a checkpoint could differ with Deblocking and Fixed bitrate on).
- **Memory**: `NTSCRT_DIGITAL_CHECKPOINT_MB` (default 256 MB per effect, `0` = none, docs/ENVIRONMENT.md), the least
  recently used (taken or resumed from) going first; the trail on top (a quarter of that at most). Measured
  (`bench_digital`, after a cold seek to frame 1799 of a 1080p clip: `digital_checkpoints=` count:MB + trail):
  Video call 0.7 MB per checkpoint (data loss: the decoder's picture differs from the encoder's), Early web video
  0.36 MB, CD-ROM 0.4 MB, Datamosh 1.5 MB (480x272, both pictures: a whole minute is 119 checkpoints, 178 MB, plus
  14 in the trail, 21 MB). With Downscale off the capture is the picture's size up to 1280 wide: about 11 MB each,
  so about 23 checkpoints (345 captures) fit; past the budget the oldest go and a jump further back re-runs from
  the keyframe as before. Webcam and MiniDV keep none (each frame needs only the one before).
- Timings note: ` digital_captures=N digital_from=C digital_checkpoints=count:MB+count:MB` (`C`: the capture the
  run went on from, -1 from a keyframe; the counts: checkpoints, then the trail).

The pictures are the same as before (and as playing from the start), byte for byte: every format on the plug-in's
default look and alone, with data loss and deblocking (`tests/digital/test_digital_seek.cpp`, through the C ABI:
cold seeks, jumps back and forth, stepping back across a keyframe, renders at render scale 0.5 in between, other
settings in between and back, an aborted render; each compared with frames played in order on another instance,
or with a fresh instance's cold render); `test_digital` (the renderer's own jumped-to = played checks, unchanged);
`test_ofx` "Digital video: after a jump into a clip ..." (the mock host, compared with a fresh effect's render).

What it can't change: **the first render of a frame whose stream was never coded** still re-runs it from the
keyframe: exact pictures need every capture since then, and the checkpoints only exist for captures already coded.
With Datamosh at 1 every keyframe is dropped and the stream reaches back to the minute's start (up to 1,799
frames: 28 s in the core alone at 1080p, and in Resolve each fetched frame is rendered and kept by Resolve,
docs/dev/SCREENLOOP-MEMORY.md). Once those frames were coded, any jump within them re-runs at most 14 captures
(`test_digital_seek` "with every keyframe dropped ..."). A render at another render scale codes its own stream
once (Resolve's viewer: frames that size are small, but a Datamosh viewer render far into a minute still fetches
its minute once). The plug-ins still declare the stream back to its keyframe in `getFramesNeeded` (unchanged).

## Numbers

### The core (`bench_digital`, mock host through the C ABI)

`bench_digital --format <id> --seek 1850,108900 --then 10 --back 10 --scale 0.25` (1920x1080 generated frames,
30 fps, the plug-in's default look plus the format: NTSC and CRT on; RTX 5090, Ryzen 9 9950X3D). For each frame a
fresh instance renders it cold (after a warm-up render with Digital video off), plays on 10 frames, steps back 10
frames one at a time from it, then jumps to N - 60 and N - 300, then renders N at render scale 0.25 and N + 1 full
size. Before = 0.5.3's renderer with the same bench. Raw: `C:/ntb/w23j-base/bench_before.txt`,
`bench_after2.txt` on the dev PC.

| format, frame | cold jump: fetches, ms (before / after) | 10 steps back: fetches, captures, ms per step (before / after) |
|---|---|---|
| Webcam, 1850 | 1, 67 / 1, 54 | 5, 10, 16 / 5, 10, 13 |
| Video call, 1850 | 25, 322 / 25, 273 | 0, 115, 79 / 0, 0, 5.6 |
| Video call, 108900 | 30, 419 / 30, 329 | 0, 140, 102 / 0, 0, 5.2 |
| MiniDV, 1850 | 1, 83 / 1, 66 | 10, 20, 32 / 10, 20, 25 |
| Early web video, 1850 | 25, 293 / 25, 242 | 0, 115, 65 / 0, 0, 5.7 |
| Early web video, 108900 (a keyframe) | 0, 57 / 0, 51 | 365, 365, 424 / 75, 75, 73 |
| CD-ROM, 1850 | 5, 104 / 5, 75 | 0, 14, 14 / 0, 0, 3.7 |
| Datamosh, 1850 | 50, 651 / 50, 520 | 0, 145, 120 / 0, 0, 3.9 |
| Datamosh, 108900 (01:00:30:00) | 900, 10774 / 900, 9222 | 0, 245, 159 / 0, 0, 6.2 |

(Fetches are host fetches; the core's source cache keeps the last 65 frames, so steps back within them fetch
nothing either way: the time is the codec re-run. Early web at 108900 lands on a keyframe: the steps back are
the GOP before it, never coded, so the first step re-runs it once.) Further, Datamosh: a jump from 1840 back to
1790 re-ran 1,790 captures before and after (frames 0..1789 were never coded: the first render of them), to 1550
afterwards 20 / 5 captures (241 / 47 ms); the render at scale 0.25 and then N + 1 full size: before 75 ms + 14 ms
(the full-size frame coded on from the reduced-scale stream: wrong pictures), after 193 ms (its own stream, 50
fetches) + 94 ms (from the full-size checkpoint, 6 captures).

### The plug-in in a mock host (`test_ofx`)

RESULTS_MOCK

### Headless Resolve

RESULTS_RESOLVE
