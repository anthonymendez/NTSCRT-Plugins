#!/usr/bin/env python3
"""GPU-vs-CPU parity suite inside headless DaVinci Resolve (run_resolve_tests.py --parity / --parity-quick).

Renders every bundled preset, every effect stage on its own, every NTSC setting and the key options of the
other stages through the plug-in in one headless Resolve session per configuration, and compares the frames:

  cpu   NTSCRT_NTSC_GPU=0 NTSCRT_DIGITAL_GPU=0 NTSCRT_OFX_GPU=0 NTSCRT_FAST_TRANSFER=0
                                                                  the reference (0.5.0-rc.3's routes)
  gpu   nothing set                                               the default: host GPU frames, NTSC and Digital
                                                                  video on the GPU
  mixa  NTSCRT_OFX_GPU=0                                          CPU frames, NTSC and Digital video on the GPU
                                                                  (AMD / Intel GPUs)
  mixb  NTSCRT_NTSC_GPU=0                                         host GPU frames, ntsc-rs (so Digital video on
                                                                  the CPU too: ntsc-rs needs the picture there)
  cpu2  as cpu, a subset of the cases                             Resolve's own session-to-session noise

Pairs and their rule (docs/dev/HOST-GPU.md, docs/dev/NTSC-GPU.md, docs/dev/DIGITAL-GPU.md):
  mixa vs cpu, gpu vs mixb   only the NTSC and Digital video routes differ: every value equal
  mixb vs cpu, gpu vs cpu    host GPU frames: each channel within 1/255 and at most 0.2% of values different
  cpu2 vs cpu                noise: reported (expected 0)
8-bit and half-float Fusion sources against a CPU-frames session are informational: with GPU render offered,
Resolve's Fusion page hands the plug-in float images whatever the tools' depth (HOST-GPU.md).

Where the frames come from (all 16-bit PNGs through the render queue; the timelines stay 640x360, other sizes
are projects of their own made at that size before they have timelines):
  clip     a Fusion comp on a 130 s ProRes clip (MediaIn -> ChangeDepth float32 -> NTSCRT), frames 0, 75, 1850
  hour     a Fusion comp on a 1 h clip, frame 107980 (59:59 into the clip)
  title    a Fusion composition: Text+ over transparency -> NTSCRT -> Merge over a background (alpha modes)
  edit     the Edit page: NTSCRT on the 130 s clip (params through the dev copy's NTSCRT_OFX_TEST_PRESET_FILE),
           also with the clip zoomed / panned / cropped (transparent borders: Clip mode's hole filling)
  edit1h   the Edit page on the 1 h clip, frame 107980
  1080p / uhd  projects of that size: the Edit page and a Fusion comp on the clip
Each session also renders the sources without the effect (they must be equal across sessions) and a 16-bit
PNG still round trip (the intermediate is lossless).

The source clips are made with ffmpeg (needed on PATH) into <out>/_src the first time.
Results: <out>/parity.json, parity.csv, parity.md, one folder per session (frames, ntscrt-ofx.log, session.json).
"""
import array
import copy
import csv
import datetime
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

import run_resolve_tests as rrt

ROOT = rrt.ROOT
ENV_KEYS = ("NTSCRT_NTSC_GPU", "NTSCRT_DIGITAL_GPU", "NTSCRT_OFX_GPU", "NTSCRT_FAST_TRANSFER", "NTSCRT_FILL_GPU")
CPU_ENV = {"NTSCRT_NTSC_GPU": "0", "NTSCRT_DIGITAL_GPU": "0", "NTSCRT_OFX_GPU": "0", "NTSCRT_FAST_TRANSFER": "0"}
# "digital": the Digital video route its frames must log (digital_route=; DIGITAL-GPU.md). With ntsc-rs
# (mixb) Digital video stays on the CPU where NTSC is on (ntsc-rs needs the picture there), on the GPU where
# it's off ("cpu-if-ntsc").
CONFIGS = {
    "cpu": {"env": CPU_ENV, "frames": "cpu", "ntsc": "cpu", "digital": "cpu",
            "what": "reference: CPU frames, ntsc-rs, CPU Digital video, rc.3 transfers"},
    "gpu": {"env": {}, "frames": "gpu", "ntsc": "gpu", "digital": "gpu",
            "what": "default: host GPU frames, NTSC and Digital video on the GPU"},
    "mixa": {"env": {"NTSCRT_OFX_GPU": "0"}, "frames": "cpu", "ntsc": "gpu", "digital": "gpu",
             "what": "CPU frames, NTSC and Digital video on the GPU"},
    "mixb": {"env": {"NTSCRT_NTSC_GPU": "0"}, "frames": "gpu", "ntsc": "cpu", "digital": "cpu-if-ntsc",
             "what": "host GPU frames, ntsc-rs, CPU Digital video (with NTSC)"},
    "cpu2": {"env": CPU_ENV, "frames": "cpu", "ntsc": "cpu", "digital": "cpu", "what": "the reference again (noise subset)",
             "subset": "noise"},
}
# (session, against, rule)
PAIRS = [("mixa", "cpu", "exact"), ("gpu", "mixb", "exact"), ("mixb", "cpu", "hostgpu"), ("gpu", "cpu", "hostgpu"),
         ("cpu2", "cpu", "noise")]
TOL_MAX16 = 257          # 1/255 of 65535 (HOST-GPU.md: within 1/255 + 1e-5)
TOL_FRACTION = 0.002     # at most 0.2% of the values different
CLIP_FRAMES = 3900       # 130 s at 30 fps
HOUR_FRAME = 107980      # 59:59.3 into the 1 h clip
HOUR_FRAMES = 108600     # 1 h 20 s


# ------------------------------------------------------------------ cases ---
def schema():
    return json.loads((ROOT / "core/params/schema.json").read_text(encoding="utf-8"))


def host(pid):
    return pid.replace(".", "_")


def menu_orders(sch):
    """(looks menu, Screen Loop menu) as the plug-in builds them (hostparams/src/params.cpp parse_presets)."""
    src = (ROOT / "hostparams/src/params.cpp").read_text(encoding="utf-8")
    block = src[src.index("released_preset_order() {"):]
    released = re.findall(r'"([^"]+)"', block[:block.index("};")])
    looks, loops = [], []
    for f in sorted((ROOT / "presets").rglob("*.json")):
        rel = f.relative_to(ROOT / "presets").with_suffix("").as_posix()
        kind = json.loads(f.read_text(encoding="utf-8")).get("kind")
        (loops if kind == "ntscrt-video-feedback" else looks).append(rel)
    looks += ["Digital video/" + d["name"] for d in sch.get("digital_formats", [])]

    def key(n):
        return (released.index(n) if n in released else len(released), n.encode())
    return sorted(looks, key=key), sorted(loops, key=key)


def slug(s):
    return re.sub(r"[^A-Za-z0-9]+", "-", s).strip("-").lower()


def strong(p):
    """Strong values to try for one schema param (choices: every other option, as indices)."""
    t, d = p["type"], p.get("default")
    if t == "bool":
        return [0 if d else 1]
    if t == "choice":
        return [i for i, o in enumerate(p["options"]) if o["value"] != d]
    lo, hi = p.get("min"), p.get("max")
    if lo is None or hi is None or hi <= lo:
        return []
    v = lo + 0.8 * (hi - lo)
    if d is not None and abs(v - d) < 0.1 * (hi - lo):
        v = lo + 0.25 * (hi - lo)
    return [int(round(v)) if t == "int" else round(v, 6)]


def choice_index(p, value):
    for i, o in enumerate(p.get("options", [])):
        if o["value"] == value or o.get("label") == value:
            return i
    return None


def path_overrides(sch, pointers):
    """{json pointer: value} (a Digital video format's "applies", nested dicts allowed) -> {host param: number}."""
    by_path = {p["path"]: p for p in sch["params"]}
    out = {}

    def put(ptr, v):
        if isinstance(v, dict):
            for k, vv in v.items():
                put(f"{ptr}/{k}", vv)
            return
        p = by_path.get(ptr)
        if not p:
            return
        if p["type"] == "choice":
            i = choice_index(p, v)
            if i is not None:
                out[host(p["id"])] = i
        elif p["type"] == "bool":
            out[host(p["id"])] = 1 if v else 0
        else:
            out[host(p["id"])] = v
    for ptr, v in pointers.items():
        put(ptr, v)
    return out


