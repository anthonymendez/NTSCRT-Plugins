#!/usr/bin/env python3
"""Renders the README preview animations with the NTSCRT OpenFX plug-in inside DaVinci Resolve (headless).

  python scripts/make_previews_resolve.py --build <cmake build dir> [--bundle <NTSCRT.ofx.bundle>]
                                          [--work DIR] [--out docs/images]

--build defaults to the NTSCRT_BUILD_DIR environment variable; --bundle to
<build>/ofx/Release/NTSCRT.ofx.bundle.

1. scripts/make_previews.ps1 -SceneOnly writes the looping test scene (PNG frames).
2. Resolve Studio is started with -nogui and loads a copy of --bundle (OFX_PLUGIN_PATH, the same
   start-up as tests/resolve/run_resolve_tests.py --headless). Each look is a Fusion comp:
   Loader (the scene frames) -> NTSCRT tool with the look's parameters, rendered to PNG through the
   render queue (no dialogs). Resolve quits afterwards.
3. The Resolve frames are checked against ntscrt-cli (mean difference per look; when --build has it).
4. scripts/make_previews.ps1 -Renders composites them and writes hero-before-after.webp and
   looks-grid.webp (animated WebP) with ffmpeg.

Needs Resolve Studio (external scripting), Windows PowerShell and ffmpeg on PATH. Without Resolve,
scripts/make_previews.ps1 alone renders the same previews through ntscrt-cli.
Resolve must not be running: the script waits (up to --wait minutes) for it to quit.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests" / "resolve"))
import run_resolve_tests as rrt  # noqa: E402  (PNG I/O, Resolve start-up, comps, render queue)

W, H, FPS = 960, 540, 30
GIF_FRAMES = 60
# Must match the looks in make_previews.ps1 (name, look, first frame of the loop). A file name is a
# bundled preset: it is chosen from the effect's Preset menu, as a user would (so presets saved in an
# older format are converted, and an animated one gets its keyframes); a dict is set control by control.
LOOKS = [
    ("default", {}, 0),
    ("vhold", "V Hold glitch.json", 30),
    ("obliterated", "Obliterated.json", 0),
    ("webcam", "Webcam (low quality).json", 0),
    ("royale", {"shader": {"enabled": True, "preset": "royale"}}, 0),
]


def log(msg):
    print(msg, flush=True)


def ofx_name(pid):
    return "".join(c if c.isalnum() or c == "_" else "_" for c in pid)


def lookup(look, path):
    v = look
    for part in path.strip("/").split("/"):
        if not isinstance(v, dict) or part not in v:
            return None
        v = v[part]
    return v


def apply_look(fx, schema, look):
    """Set every look parameter of the NTSCRT tool: the look's value, else the default (what choosing
    a preset in the Inspector does). Returns how many inputs Fusion refused."""
    refused = []
    for p in schema["params"]:
        if p.get("preset") is False or p.get("kind") == "feedback" or p["path"].startswith("/feedback"):
            continue
        v = lookup(look, p["path"])
        if v is None:
            v = p.get("default")
        if p["type"] == "choice":
            vals = [o["value"] for o in p["options"]]
            v = vals.index(v) if v in vals else vals.index(p.get("default")) if p.get("default") in vals else 0
        elif p["type"] == "bool":
            v = 1 if v else 0
        elif v is None:
            continue
        if fx.SetInput(ofx_name(p["id"]), v) is False:
            refused.append(p["id"])
    return refused


def wait_for_resolve_to_quit(minutes):
    end = time.time() + minutes * 60
    while rrt.resolve_running():
        if time.time() > end:
            raise RuntimeError("DaVinci Resolve is still running: quit it first")
        log("Resolve is running (another run?): waiting 30 s")
        time.sleep(30)


def start_resolve(bundle, log_dir):
    for attempt in (1, 2):  # Resolve's script server occasionally fails to start: retry once
        proc = rrt.start_headless(bundle, log_dir)
        end, r = time.time() + 180, None
        while not r and time.time() < end and proc.poll() is None:
            time.sleep(3)
            try:
                r = rrt.connect()
            except Exception:
                r = None
        if r:
            return proc, r
        log(f"headless Resolve didn't answer (attempt {attempt}); restarting it")
        proc.kill()
        proc.wait(60)
        time.sleep(5)
    raise RuntimeError("headless Resolve didn't answer twice")


def render_in_resolve(r, a, seq, renders, looks):
    rrt.W, rrt.H, rrt.FPS = W, H, FPS          # the project / render size (set before any timeline exists)
    rrt.PROJECT = "NTSCRT README Previews"
    args = argparse.Namespace(out=str(renders), build=a.build, only=set(), quick=False, keep_project=False)
    s = rrt.Suite(r, args)
    s.open_project()
    try:
        fu, _ = s.new_comp("previews")
        loader = fu.add("Loader")
        loader.Clip = str(seq / "in_0000.png")
        # Scene check: the Loader passes the frames through unchanged.
        chk = fu.capture(loader, [0, 37], "scene-check")
        for f, path in chk.items():
            d = rrt.stats(rrt.read_png(path), rrt.read_png(seq / f"in_{f:04d}.png"))["rgb"]
            log(f"scene frame {f} through Resolve vs source: mean {d['mean']}, max {d['max']}")
            if d["mean"] > 1.0:
                raise RuntimeError(f"Resolve changed the scene frames (frame {f}: mean diff {d['mean']}): "
                                   "colour management or a frame offset?")
        for name, look, start, menu_index in looks:
            fx = s.ntscrt(fu, loader)
            if menu_index is not None:
                # The user's action: the plug-in writes the values (and an animated preset's keyframes).
                fx.SetInput("host_preset", menu_index)
            else:
                refused = apply_look(fx, s.schema, look)
                if refused:
                    log(f"  {name}: Fusion refused {len(refused)} inputs: {', '.join(refused[:8])}...")
            t0 = time.time()
            if menu_index is not None and animated(look):
                warm_up_keyframes(fu, fx, (start, start + GIF_FRAMES - 1), renders, name)
            fu.capture(fx, (start, start + GIF_FRAMES - 1), name)
            log(f"  {name}: frames {start}-{start + GIF_FRAMES - 1} rendered in {time.time() - t0:.1f}s")
        return s.r.GetVersionString(), s.version
    finally:
        s.close()


def warm_up_keyframes(fu, fx, frames, renders, name):
    """Render a just-chosen animated preset's frames once and throw them away. Resolve takes the keyframes
    the plug-in writes in pieces while the first render runs (the plug-in logs "keyframes changed without a
    param change notification"), and frames rendered before the last piece keep a half-keyed look: in
    v0.5.0-rc.3 as since, on the CPU and the GPU path alike, V Hold glitch differed from ntscrt-cli in
    about two runs out of three. The second render has all the keys."""
    fu.capture(fx, frames, f"{name}-warmup")
    for p in Path(renders).glob(f"{name}-warmup-*.png"):
        p.unlink()


def animated(look):
    """A look with timeline keys: choosing it from the Preset menu keys its animation."""
    return len((look.get("timeline") or {}).get("keys") or []) >= 2


def preset_menu_index(file_name):
    """Index of a bundled preset in the plug-in's Preset menu (make_presets_gallery.menu_orders: the order
    hostparams/src/params.cpp builds; index 0 is the menu's first, empty entry)."""
    import make_presets_gallery as mpg  # imported here: it imports this module
    looks, _ = mpg.menu_orders(mpg.load_schema())
    return looks.index(Path(file_name).stem) + 1


def compare_with_cli(a, seq, renders, looks, work):
    cli, res = None, None
    b = Path(a.build)
    for c in (b / "tools/ntscrt-cli/Release/ntscrt-cli.exe", b / "tools/ntscrt-cli/ntscrt-cli"):
        if c.exists():
            cli = c
    res = Path(a.bundle) / "Contents" / "Resources"
    if not cli or not (res / "shaders").is_dir():
        log("ntscrt-cli not found: skipping the CLI comparison")
        return {}
    out = {}
    cdir = work / "cli"
    cdir.mkdir(parents=True, exist_ok=True)
    for name, look, start, _ in looks:
        lp = cdir / f"{name}.json"
        lp.write_text(json.dumps(look), encoding="utf-8")
        frames = [start, start + GIF_FRAMES // 2, start + GIF_FRAMES - 1]
        for f in frames:
            o = cdir / f"{name}-{f:04d}.png"
            cmd = [str(cli), "--seq", str(seq / "in_%04d.png"), "--out", str(cdir / f"{name}-%04d.png"), "--frame", str(f),
                   "--frames", "1", "--fps", str(FPS), "--look", str(lp), "--resources", str(res)]
            if animated(look):
                cmd.append("--timeline")  # keyed from the effect's first frame, as choosing it in the menu does
            p = subprocess.run(cmd, capture_output=True, text=True)
            if p.returncode or not o.exists():
                raise RuntimeError(f"ntscrt-cli failed: {p.stderr.strip()}")
        diffs = [rrt.stats(rrt.read_png(renders / f"{name}-{f:04d}.png"), rrt.read_png(cdir / f"{name}-{f:04d}.png"))["rgb"]["mean"]
                 for f in frames]
        out[name] = {"frames": frames, "mean_diff": diffs}
        log(f"  {name}: Resolve vs ntscrt-cli mean diff {diffs}")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", default=os.environ.get("NTSCRT_BUILD_DIR"),
                    help="CMake build dir with ntscrt-cli, for the comparison (default: $NTSCRT_BUILD_DIR)")
    ap.add_argument("--bundle", help="the NTSCRT.ofx.bundle Resolve loads (a copy, via OFX_PLUGIN_PATH; "
                                     "default: <build>/ofx/Release/NTSCRT.ofx.bundle)")
    ap.add_argument("--work", default=os.path.join(tempfile.gettempdir(), "ntscrt-previews-resolve"))
    ap.add_argument("--out", default=str(ROOT / "docs" / "images"))
    ap.add_argument("--wait", type=float, default=40, help="minutes to wait for a running Resolve to quit")
    ap.add_argument("--max-diff", type=float, default=2.0, help="fail if a look differs from ntscrt-cli by more")
    a = ap.parse_args()
    if not a.build:
        ap.error("--build (or the NTSCRT_BUILD_DIR environment variable) is required")
    a.bundle = a.bundle or str(Path(a.build) / "ofx" / "Release" / "NTSCRT.ofx.bundle")
    if not Path(a.bundle).is_dir():
        ap.error(f"no plug-in bundle at {a.bundle}: build first, or pass --bundle")
    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    ps1 = ROOT / "scripts" / "make_previews.ps1"
    ps = ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(ps1)]
    t_all = time.time()

    looks = [(n, json.loads((ROOT / "presets" / l).read_text(encoding="utf-8")), s, preset_menu_index(l))
             if isinstance(l, str) else (n, l, s, None) for n, l, s in LOOKS]
    log("scene frames ...")
    subprocess.run(ps + ["-SceneOnly", "-WorkDir", str(work)], check=True)
    seq = work / "seq"
    renders = work / "resolve"
    renders.mkdir(parents=True, exist_ok=True)
    for p in renders.glob("*.png"):
        p.unlink()

    wait_for_resolve_to_quit(a.wait)
    (work / "ntscrt-ofx.log").unlink(missing_ok=True)
    os.environ["NTSCRT_OFX_LOG"] = str(work / "ntscrt-ofx.log")
    proc, r = start_resolve(a.bundle, work)
    try:
        log(f"Resolve {r.GetVersionString()} headless: rendering {len(looks)} looks x {GIF_FRAMES} frames")
        render_in_resolve(r, a, seq, renders, looks)
    finally:
        r.Quit()
        try:
            proc.wait(90)
        except subprocess.TimeoutExpired:
            proc.kill()
    plog = work / "ntscrt-ofx.log"
    if plog.exists():
        ver = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        loads = [l for l in plog.read_text(encoding="utf-8", errors="replace").splitlines() if f"NTSCRT {ver}" in l]
        log("plug-in log: " + (loads[-1].strip() if loads else "(no load line)"))
        if not any("dev copy" in l for l in loads):
            log("WARNING: the log doesn't say 'dev copy': Resolve may have loaded the installed plug-in")

    log("comparing with ntscrt-cli ...")
    diffs = compare_with_cli(a, seq, renders, looks, work)
    bad = {n: d for n, d in diffs.items() if max(d["mean_diff"]) > a.max_diff}
    if bad:
        log(f"FAIL: Resolve output differs from ntscrt-cli: {bad}")
        return 1

    log("compositing the WebP previews ...")
    subprocess.run(ps + ["-Renders", str(renders), "-WorkDir", str(work), "-OutDir", a.out], check=True)
    (work / "previews.json").write_text(json.dumps({"cli_diff": diffs}, indent=2), encoding="utf-8")
    log(f"done in {time.time() - t_all:.0f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
