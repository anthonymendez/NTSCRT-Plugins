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
  frames: docs/dev/SCREENLOOP-MEMORY.md) replaced the stream (Resolve's jobs alternate the two: a GOP re-run at each
  switch, measured below), or, when the capture size came out the same at both scales, went on from it. That was
  also a correctness bug: the stream key held the capture size (from the Downscale width) but not the frames'
  size, so full-size frames after such a render could be coded from reduced-scale pictures and differ from an
  export (`test_digital_seek` "seeks, steps back, other render scales ..." fails on 0.5.3 at scale 0.5).
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
`test_ofx` "Digital video: after a jump into a clip ..." (the mock host, compared with a fresh effect's render). The
headless-Resolve parity suite's Digital video cases (`run_resolve_tests.py --headless --parity --parity-digital`,
2026-11-20, every config: 182 images per pair) pass: the CPU / GPU routes equal byte for byte, host GPU frames
within the usual rule, the CPU reference again 0 different.

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

`test_ofx` "Digital video: after a jump into a clip ..." (128x72 frames, 30 fps, every frame the same picture, the
plug-in's default look plus the format from the Preset menu, one fresh effect per row; the core's source cache
serves earlier frames it already converted, so the steps back fetch nothing from the host either way: the
difference is the codec's re-run). Earlier frames fetched from the host, ms. Before = 0.5.3's plug-in
(`NTSCRT_TEST_OFX_BINARY`), after = this branch.

| format, frame | jump (before / after) | 5 steps back | viewer render at 0.25 | full size after it | aborted, then again |
|---|---|---|---|---|---|
| Webcam, 1850 | 1, 92 / 1, 70 | 3, 56 / 3, 46 | 2, 14 / 2, 10 | 0, 8 / 1, 9 | 0, 3 / 0, 4 |
| Video call, 1850 | 25, 146 / 25, 145 | 0, 263 / 0, 31 | 3, 19 / 26, 90 | 0, 8 / 1, 19 | 0, 3 / 0, 4 |
| Video call, 108900 | 30, 184 / 30, 182 | 0, 310 / 0, 38 | 3, 16 / 31, 102 | 0, 8 / 0, 6 | 0, 5 / 0, 6 |
| MiniDV, 1850 | 1, 83 / 1, 64 | 5, 118 / 5, 110 | 1, 20 / 1, 18 | 0, 13 / 1, 20 | 0, 4 / 0, 3 |
| Early web video, 1850 | 25, 160 / 25, 104 | 0, 172 / 0, 28 | 3, 14 / 26, 60 | 0, 5 / 1, 14 | 0, 6 / 0, 7 |
| Early web video, 108900 (a keyframe) | 0, 128 / 0, 68 | 222, 638 / 75, 150 | 3, 16 / 1, 6 | 0, 6 / 1, 9 | 11, 130 / 0, 3 |
| CD-ROM, 1850 | 5, 106 / 5, 67 | 0, 53 / 0, 26 | 2, 20 / 6, 10 | 0, 7 / 1, 8 | 10, 18 / 10, 11 |
| Datamosh, 1850 | 50, 239 / 50, 198 | 0, 358 / 0, 29 | 5, 41 / 51, 144 | 0, 8 / 2, 31 | 0, 6 / 0, 4 |
| Datamosh, 108900 | 900, 3742 / 900, 3127 | 0, 569 / 0, 46 | 5, 27 / 901, 2704 | 0, 8 / 1, 17 | 0, 7 / 7, 31 |

The before plug-in fails the test's picture checks: its full-size frame after the reduced-scale render went on
from the reduced-scale stream. The viewer column is the price of that fix: a render at another scale codes its
own stream (once), where 0.5.3 borrowed the full-size one.

### Headless Resolve

`C:/ntb/w23j-res/dvmeasure.py` on the dev PC (headless Resolve 21.1.2, RTX 5090; raw: `C:/ntb/w23j-res/before/`,
`after/`): a 1080p 30 fps project of its own, the Edit page, NTSCRT on a 640x360 ProRes clip (130 s) or a 1 h
H.264 clip, the format's values through the dev copy's test preset file, a fresh timeline per row. Render-queue
jobs on the same effect, one frame each: N (the jump), N - 1, N - 2, N - 3, then N + 1. N = 1850, and 108050 in the
1 h clip (01:00:01:20, just past the hour: a Datamosh stream from 108000). Per job: earlier frames fetched from
Resolve and the plug-in's ms over the job's full-size renders (Resolve renders the job's frame several times, 5-7
renders per job, and once the playhead's frame at scale 0.11), from the verbose log. Every job's wall time is
0.85-0.95 s whatever the plug-in does (Resolve's own job overhead) except the jumps (1.4-1.9 s for the GOP formats).

| format | where | jump: fetches, ms (before / after) | N-1 | N-2 | N-3 | N+1 |
|---|---|---|---|---|---|---|
| Webcam (2005) | 1850 | 1, 42 / 1, 38 | 1, 39 / 1, 37 | 0, 18 / 0, 48 | 1, 41 / 1, 87 | 1, 38 / 1, 84 |
| Video call | 1850 | 25, 602 / 25, 642 | 0, 297 / 0, 70 | 0, 168 / 0, 70 | 0, 282 / 0, 64 | 1, 177 / 1, 92 |
| MiniDV | 1850 | 1, 65 / 1, 72 | 1, 70 / 1, 96 | 1, 71 / 1, 64 | 1, 68 / 1, 97 | 1, 68 / 1, 119 |
| Early web video | 1850 | 25, 516 / 25, 564 | 0, 238 / 0, 62 | 0, 134 / 0, 102 | 0, 224 / 0, 115 | 1, 143 / 1, 43 |
| CD-ROM (Video 1) | 1850 | 5, 144 / 5, 150 | 0, 42 / 0, 34 | 0, 23 / 0, 70 | 0, 59 / 0, 87 | 1, 53 / 1, 37 |
| Datamosh | 1850 | 50, 1122 / 50, 1170 | 0, 267 / 0, 46 | 0, 243 / 0, 39 | 0, 211 / 0, 130 | 1, 153 / 1, 57 |
| Webcam (2005) | 1 h | 1, 83 / 1, 62 | 1, 87 / 1, 38 | 0, 53 / 0, 51 | 1, 81 / 1, 81 | 1, 77 / 1, 88 |
| Video call | 1 h | 25, 898 / 25, 610 | 0, 301 / 0, 71 | 0, 245 / 0, 74 | 0, 406 / 0, 67 | 1, 277 / 1, 106 |
| MiniDV | 1 h | 1, 76 / 1, 92 | 1, 107 / 1, 101 | 1, 105 / 1, 112 | 1, 83 / 1, 115 | 1, 70 / 1, 110 |
| Early web video | 1 h | 25, 580 / 25, 508 | 0, 312 / 0, 63 | 0, 142 / 0, 115 | 0, 252 / 0, 121 | 1, 159 / 1, 46 |
| CD-ROM (Video 1) | 1 h | 5, 103 / 5, 168 | 0, 49 / 0, 93 | 0, 20 / 0, 76 | 0, 35 / 0, 35 | 1, 57 / 1, 38 |
| Datamosh | 1 h | 50, 1066 / 50, 1115 | 0, 242 / 0, 45 | 0, 223 / 0, 132 | 0, 218 / 0, 33 | 1, 142 / 1, 131 |

The table counts only the renders after each job started. Resolve also renders the job's frame at its viewer's
scale (0.11 here) when the job's marks are set, and the frame before again at full size, so the plug-in sees,
per job: frame N at 0.11, the previous job's frame at full size, then N at full size. On one stream that
alternation re-ran the GOP at each switch (the 0.11 picture's capture size rounds to another height, 320x182
against 320x180, so 0.5.3 started over rather than mixing them); with one stream per size and the checkpoints it
goes on. The whole case (the 5 jobs, every render of the effect, 38-52 renders) from the same log:

| format | where | renders' captures coded (before / after) | earlier frames fetched | plug-in ms |
|---|---|---|---|---|
| Webcam (2005) | 1850 | 9 / 11 | 4 / 4 | 1128 / 1195 |
| Video call | 1850 | 306 / 91 | 52 / 51 | 1944 / 1143 |
| MiniDV | 1850 | 33 / 33 | 10 / 10 | 549 / 676 |
| Early web video | 1850 | 306 / 91 | 52 / 51 | 1535 / 1037 |
| CD-ROM (Video 1) | 1850 | 45 / 35 | 12 / 11 | 454 / 493 |
| Datamosh | 1850 | 312 / 128 | 121 / 101 | 2399 / 1697 |
| Webcam (2005) | 1 h | 9 / 11 | 4 / 4 | 447 / 403 |
| Video call | 1 h | 306 / 91 | 52 / 51 | 2683 / 1150 |
| MiniDV | 1 h | 33 / 33 | 10 / 10 | 712 / 777 |
| Early web video | 1 h | 306 / 91 | 52 / 51 | 1743 / 1008 |
| CD-ROM (Video 1) | 1 h | 45 / 35 | 12 / 11 | 398 / 515 |
| Datamosh | 1 h | 332 / 127 | 121 / 101 | 2329 / 1685 |

So in Resolve the jump itself costs the same (the frames since the keyframe, fetched once per render size), and
after it the GOP formats code a third as many captures and the plug-in spends 30-57% less time; the job wall times
don't move (Resolve's own overhead, about 0.9 s a job, dominates at these sizes). Webcam and MiniDV are unchanged
(each frame reads its one earlier frame). Resolve's peak private memory was the same in both runs (4.7-12 GB over
the session, growing with the frames fetched). Not measured in Resolve: a Datamosh jump far into a minute (e.g.
107980: 1,780 frames; at 1080p Resolve keeps each fetched frame, about 80 MB each, docs/dev/SCREENLOOP-MEMORY.md,
so it would pass 40 GB before and after alike), UHD projects, and the GUI viewer's own renders while scrubbing.