def loop_overrides(sch, fb):
    by_path = {p["path"]: p for p in sch["params"]}
    out = {"screenloop_enabled": 1}
    for k, v in fb.get("values", {}).items():
        p = by_path.get(f"fb:/values/{k}")
        if p:
            out[host(p["id"])] = (1 if v else 0) if p["type"] == "bool" else v
    if "seconds" in fb:
        out["screenloop_seconds"] = fb["seconds"]
    return out


ALL_OFF = {"crop_enabled": 0, "downscale_enabled": 0, "digital_enabled": 0, "ntsc_enabled": 0, "glitch_enabled": 0,
           "crt_enabled": 0, "screenloop_enabled": 0}
NTSC_KEEP_ON = ("ntsc.enabled", "ntsc.scale_settings", "ntsc.scale_with_video_size")
NTSC_ZERO = ("ntsc.snow_intensity", "ntsc.chroma_phase_error", "ntsc.chroma_phase_noise_intensity",
             "ntsc.chroma_delay_horizontal", "ntsc.chroma_delay_vertical", "ntsc.luma_smear", "ntsc.composite_preemphasis",
             "ntsc.vhs_chroma_loss")


def build_cases(quick=False):
    """Every case of the suite. Each: id, group, where, frames, params ({host input: value}), optional menu
    ([input, index, name]), look (Edit page: a bundled look name), source (clip sources), tags, covers
    (schema param ids this case sets on their own)."""
    sch = schema()
    P = {p["id"]: p for p in sch["params"]}
    parents = {g["id"]: g.get("parent") for g in sch["groups"]}
    looks, loops = menu_orders(sch)
    fmts = {"Digital video/" + d["name"]: d for d in sch.get("digital_formats", [])}
    files = sorted((ROOT / "presets").rglob("*.json"))
    frames = [0, 1850] if quick else [0, 75, 1850]
    cases = []

    def add(cid, group, params=None, where="clip", menu=None, tags=(), covers=(), look=None, source="f32", fr=None,
            animated=False, xform=False):
        cases.append({"id": cid, "group": group, "where": where, "params": dict(params or {}), "menu": menu,
                      "look": look, "source": source, "frames": list(fr or frames), "tags": list(tags),
                      "covers": list(covers), "animated": animated, "xform": xform})

    def is_animated(name):
        f = ROOT / "presets" / f"{name}.json"
        if not f.exists():
            return False
        return len(((json.loads(f.read_text(encoding="utf-8")).get("timeline") or {}).get("keys") or [])) >= 2

    # 1. Presets: every file under presets/ (looks and Screen Loop) and every Digital video format, from the menus.
    quick_presets = {"Mild VHS", "Glitch pop", "Clean CRT", "CD-ROM (low quality)", "Digital video/MiniDV",
                     "Screen Loop/Bottom to top"}
    for i, name in enumerate(looks):
        if quick and name not in quick_presets:
            continue
        add(f"preset-{slug(name)}", "preset", menu=["host_preset", i + 1, name], tags=("noise", "hour"),
            animated=is_animated(name))
    for i, name in enumerate(loops):
        if quick and name not in quick_presets:
            continue
        add(f"preset-{slug(name)}", "preset", menu=["host_screenloop_preset", i + 1, name], tags=("noise", "hour"))
    expected = {"files": len(files), "looks_from_files": len([n for n in looks if n not in fmts]), "loops": len(loops),
                "digital_formats": len(fmts)}
    if expected["looks_from_files"] + expected["loops"] != expected["files"]:
        raise RuntimeError(f"preset count mismatch: {expected}")
    # CRT shader presets, over the default look and alone.
    crt = P["crt.preset"]
    for i, o in enumerate(crt["options"]):
        if quick and o["value"] not in ("royale", "glow_gauss"):
            continue
        add(f"crt-{o['value']}-over-default", "crt", {"crt_preset": i}, covers=["crt.preset"], tags=("noise",))
        # sim keeps a feedback (docs/dev/CRT.md): every sim case goes in the noise session too, which checks
        # that a frame is the same in another session (another render order).
        temporal = ("noise",) if o["value"] == "sim" else ()
        add(f"crt-{o['value']}-alone", "crt", dict(ALL_OFF, crt_enabled=1, crt_preset=i), covers=["crt.preset"],
            tags=temporal)
        mine = [p for p in sch["params"] if p["id"].startswith(f"crt.{o['value']}.")]
        vals, cov = {}, []
        for p in mine:
            s = strong(p)
            if s:
                vals[host(p["id"])] = s[0]
                cov.append(p["id"])
        if not quick:
            add(f"crt-{o['value']}-all-params-strong", "crt", dict(ALL_OFF, crt_enabled=1, crt_preset=i, **vals), covers=cov,
                tags=temporal)

    # 2. Stages on their own from the default-off state, the default look, everything off.
    add("default-look", "stage", tags=("noise", "hour"))
    add("all-off", "stage", ALL_OFF, tags=("noise", "hour"))
    for st in ("crop", "downscale", "ntsc", "digital", "glitch", "crt", "screenloop"):
        add(f"stage-{st}-alone", "stage", dict(ALL_OFF, **{f"{st}_enabled": 1}), covers=[f"{st}.enabled".replace("crt.", "crt.")],
            tags=("noise", "hour"))
    add("stage-ntsc-digital-screenloop", "stage", {"digital_enabled": 1, "screenloop_enabled": 1}, tags=("noise", "hour"))

    # NTSC settings one at a time: NTSC on with every optional effect off, then one setting at a strong value
    # (its parent switches on).
    ntsc_base = dict(ALL_OFF, ntsc_enabled=1)
    for p in sch["params"]:
        if p["id"].startswith("ntsc.") and p["type"] == "bool" and p["id"] not in NTSC_KEEP_ON:
            ntsc_base[host(p["id"])] = 0
    for pid in NTSC_ZERO:
        ntsc_base[host(pid)] = 0
    add("ntsc-isolated-base", "ntsc", ntsc_base, tags=("noise",))
    quick_ntsc = {"ntsc.head_switching", "ntsc.vhs_tape_speed", "ntsc.snow_intensity", "ntsc.use_field", "ntsc.ringing"}

    def with_parents(params, p):
        g = p.get("group")
        while g:
            if g in P and P[g]["type"] == "bool":
                params[host(g)] = 1
            g = parents.get(g)
        return params

    for p in sch["params"]:
        if not p["id"].startswith("ntsc.") or p["id"] == "ntsc.enabled":
            continue
        if quick and p["id"] not in quick_ntsc:
            continue
        for k, v in enumerate(strong(p)):
            if p["type"] == "bool" and host(p["id"]) in ntsc_base and ntsc_base[host(p["id"])] == 0:
                v = 1
            prm = with_parents(dict(ntsc_base), p)
            prm[host(p["id"])] = v
            label = p["options"][v]["value"] if p["type"] == "choice" else v
            add(f"ntsc-{host(p['id'])[5:]}-{slug(str(label))}", "ntsc", prm, covers=[p["id"]])

    # Glitch, Digital video, Screen Loop, Downscale, Crop: each option at a strong value.
    stage_bases = {
        "glitch": dict(ALL_OFF, glitch_enabled=1),
        "digital": dict(ALL_OFF, digital_enabled=1),
        "screenloop": {"screenloop_enabled": 1},   # over the default look: NTSC inside the loop
        "downscale": dict(ALL_OFF, downscale_enabled=1),
        "crop": dict(ALL_OFF, crop_enabled=1, crop_scale=2.0),
    }
    quick_opts = {"glitch.vertical_hold", "glitch.dropouts", "digital.codec", "digital.datamosh", "screenloop.delay",
                  "screenloop.key", "downscale.method", "crop.ratio"}
    for stage, base in stage_bases.items():
        if not quick:
            add(f"{stage}-base", stage, base)
        for p in sch["params"]:
            if not p["id"].startswith(stage + ".") or p["id"] == f"{stage}.enabled":
                continue
            if quick and p["id"] not in quick_opts:
                continue
            for v in strong(p):
                prm = dict(base)
                prm[host(p["id"])] = v
                label = p["options"][v]["value"] if p["type"] == "choice" else v
                add(f"{stage}-{host(p['id'])[len(stage) + 1:]}-{slug(str(label))}", stage, prm, covers=[p["id"]])
    add("digital-over-default-look", "digital", {"digital_enabled": 1}, tags=("noise",))
    if not quick:
        add("digital-datamosh-over-default", "digital", {"digital_enabled": 1, "digital_codec": 1, "digital_datamosh": 0.8})

    # 3. Formats: 8-bit and half-float Fusion sources, a source smaller than the frame.
    fmt_cases = [("default", {}, None), ("all-off", ALL_OFF, None), ("ntsc-alone", dict(ALL_OFF, ntsc_enabled=1), None),
                 ("clean-crt", {}, ["host_preset", looks.index("Clean CRT") + 1, "Clean CRT"]),
                 ("minidv", {}, ["host_preset", looks.index("Digital video/MiniDV") + 1, "Digital video/MiniDV"]),
                 ("bottom-to-top", {}, ["host_screenloop_preset", loops.index("Screen Loop/Bottom to top") + 1,
                                        "Screen Loop/Bottom to top"])]
    for srcname in ("i8", "f16", "small"):
        for lab, prm, menu in fmt_cases:
            if quick and lab not in ("default", "minidv"):
                continue
            if srcname == "small" and lab in ("all-off", "clean-crt"):
                continue
            add(f"{srcname}-{lab}", "format", prm, menu=menu, source=srcname, tags=("depth",) if srcname != "small" else ())

    # Alpha: a title over transparency, both Transparent-areas modes (Clip fills the holes: on the GPU with host GPU
    # frames, fill_route=gpu, so the path stays gpu-direct / gpu-download; route_problems checks it).
    title_frames = [0, 149] if quick else [0, 75, 149]
    for lab, prm, menu in (("clip", {"alpha_mode": 0}, None), ("spill", {"alpha_mode": 1}, None),
                           ("all-off-clip", dict(ALL_OFF, alpha_mode=0), None),
                           ("screenloop-clip", {"screenloop_enabled": 1, "alpha_mode": 0}, None),
                           ("digital-clip", {"digital_enabled": 1, "alpha_mode": 0}, None),
                           ("mild-vhs-spill", {"alpha_mode": 1}, ["host_preset", looks.index("Mild VHS") + 1, "Mild VHS"])):
        if quick and lab not in ("clip", "spill"):
            continue
        add(f"title-{lab}", "alpha", prm, where="title", menu=menu, fr=title_frames, covers=["alpha.mode"])

    # Far into the timeline: frame 107980 of a 1 h clip, every preset and the stages.
    for c in [c for c in cases if "hour" in c["tags"] and c["where"] == "clip"]:
        h = copy.deepcopy(c)
        h.update(id="hour-" + c["id"], where="hour", frames=[HOUR_FRAME], tags=[])
        cases.append(h)

    # The Edit page (scripts can't set OpenFX params there: the dev copy's test preset file sets them).
    loopf = lambda n: json.loads((ROOT / "presets" / f"{n}.json").read_text(encoding="utf-8"))  # noqa: E731
    minidv = path_overrides(sch, fmts["Digital video/MiniDV"]["applies"])
    edit = [("default", None, {}), ("mild-vhs", "Mild VHS", {}), ("glitch-pop", "Glitch pop", {}),
            ("clean-crt", "Clean CRT", {}), ("ntsc-off", None, {"ntsc_enabled": 0}),
            ("bottom-to-top", None, loop_overrides(sch, loopf("Screen Loop/Bottom to top"))),
            ("minidv", None, minidv), ("cd-rom", "CD-ROM (low quality)", {})]
    for lab, look, prm in edit:
        if quick and lab not in ("default", "glitch-pop", "minidv"):
            continue
        add(f"edit-{lab}", "edit", prm, where="edit", look=look, animated=is_animated(look) if look else False,
            tags=("noise",) if lab == "default" else ())
        if lab in ("default", "mild-vhs", "glitch-pop", "bottom-to-top", "minidv"):
            add(f"edit1h-{lab}", "edit", prm, where="edit1h", look=look, fr=[HOUR_FRAME],
                animated=is_animated(look) if look else False)
    for lab, prm in (("default", {}), ("spill", {"alpha_mode": 1}), ("bottom-to-top",
                     loop_overrides(sch, loopf("Screen Loop/Bottom to top")))):
        if quick and lab != "default":
            continue
        add(f"edit-xform-{lab}", "edit", prm, where="edit", xform=True)

    # 1080p and UHD projects: the Edit page and a Fusion comp on the clip.
    for proj in ("1080p", "uhd"):
        if quick and proj == "uhd":
            continue
        for lab, look, prm in edit:
            if lab not in ("default", "mild-vhs", "clean-crt", "bottom-to-top", "minidv"):
                continue
            if lab == "bottom-to-top":
                # Screen Loop on an Edit-page clip at 1080p / UHD: getFramesNeeded asks Resolve for the loop's history
                # (frames 1440..1850 for frame 1850), float frames of the timeline's size; Resolve grew to 20-30 GB
                # and the render didn't finish in 10 minutes (2026-11-12, CPU path), blocking the session. Covered at
                # 640x360 (edit-bottom-to-top) and on the Fusion page of these projects.
                continue
            if quick and lab != "default":
                continue
            add(f"{proj}-edit-{lab}", "size", prm, where=f"{proj}-edit", look=look, fr=[0, 1850])
        add(f"{proj}-edit-xform-default", "size", {}, where=f"{proj}-edit", xform=True, fr=[0, 1850])
        add(f"{proj}-fusion-default", "size", {}, where=f"{proj}-clip", fr=[0, 1850])
        if not quick:
            add(f"{proj}-fusion-mild-vhs", "size", {}, menu=["host_preset", looks.index("Mild VHS") + 1, "Mild VHS"],
                where=f"{proj}-clip", fr=[0, 1850])
            add(f"{proj}-fusion-bottom-to-top", "size", {}, where=f"{proj}-clip", fr=[0, 1850],
                menu=["host_screenloop_preset", loops.index("Screen Loop/Bottom to top") + 1, "Screen Loop/Bottom to top"])
    ids = [c["id"] for c in cases]
    dup = {i for i in ids if ids.count(i) > 1}
    if dup:
        raise RuntimeError(f"duplicate case ids: {sorted(dup)}")
    return cases, expected


