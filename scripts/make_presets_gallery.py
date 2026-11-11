#!/usr/bin/env python3
"""Regenerates the presets gallery: docs/PRESETS-GALLERY.md and one animated GIF per preset in
docs/images/presets/<slug>.gif.

  python scripts/make_presets_gallery.py --build <cmake build dir> [--renderer resolve|cli]
                                         [--bundle <NTSCRT.ofx.bundle>] [--work DIR] [--only slug,slug] [--page-only]

--build defaults to the NTSCRT_BUILD_DIR environment variable; --bundle to
<build>/ofx/Release/NTSCRT.ofx.bundle.

The preset list comes from the code, so new presets show up by themselves:
  - Looks and Digital video formats: the plug-in's Preset menu (bundled presets/*.json in the order of
    released_preset_order() in ofx/ofx_params.cpp, then newer ones alphabetically, then the schema's
    digital_formats as "Digital video/<name>"). A look with timeline keys is animated: choosing it
    writes keyframes, one timeline pass from the effect's first frame, repeated.
  - Screen Loop presets (presets/Screen Loop/*.json, kind ntscrt-video-feedback): the Screen Loop preset menu.
  - CRT shader presets: the options of crt.preset in core/params/schema.json.

1. scripts/make_previews.ps1 -SceneOnly writes the synthetic looping scene (960x540, loops every 2 s at 30 fps).
2. --renderer resolve (default): DaVinci Resolve Studio is started headless (-nogui) with a copy of --bundle
   (tests/resolve/run_resolve_tests.py start_headless). One Fusion comp: Loader (the scene) -> a fresh NTSCRT
   tool per preset with the preset chosen from its menu input, exactly as a user would (Preset,
   Screen Loop preset, or CRT shader), frames rendered through the render queue. Anything Resolve fails on
   is rendered with ntscrt-cli instead (--look / --feedback / --timeline). Every preset (or --compare)
   is checked against ntscrt-cli on its first, middle and last frame (fails above --max-diff). --renderer cli renders everything with ntscrt-cli.
   Resolve must not be running; the script waits (up to --wait minutes) for it to quit.
3. ffmpeg (on PATH) encodes each GIF (palettegen/paletteuse) and the page is written.

--page-only rewrites the Markdown from the last run's work/gallery.json without rendering.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "resolve"))
sys.path.insert(0, str(ROOT / "scripts"))
import run_resolve_tests as rrt  # noqa: E402  (PNG I/O, Resolve start-up, comps, render queue)
import make_previews_resolve as mpr  # noqa: E402  (start_resolve, wait_for_resolve_to_quit, ofx_name)

W, H, FPS = 960, 540, 30
SCENE_LOOP = 60                 # make_previews.ps1's scene repeats every 60 frames (2 s)
GIF_W, GIF_H = 336, 189         # 960x540 x 0.35
GIF_STEP = 3                    # every 3rd frame of the 30 fps renders: a 10 fps GIF
GIF_STEP_LONG = 5               # passes longer than the 2 s scene loop: 6 fps
PAGE = ROOT / "docs" / "PRESETS-GALLERY.md"
IMAGES = ROOT / "docs" / "images" / "presets"
PROJECT = "NTSCRT Presets Gallery"
GROUPS = [
    ("look", "Looks", "Bundled looks with fixed settings (the noise, waves and glitches still move every frame). "
                      "Choose them in **Presets → Preset**."),
    ("animated", "Animated looks", "These looks carry timeline keys: choosing one in **Presets → Preset** writes "
                                   "keyframes, one pass of the timeline from the effect's first frame, repeated until "
                                   "the effect ends. Each GIF shows one pass."),
    ("format", "Digital video formats", "Entries named `Digital video/<format>` in **Presets → Preset**: they turn "
                                        "on the Digital video stage (a digital camera and codec instead of tape) and "
                                        "set its values and the width; the rest of the look stays as it was "
                                        "(here: the default look). The descriptions are the formats' own."),
    ("loop", "Screen Loop", "**Presets → Screen Loop preset**: video feedback, a camcorder filming the TV that shows "
                            "its own picture. These set only the Screen Loop controls (here over the default look). "
                            "Each GIF shows the preset's Length: one move."),
    ("crt", "CRT shaders", "**CRT → Shader**: the libretro slang CRT shaders over the default look "
                           "(which uses CRT Glow (Gaussian)). Best seen at full size: the GIFs are scaled down, "
                           "so the masks and scanlines blur."),
]


def log(msg):
    print(msg, flush=True)


def slugify(name):
    return re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()


# --------------------------------------------------------------- presets ---
def load_schema():
    return json.loads((ROOT / "core/params/schema.json").read_text(encoding="utf-8"))


def menu_orders(schema):
    """(looks menu, Screen Loop menu) as the plug-in builds them: ofx/ofx_params.cpp parse_presets."""
    src = (ROOT / "ofx/ofx_params.cpp").read_text(encoding="utf-8")
    block = src[src.index("released_preset_order() {"):]
    released = re.findall(r'"([^"]+)"', block[:block.index("};")])
    looks, loops = [], []
    for f in sorted((ROOT / "presets").rglob("*.json")):
        rel = f.relative_to(ROOT / "presets").with_suffix("").as_posix()
        kind = json.loads(f.read_text(encoding="utf-8")).get("kind")
        (loops if kind == "ntscrt-video-feedback" else looks).append(rel)
    looks += ["Digital video/" + d["name"] for d in schema.get("digital_formats", [])]

    def key(n):
        return (released.index(n) if n in released else len(released), n.encode())
    return sorted(looks, key=key), sorted(loops, key=key)


def nest(pointers):
    """{"/a/b": v} -> {"a": {"b": v}} (a Digital video format's "applies" as a look)."""
    out = {}
    for ptr, v in pointers.items():
        parts = ptr.strip("/").split("/")
        d = out
        for p in parts[:-1]:
            d = d.setdefault(p, {})
        d[parts[-1]] = v
    return out


def timeline_keys(look):
    # Keys play whatever the timeline's "enabled" says, as in the core (core/params/timeline.h).
    return (look.get("timeline") or {}).get("keys") or []


def entries(schema):
    """Every preset in the gallery, in menu order within its group."""
    looks, loops = menu_orders(schema)
    fmts = {"Digital video/" + d["name"]: d for d in schema.get("digital_formats", [])}
    out = []
    for i, name in enumerate(looks):
        e = {"name": name, "slug": slugify(name), "menu": "host_preset", "index": i + 1}
        if name in fmts:
            e.update(group="format", look=nest(fmts[name]["applies"]), hint=fmts[name].get("hint", ""))
        else:
            look = json.loads((ROOT / "presets" / f"{name}.json").read_text(encoding="utf-8"))
            keys = timeline_keys(look)
            e.update(look=look, group="animated" if len(keys) >= 2 else "look")
            if e["group"] == "animated":
                e["pass_frames"] = max(1, round(look["timeline"]["duration"] * FPS))
        out.append(e)
    for i, name in enumerate(loops):
        fb = json.loads((ROOT / "presets" / f"{name}.json").read_text(encoding="utf-8"))
        out.append({"name": name, "slug": slugify(name), "menu": "host_screenloop_preset", "index": i + 1,
                    "group": "loop", "look": {}, "feedback": fb})
    crt = next(p for p in schema["params"] if p["id"] == "crt.preset")
    for i, o in enumerate(crt["options"]):
        out.append({"name": o["label"], "value": o["value"], "slangp": o.get("slangp", ""), "slug": "crt-" + slugify(o["value"]),
                    "menu": "crt_preset", "index": i, "group": "crt",
                    "look": {"shader": {"enabled": True, "preset": o["value"]}}})
    for e in out:
        e["frames"], e["step"], e["note"] = frame_plan(e)
    slugs = [e["slug"] for e in out]
    dup = {s for s in slugs if slugs.count(s) > 1}
    if dup:
        raise RuntimeError(f"two presets share a slug: {dup}")
    return out


def frame_plan(e):
    """(frames rendered from frame 0, GIF step, note). The scene loops every 60 frames."""
    if e["group"] == "animated":
        n = e["pass_frames"]
    elif e["group"] == "loop":
        fb = e["feedback"]
        n = round(min(60.0, max(0.5, fb.get("seconds", 5))) * FPS)   # one move (the Length)
    else:
        n = SCENE_LOOP
    note = ""
    if n % SCENE_LOOP:
        note = f"{n / FPS:g} s pass; the scene's own 2 s loop jumps where the GIF restarts."
    if e["group"] == "loop" and not e["feedback"]["values"].get("loop"):
        note = (note + " " if note else "") + "One-way move: it starts over where the GIF restarts."
    return n, GIF_STEP if n <= SCENE_LOOP else GIF_STEP_LONG, note


# ------------------------------------------------------------ descriptions ---
def schema_index(schema):
    for g in schema.get("groups", []):
        GROUP_LABELS[g["id"]] = (g.get("label", ""), g.get("parent"))   # parent set = a sub-group
    return {p["path"]: p for p in schema["params"]}


GROUP_LABELS = {}


def label(P, path, fallback):
    """The control's label, qualified by its sub-group ("Tracking noise height") when the label alone is vague."""
    p = P.get(path)
    if not p:
        return fallback.replace("_", " ")
    g = GROUP_LABELS.get(p.get("group"))
    if g and g[1] and g[0].lower() not in p["label"].lower():
        return f"{g[0]} {p['label'][:1].lower()}{p['label'][1:]}"
    return p["label"]


def option_label(P, path, value):
    p = P.get(path) or {}
    for o in p.get("options", []):
        if o["value"] == value:
            return o.get("label", str(value))
    return str(value)


def changed(p, v):
    ref = p.get("neutral", p.get("default"))
    if isinstance(v, bool) or isinstance(ref, bool):
        return bool(v) != bool(ref)
    try:
        return abs(float(v) - float(ref)) > 1e-6
    except (TypeError, ValueError):
        return v != ref


def keyed_labels(P, look):
    """Labels of the values that differ between timeline keys."""
    keys = timeline_keys(look)
    seen, out = set(), []
    base = {"ntsc": "/ntsc/settings/", "glitch": "/glitch/values/", "digital": "/digital/values/"}
    for sect in ("ntsc", "glitch", "shader", "digital"):
        names = sorted({n for k in keys for n in (k.get(sect) or {})})
        for n in names:
            vals = {json.dumps(k.get(sect, {}).get(n)) for k in keys if n in (k.get(sect) or {})}
            if len(vals) < 2:
                continue
            if sect == "shader":
                pre = (look.get("shader") or {}).get("preset", "glow_gauss")
                path = next((pp for pp, p in P.items() if pp == f"/shader/params/{pre}/{n}" or
                             (pp.endswith("/" + n) and pp.startswith("/shader"))), "")
            else:
                path = base[sect] + n
            lab = label(P, path, n)
            if lab not in seen:
                seen.add(lab)
                out.append(lab)
    return out


def describe(e, P):
    g = e["group"]
    if g == "format":
        return e["hint"]
    if g == "crt":
        return f"`{e['value']}`: libretro `{e['slangp']}` over the default look." + \
            (" This is the default look's shader." if e["value"] == "glow_gauss" else "")
    if g == "loop":
        fb = e["feedback"]
        parts = []
        # The camera and keying controls, in this order (centre, exposure and softness left out).
        for n in ("delay", "zoom", "turn", "tilt", "roll", "spin", "push", "drift_x", "drift_y", "shake", "key",
                  "key_invert", "hue_drift", "color_drift", "loop", "counter"):
            p, v = P.get(f"fb:/values/{n}"), fb["values"].get(n)
            if not p or v is None or not changed(p, v):
                continue
            if p["type"] == "bool":
                parts.append(p["label"].lower())
            else:
                parts.append(f"{p['label'][:1].lower()}{p['label'][1:]} {v:.3g}")
        length = fb.get("seconds", 5)
        return f"Length {length:g} s; " + ", ".join(parts) + "."
    look = e["look"]
    parts = []
    ds = look.get("downscale") or {}
    if ds.get("enabled"):
        parts.append(f"downscaled to {ds.get('width')} px ({option_label(P, '/downscale/method', ds.get('method'))})")
    nt = look.get("ntsc") or {}
    s = nt.get("settings") or {}
    if nt.get("enabled") is False:
        parts.append("NTSC signal off")
    elif s:
        if s.get("vhs_settings"):
            sp = option_label(P, "/ntsc/settings/vhs_tape_speed", s.get("vhs_tape_speed")).split(" ")[0]
            parts.append(f"VHS {sp}" if sp != "None" else "VHS")
        on = [lab for n, lab in (("head_switching", "head switching"), ("tracking_noise", "tracking noise"),
                                 ("ringing", "ringing")) if s.get(n)]
        noise = [lab for n, lab in (("composite_noise", "composite"), ("luma_noise", "luma"),
                                    ("chroma_noise", "chroma")) if s.get(n)]
        if noise:
            on.append("/".join(noise) + " noise")
        if s.get("vhs_settings") and s.get("vhs_edge_wave_enabled") and s.get("vhs_edge_wave", 0) > 0:
            on.append("edge wave")
        if s.get("snow_intensity", 0) > 0:
            on.append("snow")
        if on:
            parts.append(", ".join(on))
    gl = look.get("glitch") or {}
    if gl.get("enabled"):
        on = [P[f"/glitch/values/{n}"]["label"].lower() for n, v in (gl.get("values") or {}).items()
              if f"/glitch/values/{n}" in P and changed(P[f"/glitch/values/{n}"], v)]
        if on:
            parts.append("glitch: " + ", ".join(on[:5]) + (f" (+{len(on) - 5})" if len(on) > 5 else ""))
    dg = look.get("digital") or {}
    if dg.get("enabled"):
        v = dg.get("values") or {}
        parts.append(f"Digital video ({option_label(P, '/digital/values/codec', v.get('codec', 0))} codec, "
                     f"{v.get('frame_rate', 30):g} fps)")
    sh = look.get("shader") or {}
    if sh.get("enabled") is False:
        parts.append("no CRT shader")
    elif sh:
        parts.append(option_label(P, "/shader/preset", sh.get("preset", "glow_gauss")))
    text = "; ".join(parts)
    text = text[:1].upper() + text[1:] + "." if text else ""
    if g == "animated":
        kl = keyed_labels(P, look)
        n = len(timeline_keys(look))
        dur = look["timeline"]["duration"]
        moves = ", ".join(kl[:4]) + (f" and {len(kl) - 4} more" if len(kl) > 4 else "")
        text = f"**Animated** ({dur:g} s pass, {n} keys{': ' + moves if moves else ''}). " + text
    return text


# ---------------------------------------------------------------- rendering ---
def find_cli(build):
    b = Path(build)
    for c in (b / "tools/ntscrt-cli/Release/ntscrt-cli.exe", b / "tools/ntscrt-cli/ntscrt-cli"):
        if c.exists():
            return c
    return None


def find_resources(bundle, build):
    for r in (Path(bundle) / "Contents" / "Resources" if bundle else None,
              Path(build) / "ofx/Release/NTSCRT.ofx.bundle/Contents/Resources", Path(build) / "resources"):
        if r and (r / "shaders").is_dir():
            return r
    return None


def render_cli(cli, res, seq, e, out_dir, tag=None, frames=None):
    """ntscrt-cli: the preset's look (--timeline for an animated one) or Screen Loop (--feedback)."""
    tag = tag or e["slug"]
    out_dir.mkdir(parents=True, exist_ok=True)
    lp = out_dir / f"{tag}.look.json"
    lp.write_text(json.dumps(e["look"]), encoding="utf-8")
    first, count = (0, e["frames"]) if frames is None else frames
    cmd = [str(cli), "--seq", str(seq / "in_%04d.png"), "--out", str(out_dir / f"{tag}-%04d.png"),
           "--frame", str(first), "--frames", str(count), "--fps", str(FPS), "--look", str(lp), "--resources", str(res)]
    if e["group"] == "animated":
        cmd.append("--timeline")
    if e.get("feedback"):
        fp = out_dir / f"{tag}.feedback.json"
        fp.write_text(json.dumps(e["feedback"]), encoding="utf-8")
        cmd += ["--feedback", str(fp)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        raise RuntimeError(f"ntscrt-cli failed for {e['name']}: {p.stderr.strip()}")
    return {f: out_dir / f"{tag}-{f:04d}.png" for f in range(first, first + count)}


def render_resolve(a, seq, todo, renders):
    """Renders `todo` in headless Resolve. Returns ({slug: "resolve"}, {slug: error})."""
    done, failed = {}, {}
    mpr.wait_for_resolve_to_quit(a.wait)
    (Path(a.work) / "ntscrt-ofx.log").unlink(missing_ok=True)   # count this run's lines only
    os.environ["NTSCRT_OFX_LOG"] = str(Path(a.work) / "ntscrt-ofx.log")
    proc, r = mpr.start_resolve(a.bundle, a.work)
    try:
        log(f"Resolve {r.GetVersionString()} headless: {len(todo)} presets")
        rrt.W, rrt.H, rrt.FPS = W, H, FPS      # project size, set before any timeline exists
        rrt.PROJECT = PROJECT
        args = argparse.Namespace(out=str(renders), build=a.build, only=set(), quick=False, keep_project=False)
        s = rrt.Suite(r, args)
        s.open_project()
        try:
            fu, item = s.new_comp("gallery")
            loader = fu.add("Loader")
            loader.Clip = str(seq / "in_0000.png")
            dur = item.GetDuration() if hasattr(item, "GetDuration") else None
            need = max(e["frames"] for e in todo)
            log(f"comp clip: {dur} frames (need {need})")
            chk = fu.capture(loader, [0, 37], "scene-check")
            for f, path in chk.items():
                d = rrt.stats(rrt.read_png(path), rrt.read_png(seq / f"in_{f:04d}.png"))["rgb"]
                log(f"scene frame {f} through Resolve vs source: mean {d['mean']}, max {d['max']}")
                if d["mean"] > 1.0:
                    raise RuntimeError(f"Resolve changed the scene frames (frame {f}: mean diff {d['mean']})")
            for e in todo:
                t0 = time.time()
                fx = None
                try:
                    fx = s.ntscrt(fu, loader)
                    # The user's action: pick the preset in its menu (the plug-in writes the values / keyframes).
                    fx.SetInput(e["menu"], e["index"])
                    if e["group"] == "loop" and fx.GetInput("screenloop_enabled") != 1:
                        raise RuntimeError("the Screen Loop preset didn't turn Screen Loop on")
                    if e["group"] == "animated":
                        e["keyed_check"] = check_keyed(fx, e)
                    fu.capture(fx, (0, e["frames"] - 1), e["slug"])
                    done[e["slug"]] = "resolve"
                    log(f"  {e['name']}: {e['frames']} frames in {time.time() - t0:.1f}s")
                except Exception as ex:  # noqa: BLE001  (fall back to ntscrt-cli for this one)
                    failed[e["slug"]] = str(ex)
                    log(f"  {e['name']}: Resolve failed: {ex}")
                finally:
                    if fx:
                        try:
                            fx.Delete()
                        except Exception:  # noqa: BLE001
                            pass
        finally:
            s.close()
    finally:
        r.Quit()
        try:
            proc.wait(90)
        except subprocess.TimeoutExpired:
            proc.kill()
    plog = Path(a.work) / "ntscrt-ofx.log"
    if plog.exists():
        ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        lines = plog.read_text(encoding="utf-8", errors="replace").splitlines()
        loads = [x for x in lines if f"NTSCRT {ver}" in x]
        log("plug-in log: " + (loads[-1].strip() if loads else "(no load line)"))
        if not any("dev copy" in x for x in loads):
            log("WARNING: the log doesn't say 'dev copy': Resolve may have loaded the installed plug-in")
        anim = [x for x in lines if "animated preset" in x]
        log(f"plug-in log: {len(anim)} 'animated preset' lines (keyframes written)")
    return done, failed


def check_keyed(fx, e):
    """Informational: how many keyed inputs move between frame 0 and mid-pass (Fusion's GetInput(name, t))."""
    names = set()
    for k in timeline_keys(e["look"]):
        names |= {f"ntsc_{n}" for n in (k.get("ntsc") or {})}
    n = e["pass_frames"]
    moving = []
    for nm in sorted(names):
        try:
            vals = {round(float(fx.GetInput(nm, f) or 0), 5) for f in (0, n // 4, n // 2, 3 * n // 4)}
        except Exception:  # noqa: BLE001
            continue
        if len(vals) > 1:
            moving.append(nm)
    return moving


def compare(cli, res, seq, e, renders, work):
    n = e["frames"]
    frames = sorted({0, n // 2, n - 1})
    out = []
    for f in frames:
        # One frame at a time is enough: the core fetches the earlier frames it needs from --seq.
        ref = render_cli(cli, res, seq, e, work / "cmp", tag=f"{e['slug']}-cmp", frames=(f, 1))[f]
        d =rrt.stats(rrt.read_png(renders / f"{e['slug']}-{f:04d}.png"), rrt.read_png(ref))["rgb"]["mean"]
        out.append((f, d))
    return out


# -------------------------------------------------------------------- GIFs ---
def make_gif(ffmpeg, frames, fps, out, work, colors):
    tmp = work / "gif-tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    tmp.mkdir(parents=True)
    for i, p in enumerate(frames):
        shutil.copyfile(p, tmp / f"{i:04d}.png")
    vf = (f"scale={GIF_W}:{GIF_H}:flags=lanczos,split[a][b];[a]palettegen=max_colors={colors}:stats_mode=full[p];"
          f"[b][p]paletteuse=dither=none:diff_mode=rectangle")
    cmd = [ffmpeg, "-hide_banner", "-loglevel", "error", "-y", "-framerate", f"{fps:g}", "-i", str(tmp / "%04d.png"),
           "-vf", vf, "-loop", "0", str(out)]
    subprocess.run(cmd, check=True)


# -------------------------------------------------------------------- page ---
def write_page(all_entries, P, info):
    lines = ["# Presets gallery", "",
             "Every preset that ships with the NTSCRT plug-ins, applied to the same synthetic test scene (a looping "
             "synthwave grid, 2 s at 30 fps) so they can be compared side by side. In DaVinci Resolve they're in the "
             "NTSCRT effect's Inspector, in the **Presets** group at the top: **Preset** (looks and Digital video "
             "formats) and **Screen Loop preset**; the CRT shaders are in **CRT → Shader**. Choosing a preset writes its values into "
             "the controls below it, so you can tweak or keyframe them afterwards.", "",
             "The GIFs loop; the animated looks loop one pass of their keyframes. They're scaled down to "
             f"{GIF_W} px and limited to a GIF palette, so fine detail (scanlines, masks, noise) looks rougher than "
             "in a real render.", ""]
    toc = [f"[{title}](#{slugify(title)})" for g, title, _ in GROUPS if any(e["group"] == g for e in all_entries)]
    lines += ["Sections: " + " · ".join(toc), ""]
    for g, title, intro in GROUPS:
        es = [e for e in all_entries if e["group"] == g]
        if not es:
            continue
        lines += [f"## {title}", "", intro + f" ({len(es)} presets)", "", "<table>"]
        for i in range(0, len(es), 2):
            lines.append("<tr>")
            for e in es[i:i + 2]:
                d = describe(e, P)
                note = f"<br><sub><i>{e['note']}</i></sub>" if e["note"] else ""
                lines.append(f'<td width="350" valign="top"><img src="images/presets/{e["slug"]}.gif" '
                             f'width="{GIF_W}" height="{GIF_H}" alt="{e["name"]}"><br>'
                             f'<b>{e["name"]}</b><br><sub>{md_inline(d)}</sub>{note}</td>')
            if len(es[i:i + 2]) == 1:
                lines.append('<td width="350"></td>')
            lines.append("</tr>")
        lines += ["</table>", ""]
    used = info.get("renderers", {})
    by_group = {}
    for e in all_entries:
        by_group.setdefault(e["group"], set()).add(used.get(e["slug"], "?"))
    how = {"resolve": "the OpenFX plug-in in DaVinci Resolve Studio (headless), the preset chosen from its menu",
           "cli": "`ntscrt-cli` (the same core, no host)", "?": "unknown"}
    lines += ["---", "", "### How these images were made", "",
              "The scene is drawn by `scripts/make_previews.ps1 -SceneOnly` (no third-party footage). Each preset was "
              f"rendered at 960×540 at 30 fps from frame 0, then every {ordinal(GIF_STEP)} frame was encoded as a "
              f"{FPS / GIF_STEP:g} fps GIF (every {ordinal(GIF_STEP_LONG)} frame, {FPS / GIF_STEP_LONG:g} fps, for passes "
              f"longer than 2 s; {GIF_W}×{GIF_H}, a {info.get('colors', 32)}-colour palette per GIF, ffmpeg "
              "palettegen/paletteuse). Rendering path per group:", ""]
    paths = {x for v in by_group.values() for x in v}
    if len(paths) == 1:
        lines[-2] = lines[-2].replace(" Rendering path per group:", f" Every group was rendered with {how[paths.pop()]}.")
        lines.pop()
    else:
        for g, title, _ in GROUPS:
            if g in by_group:
                lines.append(f"- {title}: " + "; ".join(how[x] for x in sorted(by_group[g])) + ".")
    fell_back = [e["name"] for e in all_entries if used.get(e["slug"]) == "cli"]
    if fell_back and "resolve" in used.values():
        lines.append("- Rendered with `ntscrt-cli` because Resolve failed on them: " + ", ".join(fell_back) + ".")
    if info.get("compare"):
        worst = max(d for v in info["compare"].values() for _, d in v)
        which = "Every preset" if len(info["compare"]) == len(all_entries) else f"A sample ({', '.join(info['compare'])})"
        lines += ["", f"{which} was checked against `ntscrt-cli` given the preset file (`--timeline` for the animated "
                      f"ones, `--feedback` for Screen Loop) on the first, middle and last frame: largest mean "
                      f"difference {worst:.2f} of 255."]
    lines += ["", "Regenerate the GIFs and this page (Resolve Studio closed, ffmpeg on PATH; `--renderer cli` "
                  "renders without Resolve):", "", "```",
              "python scripts/make_presets_gallery.py --build <your CMake build dir>",
              "```", "", "New presets in `presets/`, new Digital video formats and new CRT shaders in "
              "`core/params/schema.json` are picked up automatically.", ""]
    PAGE.write_text("\n".join(lines), encoding="utf-8", newline="\n")


def ordinal(n):
    return {2: "2nd", 3: "3rd"}.get(n, f"{n}th")


def md_inline(s):
    s = s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    return re.sub(r"`([^`]+)`", r"<code>\1</code>", s)


# -------------------------------------------------------------------- main ---
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--renderer", choices=("resolve", "cli"), default="resolve")
    ap.add_argument("--build", default=os.environ.get("NTSCRT_BUILD_DIR"),
                    help="CMake build dir with ntscrt-cli (default: $NTSCRT_BUILD_DIR)")
    ap.add_argument("--bundle", help="the NTSCRT.ofx.bundle Resolve loads (default: <build>/ofx/Release/NTSCRT.ofx.bundle)")
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "ntscrt-presets-gallery"))
    ap.add_argument("--only", default="", help="comma-separated slugs to (re)render; the page still lists all")
    ap.add_argument("--compare", default="all", help="slugs checked against ntscrt-cli ('all', or comma-separated)")
    ap.add_argument("--max-diff", type=float, default=2.0, help="fail if a compared preset differs by more (of 255)")
    ap.add_argument("--colors", type=int, default=32, help="GIF palette size")
    ap.add_argument("--wait", type=float, default=40, help="minutes to wait for a running Resolve to quit")
    ap.add_argument("--page-only", action="store_true")
    ap.add_argument("--gif-only", action="store_true", help="re-encode the GIFs from the last run's frames")
    a = ap.parse_args()
    if not a.build and not a.page_only:
        ap.error("--build (or the NTSCRT_BUILD_DIR environment variable) is required")
    if a.build and not a.bundle:
        a.bundle = str(Path(a.build) / "ofx" / "Release" / "NTSCRT.ofx.bundle")
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    schema = load_schema()
    P = schema_index(schema)
    es = entries(schema)
    info_path = work / "gallery.json"
    info = json.loads(info_path.read_text(encoding="utf-8")) if info_path.exists() else {}
    info.setdefault("renderers", {})
    if a.page_only:
        write_page(es, P, info)
        log(f"wrote {PAGE}")
        return 0

    only = {s for s in a.only.split(",") if s}
    unknown = only - {e["slug"] for e in es}
    if unknown:
        raise SystemExit(f"--only: unknown slugs {sorted(unknown)}")
    todo = [e for e in es if not only or e["slug"] in only]
    counts = {g: sum(e["group"] == g for e in es) for g, _, _ in GROUPS}
    log(f"{len(es)} presets: {counts}; rendering {len(todo)} with {a.renderer}")
    cli = find_cli(a.build)
    res = find_resources(a.bundle, a.build)
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise SystemExit("ffmpeg not found on PATH")

    t_all = time.time()
    renders = work / "frames"
    cmp = {}
    if not a.gif_only:
        need = max(e["frames"] for e in todo)
        ps = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ROOT / "scripts" / "make_previews.ps1")]
        log("scene frames ...")
        subprocess.run(ps + ["-SceneOnly", "-WorkDir", str(work), "-SceneFrames", str(need)], check=True,
                       stdout=subprocess.DEVNULL)
        seq = work / "seq"
        renders = work / "frames"
        renders.mkdir(parents=True, exist_ok=True)
        for e in todo:
            for p in renders.glob(f"{e['slug']}-*.png"):
                p.unlink()

        fallback = []
        if a.renderer == "resolve":
            done, failed = render_resolve(a, seq, todo, renders)
            info["renderers"].update(done)
            fallback = [e for e in todo if e["slug"] not in done]
            info["resolve_errors"] = failed
        else:
            fallback = todo
        if fallback:
            if not cli or not res:
                raise SystemExit("ntscrt-cli or the bundle's shaders not found (--build / --bundle)")
            for e in fallback:
                t0 = time.time()
                render_cli(cli, res, seq, e, renders)
                info["renderers"][e["slug"]] = "cli"
                log(f"  {e['name']}: ntscrt-cli, {e['frames']} frames in {time.time() - t0:.1f}s")

        if a.renderer == "resolve" and cli and res:
            log("comparing with ntscrt-cli ...")
            wanted = [e["slug"] for e in todo] if a.compare == "all" else [s for s in a.compare.split(",") if s]
            for slug in wanted:
                e = next((x for x in todo if x["slug"] == slug), None)
                if not e or info["renderers"].get(slug) != "resolve":
                    continue
                cmp[e["name"]] = compare(cli, res, seq, e, renders, work)
                log(f"  {e['name']}: mean diff per frame {cmp[e['name']]}")
            info["compare"] = cmp

    log("GIFs ...")
    IMAGES.mkdir(parents=True, exist_ok=True)
    for e in todo:
        frames = [renders / f"{e['slug']}-{f:04d}.png" for f in range(0, e["frames"], e["step"])]
        make_gif(ffmpeg, frames, FPS / e["step"], IMAGES / f"{e['slug']}.gif", work, a.colors)
    keep = {f"{e['slug']}.gif" for e in es}
    for p in IMAGES.glob("*.gif"):
        if p.name not in keep:
            log(f"removing stale {p.name}")
            p.unlink()
    total = sum(p.stat().st_size for p in IMAGES.glob("*.gif"))
    log(f"{len(list(IMAGES.glob('*.gif')))} GIFs, {total / 1e6:.2f} MB")
    info["colors"] = a.colors
    if not a.gif_only:
        info["keyed_check"] = {e["name"]: e.get("keyed_check") for e in todo if "keyed_check" in e}
    info_path.write_text(json.dumps(info, indent=2), encoding="utf-8")
    write_page(es, P, info)
    log(f"wrote {PAGE} in {time.time() - t_all:.0f}s")
    bad = {n: v for n, v in cmp.items() if max(d for _, d in v) > a.max_diff}
    if bad:
        log(f"FAIL: Resolve output differs from ntscrt-cli: {bad}")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