def coverage(cases):
    """Schema params set on their own by some case, and the rest with the reason."""
    sch = schema()
    covered = {}
    for c in cases:
        for pid in c["covers"]:
            covered.setdefault(pid, []).append(c["id"])
    rows = []
    for p in sch["params"]:
        pid = p["id"]
        if pid in covered:
            how = "individually" if not pid.startswith("crt.") or pid == "crt.preset" or pid == "crt.enabled" \
                else "all of its preset's params at once (crt-<preset>-all-params-strong)"
            rows.append({"id": pid, "exercised": True, "how": how, "cases": len(covered[pid])})
        else:
            lo, hi = p.get("min"), p.get("max")
            why = "label row in the shader's parameter list (min = max = 0)" if p["type"] == "float" and lo == hi \
                else "a stage's on/off switch (stage-*-alone)" if pid.endswith(".enabled") \
                else "not generated"
            rows.append({"id": pid, "exercised": pid.endswith(".enabled"), "how": why, "cases": 0})
    return rows


# --------------------------------------------------------------- sources ---
def make_sources(d):
    d = Path(d)
    d.mkdir(parents=True, exist_ok=True)
    ff = shutil.which("ffmpeg")
    clip, hour = d / "parity-130s.mov", d / "parity-1h.mp4"
    if not clip.exists():
        if not ff:
            raise RuntimeError("--parity needs ffmpeg on PATH to make its source clips")
        subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30",
                        "-t", "130", "-c:v", "prores_ks", "-profile:v", "0", "-pix_fmt", "yuv422p10le", str(clip)], check=True)
    if not hour.exists():
        if not ff:
            raise RuntimeError("--parity needs ffmpeg on PATH to make its source clips")
        # 59:50 of a flat colour, then 30 s of the moving test pattern (a small file)
        subprocess.run([ff, "-hide_banner", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "color=c=0x406080:s=640x360:r=30:d=3590",
                        "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=30:d=30", "-filter_complex", "[0:v][1:v]concat=n=2:v=1[v]",
                        "-map", "[v]", "-c:v", "libx264", "-preset", "veryfast", "-crf", "18", "-g", "30", "-pix_fmt", "yuv420p",
                        str(hour)], check=True)
    still = d / "roundtrip-16bit.png"
    if not still.exists():
        write_png16(still, 640, 360)
    return {"clip": str(clip), "hour": str(hour), "still": str(still)}


def write_png16(path, w, h):
    """A 16-bit RGB PNG with every kind of value: gradients plus seeded noise in the low bits."""
    rnd = random.Random(1234)
    rows = []
    for y in range(h):
        line = bytearray(b"\x00")
        for x in range(w):
            for c in range(3):
                v = (x * 65535 // (w - 1) if c == 0 else y * 65535 // (h - 1) if c == 1 else rnd.randrange(65536))
                line += struct.pack(">H", v)
        rows.append(bytes(line))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)
    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 16, 2, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(b"".join(rows), 6)) + chunk(b"IEND", b""))


# ------------------------------------------------------------- PNG / diff ---
def read_rows(path):
    """(w, h, channels, bits, [row bytes]) of a PNG, unfiltered (Resolve writes filter 0; others are decoded)."""
    data = Path(path).read_bytes()
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", data[pos + 8:pos + 21])
        elif typ == b"IDAT":
            idat.append(data[pos + 8:pos + 8 + ln])
        elif typ == b"IEND":
            break
        pos += 12 + ln
    w, h, bd, ct, _, _, il = ihdr
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    stride = w * ch * bd // 8
    raw = zlib.decompress(b"".join(idat))
    if il == 0 and all(raw[y * (stride + 1)] == 0 for y in range(h)):
        return w, h, ch, bd, [raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)] for y in range(h)]
    # filtered rows: the runner's decoders (slow, only for files Resolve didn't write)
    if bd == 16:
        chans = rrt.read_png16(path, tuple(range(min(ch, 3))))
        rows = []
        for y in range(h):
            a = array.array("H", [0] * (w * len(chans)))
            for c, vals in enumerate(chans):
                a[c::len(chans)] = array.array("H", vals[y * w:(y + 1) * w])
            if sys.byteorder == "little":
                a.byteswap()
            rows.append(a.tobytes())
        return w, h, len(chans), 16, rows
    img = rrt.read_png(path)
    return w, h, 4, 8, [bytes(img.px[y * w * 4:(y + 1) * w * 4]) for y in range(h)]


def values(row, bd):
    if bd == 16:
        a = array.array("H")
        a.frombytes(row)
        if sys.byteorder == "little":
            a.byteswap()
        return a
    return array.array("H", (v * 257 for v in row))


def compare(pa, pb, diff_png=None):
    """Differences between two renders, in 16-bit units, RGB only: max, values differing, sum."""
    wa, ha, ca, ba, ra = read_rows(pa)
    wb, hb, cb, bb, rb = read_rows(pb)
    if (wa, ha) != (wb, hb):
        return {"error": f"size {wa}x{ha} vs {wb}x{hb}"}
    n = wa * ha * 3
    mx = cnt = tot = 0
    bad_rows = []
    for y, (x, z) in enumerate(zip(ra, rb)):
        if x == z and ca == cb and ba == bb:
            continue
        va, vb = values(x, ba), values(z, bb)
        if ca == 4 or cb == 4:
            va = array.array("H", [v for i, v in enumerate(va) if i % ca < 3])
            vb = array.array("H", [v for i, v in enumerate(vb) if i % cb < 3])
        row_bad = False
        for p, q in zip(va, vb):
            if p != q:
                d = p - q if p > q else q - p
                cnt += 1
                tot += d
                if d > mx:
                    mx = d
                row_bad = True
        if row_bad:
            bad_rows.append(y)
    out = {"max16": mx, "max_of_255": round(mx / 257, 4), "differing": cnt, "values": n,
           "pct_differing": round(100.0 * cnt / n, 5), "mean_of_255": round(tot / n / 257, 6),
           "rows_differing": len(bad_rows)}
    if cnt and diff_png and wa * ha <= 1920 * 1080:
        px = bytearray(wa * ha * 4)
        px[3::4] = b"\xff" * (wa * ha)
        for y in bad_rows:
            va, vb = values(ra[y], ba), values(rb[y], bb)
            for x in range(wa):
                for c in range(3):
                    d = abs(va[x * ca + c] - vb[x * cb + c]) / 257
                    px[(y * wa + x) * 4 + c] = min(255, int(d * 32 + (64 if d else 0)))
        rrt.write_png(diff_png, rrt.Image(wa, ha, px))
    return out


_DEPTH = {}


def order_params(params):
    """Params in an order the host accepts: Resolve ignores SetInput on a control its switch has greyed out, so
    switches turned on come first (a stage's switch, then the group switches inside it, shallowest first), then
    the values, then switches turned off (deepest first)."""
    if not _DEPTH:
        sch = schema()
        parents = {g["id"]: g.get("parent") for g in sch["groups"]}
        for g in parents:
            d, x = 0, g
            while x:
                d, x = d + 1, parents.get(x)
            _DEPTH[host(g)] = d
    def key(kv):
        k, v = kv
        if not (k in _DEPTH or k.endswith("_enabled")):
            return (1, 0)
        d = _DEPTH.get(k, 0)
        return (0, d) if v else (2, -d)
    return sorted(params.items(), key=key)


# ------------------------------------------------------- inside a session ---
def log_routes(text):
    """Route summary of the plug-in's render lines in `text`."""
    paths, ntsc, reasons, ts, inst, loops, errs = {}, {}, set(), set(), set(), {"gpu": 0, "cpu": 0}, []
    digital, digital_reasons = {}, set()
    fill, fill_reasons = {}, set()  # Clip mode's hole filling (host GPU frames with transparent pixels)
    sizes, depths = set(), set()
    identity = 0
    for line in text.splitlines():
        if "isIdentity" in line and "passes the source through" in line:
            identity += 1
        m = re.search(r"instance #(\d+)(?: debug)? render #\d+ t=(\S+) .*?dst=\S+ \((\d+x\d+)\).*? depth=(\S+) .*?"
                      r"status=(\d+)(.*?) ms=\S+ (.*)$", line)
        if not m:
            if "GPU path unavailable" in line or "cpu-fallback" in line:
                errs.append(line[line.find("instance"):][:300])
            continue
        inst.add(int(m.group(1)))
        ts.add(m.group(2))
        sizes.add(m.group(3))
        depths.add(m.group(4))
        if m.group(5) != "0":
            errs.append(f"status={m.group(5)}{m.group(6)[:200]}")
        rest = m.group(7)
        pm = re.match(r"(\S+->cpu-fallback )?(path=\S+)", rest)
        pth = (pm.group(1) or "") + pm.group(2) if pm else "path=?"
        pth = pth.replace("->cpu-fallback ", "->cpu-fallback:")
        paths[pth] = paths.get(pth, 0) + 1
        r = re.search(r" ntsc_route=(gpu|cpu)(?: \(([^)]*)\))?", rest)
        if r:
            ntsc[r.group(1)] = ntsc.get(r.group(1), 0) + 1
            if r.group(2):
                reasons.add(f"{r.group(1)} ({r.group(2)})")
        r = re.search(r" digital_route=(gpu|cpu)(?: \(([^)]*)\))?", rest)
        if r:
            digital[r.group(1)] = digital.get(r.group(1), 0) + 1
            if r.group(2):
                digital_reasons.add(f"{r.group(1)} ({r.group(2)})")
        r = re.search(r" fill_route=(gpu|cpu)(?: \(([^)]*)\))?", rest)
        if r:
            fill[r.group(1)] = fill.get(r.group(1), 0) + 1
            if r.group(2):
                fill_reasons.add(f"{r.group(1)} ({r.group(2)})")
        lp = re.search(r" ntsc_loop=gpu:(\d+),cpu:(\d+)", rest)
        if lp:
            loops["gpu"] += int(lp.group(1))
            loops["cpu"] += int(lp.group(2))
    return {"paths": paths, "ntsc": ntsc, "ntsc_reasons": sorted(reasons), "digital": digital,
            "digital_reasons": sorted(digital_reasons), "fill": fill, "fill_reasons": sorted(fill_reasons), "t": sorted(ts, key=lambda v: float(v)),
            "instances": sorted(inst), "ntsc_loop": loops, "errors": errs[:5], "sizes": sorted(sizes), "identity": identity,
            "depths": sorted(depths)}


class Session:
    """The parity cases inside one Resolve session (Suite.t_parity)."""

    def __init__(self, suite):
        self.s, self.a = suite, suite.a
        doc = json.loads(Path(self.a.parity_cases).read_text(encoding="utf-8"))
        self.sources, self.cases = doc["sources"], doc["cases"]
        if self.a.parity_subset:
            self.cases = [c for c in self.cases if self.a.parity_subset in c["tags"]]
        self.log = suite.plugin_log()
        self.out = suite.out
        self.results = {}
        self.media = {}
        self.preset_file = os.environ.get("NTSCRT_OFX_TEST_PRESET_FILE")
        self.fx_id = None
        self.codec = "RGB16"

    # -- helpers --------------------------------------------------------
    def import_media(self, key):
        k = (id(self.s.p), key)
        if k not in self.media:
            items = rrt.retry(lambda: self.s.mp.ImportMedia([self.sources[key]]), f"import {self.sources[key]}")
            self.media[k] = items[0]
        return self.media[k]

    def clip_timeline(self, name, key):
        tl = self.s.new_timeline(name)
        mpi = self.import_media(key)
        item = rrt.retry(lambda: self.s.mp.AppendToTimeline([{"mediaPoolItem": mpi}]), "append the clip")[0]
        # the playhead back at the start: Resolve renders the frame under it too (for the viewer), and at the clip's
        # end that is a far frame with a long history for Screen Loop / Digital video
        try:
            tl.SetCurrentTimecode(tl.GetStartTimecode())
        except Exception:  # noqa: BLE001
            pass
        return tl, item

    def render(self, tl, frames, tag):
        return self.s.render_timeline(tl, frames, tag, codec=self.codec)

    def mark(self):
        return self.s.log_size(self.log)

    def since(self, mark):
        return self.s.log_since_rotated(self.log, mark)

    def note(self, cid, **kw):
        self.results.setdefault(cid, {}).update(kw)

    # -- sections --------------------------------------------------------
    def run(self):
        t_all = time.time()
        self.s.p.SetCurrentRenderFormatAndCodec("png", self.codec)
        fx_ids = [e["id"] for e in self.s.r.GetEffectsList() if "ntscrt" in (e.get("name", "") + e.get("id", "")).lower()]
        self.fx_id = fx_ids[0] if fx_ids else None
        info = {"config": self.a.parity_config, "env": {k: os.environ.get(k) for k in ENV_KEYS},
                "resolve": self.s.r.GetVersionString(), "cases": len(self.cases)}
        info["roundtrip"] = self.roundtrip()
        by_where = {}
        for c in self.cases:
            by_where.setdefault(c["where"], []).append(c)
        order = ["clip", "hour", "title", "edit", "edit1h", "1080p-edit", "1080p-clip", "uhd-edit", "uhd-clip"]
        for where in order:
            cs = by_where.get(where)
            if not cs:
                continue
            t0 = time.time()
            try:
                if where.startswith(("1080p", "uhd")):
                    self.size_project(where.split("-")[0])
                if where in ("clip", "hour") or where.endswith("-clip"):
                    self.fusion_on_clip(where, cs)
                elif where == "title":
                    self.title(cs)
                else:
                    self.edit(where, cs)
            except Exception as e:  # noqa: BLE001  (a section's setup failing: its cases are errors)
                for c in cs:
                    if c["id"] not in self.results or "images" not in self.results[c["id"]]:
                        self.note(c["id"], error=f"section {where}: {e}")
                print(f"parity section {where} failed: {e}", flush=True)
            print(f"parity section {where}: {len(cs)} cases in {time.time() - t0:.0f}s", flush=True)
        info["seconds"] = round(time.time() - t_all)
        rrt.W, rrt.H = 640, 360
        doc = {"info": info, "results": self.results}
        (self.out / "session.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")
        errors = [cid for cid, r in self.results.items() if r.get("error")]
        return info, errors

    def roundtrip(self):
        """A 16-bit PNG still through the timeline and the render queue, compared with itself."""
        try:
            tl = self.s.new_timeline("par-roundtrip")
            mpi = rrt.retry(lambda: self.s.mp.ImportMedia([self.sources["still"]]), "import the still")[0]
            rrt.retry(lambda: self.s.mp.AppendToTimeline([{"mediaPoolItem": mpi}]), "append the still")
            got = self.render(tl, [0], "roundtrip")[0]
            return compare(self.sources["still"], got)
        except Exception as e:  # noqa: BLE001
            return {"error": str(e)}

    def size_project(self, which):
        w, h = {"1080p": (1920, 1080), "uhd": (3840, 2160)}[which]
        if rrt.W == w:
            return
        pm = self.s.r.GetProjectManager()
        pm.SaveProject()
        name = f"NTSCRT Resolve Parity {which}"
        if name in (pm.GetProjectListInCurrentFolder() or []):
            pm.DeleteProject(name)
        p = rrt.retry(lambda: pm.CreateProject(name), f"create project {name}")
        # A project's size is set before it has timelines (timelines' resolutions are never changed).
        for k, v in (("timelineResolutionWidth", str(w)), ("timelineResolutionHeight", str(h)),
                     ("timelineFrameRate", "30"), ("timelinePlaybackFrameRate", "30")):
            p.SetSetting(k, v)
        self.s.p, self.s.mp = p, p.GetMediaPool()
        rrt.W, rrt.H = w, h
        p.SetCurrentRenderFormatAndCodec("png", self.codec)

    def apply_fusion(self, fx, c):
        bad = []
        if c.get("menu"):
            inp, idx, name = c["menu"]
            fx.SetInput(inp, idx)
            if fx.GetInput(inp) != idx:
                bad.append(f"{inp}={idx} didn't stick ({fx.GetInput(inp)})")
            if inp == "host_screenloop_preset" and fx.GetInput("screenloop_enabled") != 1:
                bad.append("the Screen Loop preset didn't turn Screen Loop on")
        for k, v in order_params(c["params"]):
            if fx.GetInput(k) is None:
                bad.append(f"no input {k}")
                continue
            fx.SetInput(k, v)
            got = fx.GetInput(k)
            if isinstance(v, (int, float)) and got is not None and abs(float(got) - float(v)) > 1e-4 * max(1, abs(v)):
                bad.append(f"{k}={v} reads back {got}")
        return bad

    def run_case(self, c, render_fn):
        """Warm-up for animated looks (their keys reach Resolve in pieces during the first render), then the
        frames with the log marked, then (animated) the same frames again within the session."""
        t0 = time.time()
        tag = f"c-{c['id']}"
        if c.get("animated"):
            render_fn(c["frames"], tag + "-warm")
        m = self.mark()
        imgs = render_fn(c["frames"], tag)
        routes = log_routes(self.since(m))
        rec = {"images": {str(f): Path(p).name for f, p in imgs.items()}, "routes": routes}
        if c.get("animated"):
            rep = render_fn(c["frames"], tag + "-rep")
            rec["repeat"] = {str(f): compare(imgs[f], rep[f]) for f in c["frames"]}
        for f in list(self.out.glob(f"{tag}-warm-*.png")):
            f.unlink()
        rec["seconds"] = round(time.time() - t0, 1)
        self.note(c["id"], **rec)

    def fusion_on_clip(self, where, cases):
        key = "hour" if where == "hour" else "clip"
        tl, item = self.clip_timeline(f"par-{where}", key)
        comp = item.AddFusionComp() or item.GetFusionCompByIndex(1)
        fu = rrt.Fu(self.s, comp, tl)
        mi = next(t for t in comp.GetToolList(False).values() if t.GetAttrs("TOOLS_RegID") == "MediaIn")
        srcs = {}
        for name, depth in (("f32", 4), ("i8", 1), ("f16", 3)):
            if any(c["source"] == name for c in cases):
                cd = fu.add("ChangeDepth", Depth=depth)
                cd.Input = mi.Output
                srcs[name] = cd
        if any(c["source"] == "small" for c in cases):
            rs = fu.add("BetterResize", Width=400, Height=226)
            rs.Input = srcs["f32"].Output
            srcs["small"] = rs
        frames = sorted({f for c in cases for f in c["frames"]})
        for name, tool in srcs.items():
            imgs = fu.capture(tool, frames, f"src-{where}-{name}")
            self.note(f"src:{where}:{name}", images={str(f): Path(p).name for f, p in imgs.items()}, routes={})
        for i, c in enumerate(cases):
            fx = None
            try:
                fx = self.s.ntscrt(fu, srcs[c["source"]])
                bad = self.apply_fusion(fx, c)
                if bad:
                    self.note(c["id"], setup=bad)
                self.run_case(c, lambda fr, tag: fu.capture(fx, fr, tag))
            except Exception as e:  # noqa: BLE001
                self.note(c["id"], error=str(e))
                print(f"  {c['id']}: {e}", flush=True)
            finally:
                if fx:
                    try:
                        fx.Delete()
                    except Exception:  # noqa: BLE001
                        pass
            if i % 20 == 0:
                print(f"  {where} {i + 1}/{len(cases)} {c['id']}", flush=True)

    def title(self, cases):
        fu, _ = self.s.new_comp("par-title")
        bg = fu.add("Background", TopLeftRed=0.2, TopLeftGreen=0.2, TopLeftBlue=0.25, TopLeftAlpha=1, Depth=4)
        title = self.s.title(fu, 0.2)
        title.SetInput("Depth", 4)
        src = fu.add("Merge", Background=bg, Foreground=title, Depth=4)
        frames = sorted({f for c in cases for f in c["frames"]})
        imgs = fu.capture(src, frames, "src-title")
        self.note("src:title:f32", images={str(f): Path(p).name for f, p in imgs.items()}, routes={})
        for c in cases:
            fx = mg = None
            try:
                fx = self.s.ntscrt(fu, title)
                bad = self.apply_fusion(fx, c)
                if bad:
                    self.note(c["id"], setup=bad)
                mg = fu.add("Merge", Background=bg, Foreground=fx, Depth=4)
                self.run_case(c, lambda fr, tag: fu.capture(mg, fr, tag))
            except Exception as e:  # noqa: BLE001
                self.note(c["id"], error=str(e))
            finally:
                for t in (mg, fx):
                    if t:
                        try:
                            t.Delete()
                        except Exception:  # noqa: BLE001
                            pass

    XFORM = {"ZoomX": 0.6, "ZoomY": 0.6, "Pan": 40.0, "Tilt": -20.0, "CropLeft": 30.0, "CropBottom": 20.0}

    def edit(self, where, cases):
        if not self.fx_id:
            raise RuntimeError("NTSCRT isn't in Resolve's effect list")
        if not self.preset_file:
            raise RuntimeError("the Edit-page cases need --headless (the dev copy's test preset file)")
        key = "hour" if where == "edit1h" else "clip"
        frames = sorted({f for c in cases for f in c["frames"]})
        for xform in sorted({bool(c.get("xform")) for c in cases}):
            tl, item = self.clip_timeline(f"par-{where}-src{'-x' if xform else ''}", key)
            if xform:
                item.SetProperty(dict(self.XFORM))
            imgs = self.render(tl, frames, f"src-{where}{'-x' if xform else ''}")
            self.note(f"src:{where}:{'xform' if xform else 'plain'}", images={str(f): Path(p).name for f, p in imgs.items()},
                      routes={})
        for c in cases:
            try:
                tl, item = self.clip_timeline(f"par-{c['id']}", key)
                if c.get("xform"):
                    item.SetProperty(dict(self.XFORM))
                opts = " ".join(f"{k}={v}" for k, v in order_params(c["params"]))
                Path(self.preset_file).write_text(f"{c['look'] or '(defaults)'}\n0\n{opts}\n", encoding="utf-8")
                m = self.mark()
                try:
                    eff = item.AddEffect("ofx", self.fx_id)
                finally:
                    try:
                        os.remove(self.preset_file)
                    except OSError:
                        pass
                if not eff:
                    raise RuntimeError("AddEffect refused")
                applied = re.findall(r"test preset file: ([^\r\n]*)", self.since(m))
                self.note(c["id"], applied=applied[:1])
                if not applied:
                    self.note(c["id"], setup=["the test preset file wasn't applied (no log line)"])
                self.run_case(c, lambda fr, tag: self.render(tl, fr, tag))
            except Exception as e:  # noqa: BLE001
                self.note(c["id"], error=str(e))
                print(f"  {c['id']}: {e}", flush=True)


# ------------------------------------------------------------ orchestrator ---
def expected_paths(cfg):
    return "cpu" if CONFIGS[cfg]["frames"] == "cpu" else "gpu"


def route_problems(cfg, r):
    """What's wrong with a case's routes in session `cfg` (empty: as expected)."""
    if not r:
        return ["no render lines in the plug-in's log"]
    out = []
    want = CONFIGS[cfg]
    paths = r.get("paths", {})
    if not paths:
        # every stage off: Resolve passes the source through without rendering (isIdentity)
        return [] if r.get("identity") else ["no render lines in the plug-in's log"]
    for p in paths:
        if "fallback" in p:
            out.append(f"{p} ({paths[p]})")
        elif want["frames"] == "cpu" and p != "path=cpu":
            out.append(f"{p} in a CPU-frames session")
        elif want["frames"] == "gpu" and not p.startswith("path=gpu-"):
            out.append(f"{p} in a GPU-frames session")
    other = "cpu" if want["ntsc"] == "gpu" else "gpu"
    if r.get("ntsc", {}).get(other):
        out.append(f"ntsc_route={other} x{r['ntsc'][other]} ({'; '.join(r.get('ntsc_reasons', []))[:160]})")
    lp = r.get("ntsc_loop", {})
    if lp.get(other):
        out.append(f"Screen Loop passes on {other} NTSC: {lp[other]}")
    dwant = want["digital"]
    if dwant == "cpu-if-ntsc":
        dwant = "cpu" if r.get("ntsc") else "gpu"
    dother = "cpu" if dwant == "gpu" else "gpu"
    if r.get("digital", {}).get(dother):
        out.append(f"digital_route={dother} x{r['digital'][dother]} ({'; '.join(r.get('digital_reasons', []))[:160]})")
    # Host GPU frames in Clip mode with transparent pixels (titles, transformed / smaller clips): the holes are
    # filled on the GPU (HOST-GPU.md), so no frame comes down for the CPU fill (gpu-cpuconvert).
    if want["frames"] == "gpu":
        if "path=gpu-cpuconvert" in paths:
            out.append(f"path=gpu-cpuconvert x{paths['path=gpu-cpuconvert']} (Clip mode's holes filled on the CPU)")
        if r.get("fill", {}).get("cpu"):
            out.append(f"fill_route=cpu x{r['fill']['cpu']} ({'; '.join(r.get('fill_reasons', []))[:160]})")
    if r.get("errors"):
        out.append("log: " + " | ".join(r["errors"])[:300])
    return out


def wait_resolve_gone(grace=180, total=600):
    """The previous session's headless Resolve can take a while to exit after Quit (tens of GB to free). Waits for
    it; after `grace` seconds kills only headless (-nogui) Resolve processes, which are the suite's own."""
    t0 = time.time()
    while rrt.resolve_running():
        if time.time() - t0 > grace and sys.platform == "win32":
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Get-CimInstance Win32_Process -Filter \"Name='Resolve.exe'\" | Where-Object { $_.CommandLine "
                            "-like '*-nogui*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"],
                           capture_output=True)
        if time.time() - t0 > total:
            return False
        time.sleep(5)
    return True


def run_config(a, cfg, base, cases_path, out_name=None):
    sub = copy.copy(a)
    sub.parity = sub.parity_quick = False
    sub.only = {"parity"}
    sub.out = str(base / (out_name or cfg))
    sub.parity_cases = str(cases_path)
    sub.parity_config = cfg
    sub.parity_subset = CONFIGS[cfg].get("subset")
    keep = {k: os.environ.get(k) for k in ENV_KEYS + ("NTSCRT_OFX_LOG",)}
    try:
        for k in ENV_KEYS:
            os.environ.pop(k, None)
        os.environ.update(CONFIGS[cfg]["env"])
        os.environ.pop("NTSCRT_OFX_LOG", None)
        Path(sub.out).mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        print(f"=== parity session {cfg}: {CONFIGS[cfg]['what']} env={CONFIGS[cfg]['env']}", flush=True)
        if not wait_resolve_gone():
            print(f"=== parity session {cfg}: a Resolve is still running, skipped", flush=True)
            return 2, 0
        rc = rrt.run_session(sub)
        print(f"=== parity session {cfg} done rc={rc} in {time.time() - t0:.0f}s", flush=True)
        return rc, round(time.time() - t0)
    finally:
        for k, v in keep.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v


def retry_config(a, cfg, base, cases, timings):
    """--parity-retry: render again, in a session of their own, the cases of `cfg` that errored or are missing (Resolve
    stopped answering for a while, say) and merge them into its session.json. None when there was nothing to do."""
    doc = load_session(base, cfg)
    if not doc:
        print(f"--parity-retry: no session {cfg} in {base}", flush=True)
        return None
    sub = CONFIGS[cfg].get("subset")
    todo = [c for c in cases if (not sub or sub in c["tags"]) and
            (c["id"] not in doc["results"] or doc["results"][c["id"]].get("error") or "images" not in doc["results"][c["id"]])]
    if not todo:
        return None
    full = json.loads((base / "cases.json").read_text(encoding="utf-8"))
    path = base / f"cases-retry-{cfg}.json"
    path.write_text(json.dumps({"sources": full["sources"], "cases": todo, "expected": full["expected"]}, indent=1),
                    encoding="utf-8")
    print(f"--parity-retry {cfg}: {len(todo)} cases", flush=True)
    rc, secs = run_config(a, cfg, base, path, out_name=f"{cfg}-retry")
    timings[cfg + "-retry"] = secs
    redo = load_session(base, f"{cfg}-retry")
    if not redo:
        return rc or 1
    merged = []
    for c in todo:
        r = redo["results"].get(c["id"])
        if not r or r.get("error") or "images" not in r:
            continue
        for name in r["images"].values():
            shutil.copyfile(base / f"{cfg}-retry" / name, base / cfg / name)
        doc["results"][c["id"]] = dict(r, retried=True)
        merged.append(c["id"])
    # The retry session's sources against the first session's (Resolve should have rendered the same pictures).
    src_check = {}
    for k, r in redo["results"].items():
        if k.startswith("src:") and k in doc["results"]:
            for f, name in r.get("images", {}).items():
                old = doc["results"][k].get("images", {}).get(f)
                if old:
                    src_check[f"{k}:{f}"] = compare(base / cfg / old, base / f"{cfg}-retry" / name).get("max16")
    doc["info"].setdefault("retries", []).append({"cases": len(todo), "merged": len(merged), "seconds": secs,
                                                  "sources_max16_vs_first_session": src_check})
    (base / cfg / "session.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")
    print(f"--parity-retry {cfg}: merged {len(merged)} of {len(todo)}; sources vs the first session {src_check}", flush=True)
    return rc


def load_session(base, cfg):
    p = base / cfg / "session.json"
    if not p.exists():
        return None
    return json.loads(p.read_text(encoding="utf-8"))


def analyse(base, cases, configs, expected, timings):
    sess = {cfg: load_session(base, cfg) for cfg in configs}
    case_by_id = {c["id"]: c for c in cases}
    rows, pair_summary, route_rows = [], {}, []
    # routes per session
    for cfg, doc in sess.items():
        if not doc:
            continue
        for c in cases:
            if CONFIGS[cfg].get("subset") and CONFIGS[cfg]["subset"] not in c["tags"]:
                continue
            r = doc["results"].get(c["id"], {})
            probs = [] if r.get("error") else route_problems(cfg, r.get("routes"))
            route_rows.append({"session": cfg, "case": c["id"], "paths": r.get("routes", {}).get("paths"),
                               "ntsc": r.get("routes", {}).get("ntsc"), "digital": r.get("routes", {}).get("digital"),
                               "problems": probs, "error": r.get("error"),
                               "setup": r.get("setup")})
    route_bad = {(x["session"], x["case"]) for x in route_rows if x["problems"]}
    for sa, sb, rule in PAIRS:
        A, B = sess.get(sa), sess.get(sb)
        if not A or not B:
            continue
        summ = {"rule": rule, "cases": 0, "images": 0, "pass": 0, "fail": 0, "info": 0, "missing": 0, "nonzero": 0,
                "route_fail": 0}
        ids = [c["id"] for c in cases if not CONFIGS[sa].get("subset") or CONFIGS[sa]["subset"] in c["tags"]]
        src_ids = sorted(k for k in A["results"] if k.startswith("src:"))
        for cid in src_ids + ids:
            ra, rb = A["results"].get(cid, {}), B["results"].get(cid, {})
            c = case_by_id.get(cid) or {"id": cid, "group": "source", "where": cid.split(":")[1], "source": "f32",
                                        "tags": []}
            summ["cases"] += 1
            if ra.get("error") or rb.get("error") or "images" not in ra or "images" not in rb:
                summ["missing"] += 1
                rows.append({"pair": f"{sa}-vs-{sb}", "rule": rule, "case": cid, "group": c["group"], "where": c["where"],
                             "frame": "", "status": "missing",
                             "note": (ra.get("error") or rb.get("error") or "not rendered")[:200]})
                continue
            depth_info = rule == "hostgpu" and c.get("source") in ("i8", "f16")
            routes_ok = (sa, cid) not in route_bad and (sb, cid) not in route_bad
            # A pass must not come from both sessions taking the same NTSC (or Digital video) route.
            same_route = ""
            if not cid.startswith("src:") and CONFIGS[sa]["ntsc"] != CONFIGS[sb]["ntsc"]:
                na, nb = ra["routes"].get("ntsc", {}), rb["routes"].get("ntsc", {})
                if bool(na) != bool(nb):
                    same_route = f"NTSC lines in one session only ({na} vs {nb})"
            if not cid.startswith("src:") and CONFIGS[sa]["digital"] != CONFIGS[sb]["digital"]:
                da, db = ra["routes"].get("digital", {}), rb["routes"].get("digital", {})
                if bool(da) != bool(db):
                    same_route = f"Digital video lines in one session only ({da} vs {db})"
            for f, ia in ra["images"].items():
                ib = rb["images"].get(f)
                if not ib:
                    summ["missing"] += 1
                    continue
                d = compare(base / sa / ia, base / sb / ib, diff_png=None)
                summ["images"] += 1
                if "error" in d:
                    status, note = "fail", d["error"]
                elif rule == "exact" or (rule == "hostgpu" and cid.startswith("src:")):
                    status = "pass" if d["max16"] == 0 else "fail"
                    note = ""
                elif rule == "hostgpu":
                    ok = d["max16"] <= TOL_MAX16 and d["differing"] <= TOL_FRACTION * d["values"]
                    status = "pass" if ok else ("info" if depth_info else "fail")
                    note = "Fusion hands GPU-render plug-ins float images whatever the tool depth (HOST-GPU.md)" \
                        if depth_info else ""
                else:  # noise
                    status = "pass" if d["max16"] == 0 else "info"
                    note = "session-to-session noise" if d["max16"] else ""
                if status in ("pass", "info") and (not routes_ok or same_route):
                    status = "route-fail"
                    note = (same_route or "route not as expected in a session") + (f"; {note}" if note else "")
                if status == "fail" and d.get("differing"):
                    dp = base / f"diff-{sa}-vs-{sb}-{cid.replace(':', '_')}-{f}.png"
                    compare(base / sa / ia, base / sb / ib, diff_png=dp)
                    note = (note + "; " if note else "") + f"diff image {dp.name}"
                if d.get("differing"):
                    summ["nonzero"] += 1
                key = "route_fail" if status == "route-fail" else status
                summ[key] = summ.get(key, 0) + 1
                rows.append({"pair": f"{sa}-vs-{sb}", "rule": rule, "case": cid, "group": c["group"], "where": c["where"],
                             "frame": f, "status": status, "max_of_255": d.get("max_of_255"),
                             "pct_differing": d.get("pct_differing"), "mean_of_255": d.get("mean_of_255"),
                             "differing": d.get("differing"), "values": d.get("values"),
                             "paths_a": ra["routes"].get("paths"), "ntsc_a": ra["routes"].get("ntsc"),
                             "paths_b": rb["routes"].get("paths"), "ntsc_b": rb["routes"].get("ntsc"),
                             "digital_a": ra["routes"].get("digital"), "digital_b": rb["routes"].get("digital"),
                             "note": note})
        pair_summary[f"{sa}-vs-{sb}"] = summ
    # Within a session: animated looks rendered twice; the effect switched off against the source.
    repeats, identity = [], []
    for cfg, doc in sess.items():
        if not doc:
            continue
        for cid, r in doc["results"].items():
            for f, d in (r.get("repeat") or {}).items():
                repeats.append({"session": cfg, "case": cid, "frame": f, **d})
        for cid in ("all-off", "hour-all-off"):
            r = doc["results"].get(cid)
            where = case_by_id.get(cid, {}).get("where")
            src = doc["results"].get(f"src:{where}:f32")
            if r and src and "images" in r and "images" in src:
                for f, im in r["images"].items():
                    if f in src["images"]:
                        identity.append({"session": cfg, "case": cid, "frame": f,
                                         **compare(base / cfg / im, base / cfg / src["images"][f])})
    doc = {"when": datetime.datetime.now().isoformat(timespec="seconds"), "configs": {k: CONFIGS[k] for k in configs},
           "pairs": pair_summary, "expected_presets": expected, "cases": len(cases),
           "sessions": {cfg: (d or {}).get("info") for cfg, d in sess.items()}, "timings_s": timings,
           "route_problems": [x for x in route_rows if x["problems"] or x["error"] or x["setup"]],
           "routes": route_rows, "animated_repeat": repeats, "effect_off_vs_source": identity,
           "coverage": coverage(cases), "rows": rows}
    return doc


def write_reports(base, doc):
    (base / "parity.json").write_text(json.dumps(doc, indent=1), encoding="utf-8")
    cols = ["pair", "rule", "case", "group", "where", "frame", "status", "max_of_255", "pct_differing", "mean_of_255",
            "differing", "values", "paths_a", "ntsc_a", "paths_b", "ntsc_b", "note"]
    with open(base / "parity.csv", "w", newline="", encoding="utf-8") as f:
        w = csv.DictWriter(f, fieldnames=cols, extrasaction="ignore")
        w.writeheader()
        for r in doc["rows"]:
            w.writerow({k: (json.dumps(v) if isinstance(v, dict) else v) for k, v in r.items()})
    L = [f"# NTSCRT GPU/CPU parity in Resolve — {doc['when']}", "",
         f"{doc['cases']} cases; presets on disk: {doc['expected_presets']}", "",
         "| pair | rule | cases | images | pass | fail | info | route-fail | missing | non-zero |", "|---|---|---|---|---|---|---|---|---|---|"]
    for k, s in doc["pairs"].items():
        L.append(f"| {k} | {s['rule']} | {s['cases']} | {s['images']} | {s['pass']} | {s['fail']} | {s['info']} | "
                 f"{s['route_fail']} | {s['missing']} | {s['nonzero']} |")
    L += ["", "## Every image that isn't an exact match, or didn't pass", "",
          "| pair | case | frame | status | max /255 | % differing | mean /255 | routes A | routes B | note |",
          "|---|---|---|---|---|---|---|---|---|---|"]
    for r in doc["rows"]:
        if r["status"] != "pass" or r.get("differing"):
            L.append(f"| {r['pair']} | {r['case']} | {r['frame']} | {r['status']} | {r.get('max_of_255')} | "
                     f"{r.get('pct_differing')} | {r.get('mean_of_255')} | {r.get('paths_a')} {r.get('ntsc_a')} | "
                     f"{r.get('paths_b')} {r.get('ntsc_b')} | {r.get('note', '')} |")
    L += ["", "## Route problems (per session)", ""]
    for x in doc["route_problems"]:
        L.append(f"- {x['session']} {x['case']}: {x['problems']} {x.get('error') or ''} {x.get('setup') or ''}")
    L += ["", "## Animated looks rendered twice in a session (non-zero only)", ""]
    for x in doc["animated_repeat"]:
        if x.get("differing"):
            L.append(f"- {x['session']} {x['case']} frame {x['frame']}: max {x['max_of_255']}/255, {x['pct_differing']}%")
    L += ["", "## Effect off (all stages off) vs the source, per session", ""]
    for x in doc["effect_off_vs_source"]:
        L.append(f"- {x['session']} {x['case']} frame {x['frame']}: max {x.get('max_of_255')}/255, {x.get('pct_differing')}%")
    L += ["", "## Sessions", ""]
    for cfg, i in doc["sessions"].items():
        L.append(f"- {cfg}: {json.dumps(i)}")
    L += ["", "## Schema params not exercised on their own", ""]
    for r in doc["coverage"]:
        if not r["exercised"] or r["how"] != "individually":
            L.append(f"- {r['id']}: {r['how']}")
    (base / "parity.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def digital_case(c):
    """The case runs Digital video (--parity-digital): it switches the stage on, or picks a look / format with it."""
    if (c.get("params") or {}).get("digital_enabled") == 1:
        return True
    names = ([c["menu"][2]] if c.get("menu") else []) + ([c["look"]] if c.get("look") else [])
    for n in names:
        if n.startswith("Digital video/"):
            return True
        f = ROOT / "presets" / f"{n}.json"
        if f.exists() and (json.loads(f.read_text(encoding="utf-8")).get("digital") or {}).get("enabled"):
            return True
    return False


def main(a):
    import faulthandler
    faulthandler.enable()  # a crash inside fusionscript prints where it happened
    if not a.headless or not a.bundle:
        print("--parity needs --headless --bundle <NTSCRT.ofx.bundle> (one Resolve session per configuration, the "
              "dev copy's test preset file for the Edit page)", file=sys.stderr)
        return 2
    import tempfile
    base = Path(a.out or os.path.join(tempfile.gettempdir(), "ntscrt-resolve-tests",
                                      datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-parity"))
    base.mkdir(parents=True, exist_ok=True)
    quick = a.parity_quick
    configs = [c.strip() for c in a.parity_configs.split(",") if c.strip()]
    unknown = [c for c in configs if c not in CONFIGS]
    if unknown:
        print(f"--parity-configs: unknown {unknown} (known: {', '.join(CONFIGS)})", file=sys.stderr)
        return 2
    cases_path = base / "cases.json"
    if (a.parity_compare_only or a.parity_retry) and cases_path.exists():
        doc = json.loads(cases_path.read_text(encoding="utf-8"))
        cases, expected = doc["cases"], doc["expected"]
    else:
        cases, expected = build_cases(quick)
        if getattr(a, "parity_digital", False):
            cases = [c for c in cases if digital_case(c)]
        groups = {g.strip() for g in (getattr(a, "parity_groups", "") or "").split(",") if g.strip()}
        if groups:
            cases = [c for c in cases if c["group"] in groups]
        sources = make_sources(Path(a.parity_sources) if a.parity_sources else base / "_src")
        cases_path.write_text(json.dumps({"sources": sources, "cases": cases, "expected": expected, "quick": quick},
                                         indent=1), encoding="utf-8")
    print(f"parity: {len(cases)} cases, presets {expected}, sessions {configs} -> {base}", flush=True)
    timings = {}
    timings_path = base / "timings.json"
    if timings_path.exists():
        timings = json.loads(timings_path.read_text(encoding="utf-8"))
    rcs = {}
    if a.parity_retry:
        for cfg in configs:
            rc = retry_config(a, cfg, base, cases, timings)
            if rc is not None:
                rcs[cfg + "-retry"] = rc
            timings_path.write_text(json.dumps(timings), encoding="utf-8")
    elif not a.parity_compare_only:
        for cfg in configs:
            rc, secs = run_config(a, cfg, base, cases_path)
            rcs[cfg] = rc
            timings[cfg] = secs
            timings_path.write_text(json.dumps(timings), encoding="utf-8")
    t0 = time.time()
    doc = analyse(base, cases, [c for c in CONFIGS if (base / c / "session.json").exists()], expected, timings)
    doc["compare_s"] = round(time.time() - t0)
    doc["session_rc"] = rcs
    write_reports(base, doc)
    for k, s in doc["pairs"].items():
        print(f"PARITY {k} ({s['rule']}): {s['images']} images, {s['pass']} pass, {s['fail']} fail, {s['info']} info, "
              f"{s['route_fail']} route-fail, {s['missing']} missing, {s['nonzero']} non-zero", flush=True)
    bad = sum(s["fail"] + s["route_fail"] + s["missing"] for s in doc["pairs"].values())
    absent = [f"{x}-vs-{y}" for x, y, _ in PAIRS if x in configs and y in configs and f"{x}-vs-{y}" not in doc["pairs"]]
    if absent or any(rc for rc in rcs.values()):
        print(f"sessions failed {rcs}; pairs not compared: {absent}", flush=True)
        bad += 1 + len(absent)
    print(f"{'FAIL' if bad else 'PASS'} parity -> {base / 'parity.md'}", flush=True)
    return 1 if bad else 0
