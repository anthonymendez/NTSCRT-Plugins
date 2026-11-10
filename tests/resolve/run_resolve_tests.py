#!/usr/bin/env python3
"""NTSCRT plug-in tests inside a running DaVinci Resolve (Studio, external scripting on).

Drives Resolve through its scripting API: builds Fusion comps around the NTSCRT
OpenFX tool, renders with Savers / still exports, and checks the results.

  python tests/resolve/run_resolve_tests.py [--build C:/ntb/main] [--out DIR] [--only alpha,order]
                                            [--quick] [--keep-project]

Tests (see README.md next to this file):
  version   the loaded plug-in reports the version in VERSION
  alpha     Text+ through NTSCRT: Clip keeps alpha = source alpha, Spill only adds alpha
  edit      Edit-page effect on a Text+ title: the transparent area stays clean
  order     random-access renders == sequential renders (default look, Digital, Screen Loop)
  cli       plug-in output == ntscrt-cli on the same frames (default look + every CRT preset)
  depth     8-bit int / 16-bit int / 16-bit float / 32-bit float sources give the same result
  sizes     odd and portrait sizes render at the right size
  proxy     Fusion proxy (half resolution) renders and stays close to full resolution
  speed     ms per frame at 1920x1080 for every CRT preset (informational)

Resolve loads the plug-in from its OFX folder, or from OFX_PLUGIN_PATH at startup:
tests/resolve/launch_resolve_dev.ps1 starts Resolve with a build's bundle.
The tests run in their own project ("NTSCRT Resolve Tests"); the project that was
open is saved first and reopened at the end.
Exit code: 0 all passed (skips allowed), 1 a test failed, 2 setup error.
"""
import argparse
import datetime
import json
import os
import struct
import subprocess
import sys
import time
import traceback
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PROJECT = "NTSCRT Resolve Tests"
OFX_TOOL = "ofx.com.ntscrt.plugins.ntscrt"
W, H, FPS = 640, 360, 30


# ---------------------------------------------------------------- PNG I/O ---
class Image:
    """8-bit RGBA, top row first."""

    def __init__(self, w, h, px):
        self.w, self.h, self.px = w, h, px

    def channel(self, c):
        return self.px[c::4]


def read_png(path):
    data = Path(path).read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat.append(body)
        elif typ == b"IEND":
            break
    w, h, bd, ct, _, _, interlace = ihdr
    if interlace or ct not in (0, 2, 4, 6) or bd not in (8, 16):
        raise ValueError(f"{path}: unsupported PNG (type {ct}, depth {bd}, interlace {interlace})")
    ch = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    bpp = ch * bd // 8
    stride = w * bpp
    raw = zlib.decompress(b"".join(idat))
    out = bytearray(h * stride)
    prev = bytearray(stride)
    i = 0
    for y in range(h):
        f = raw[i]
        line = bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif f == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif f == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out[y * stride:(y + 1) * stride] = line
        prev = line
    px8 = out[0::2] if bd == 16 else out  # 16-bit: keep the high byte
    n = w * h
    rgba = bytearray(n * 4)
    if ch == 4:
        rgba[:] = px8
    elif ch == 3:
        for c in range(3):
            rgba[c::4] = px8[c::3]
        rgba[3::4] = b"\xff" * n
    elif ch == 2:
        for c in range(3):
            rgba[c::4] = px8[0::2]
        rgba[3::4] = px8[1::2]
    else:
        for c in range(3):
            rgba[c::4] = px8
        rgba[3::4] = b"\xff" * n
    return Image(w, h, rgba)


def write_png(path, img):
    rows = b"".join(b"\x00" + bytes(img.px[y * img.w * 4:(y + 1) * img.w * 4]) for y in range(img.h))

    def chunk(t, b):
        return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xFFFFFFFF)

    Path(path).write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", img.w, img.h, 8, 6, 0, 0, 0))
                           + chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def stats(a, b, mask=None):
    """Mean / max absolute difference (0..255) over RGB and over alpha, optionally where mask[i]."""
    if (a.w, a.h) != (b.w, b.h):
        raise ValueError(f"size mismatch {a.w}x{a.h} vs {b.w}x{b.h}")
    out = {}
    for name, chans in (("rgb", (0, 1, 2)), ("alpha", (3,))):
        tot, mx, n = 0, 0, 0
        for c in chans:
            ca, cb = a.channel(c), b.channel(c)
            if mask is None:
                d = [abs(x - y) for x, y in zip(ca, cb)]
            else:
                d = [abs(x - y) for x, y, m in zip(ca, cb, mask) if m]
            tot += sum(d)
            mx = max(mx, max(d) if d else 0)
            n += len(d)
        out[name] = {"mean": round(tot / n, 3) if n else 0.0, "max": mx}
    return out


def diff_image(a, b, gain=8):
    px = bytearray(a.w * a.h * 4)
    for c in range(3):
        px[c::4] = bytes(min(255, abs(x - y) * gain) for x, y in zip(a.channel(c), b.channel(c)))
    px[3::4] = b"\xff" * (a.w * a.h)
    return Image(a.w, a.h, px)


def mean(seq):
    seq = list(seq)
    return sum(seq) / len(seq) if seq else 0.0


def dilate(mask, w, h, r):
    """Square dilation of a 0/1 mask (separable)."""
    tmp = bytearray(len(mask))
    for y in range(h):
        row = mask[y * w:(y + 1) * w]
        for x in range(w):
            if row[x]:
                tmp[y * w + max(0, x - r):y * w + min(w, x + r + 1)] = b"\x01" * (min(w, x + r + 1) - max(0, x - r))
    out = bytearray(len(mask))
    for x in range(w):
        col = tmp[x::w]
        for y in range(h):
            if col[y]:
                for yy in range(max(0, y - r), min(h, y + r + 1)):
                    out[yy * w + x] = 1
    return out


def downscale2(img):
    w, h = img.w // 2, img.h // 2
    px = bytearray(w * h * 4)
    s = img.px
    for y in range(h):
        r0, r1 = (2 * y) * img.w * 4, (2 * y + 1) * img.w * 4
        for x in range(w):
            i0, i1 = r0 + 8 * x, r1 + 8 * x
            for c in range(4):
                px[(y * w + x) * 4 + c] = (s[i0 + c] + s[i0 + 4 + c] + s[i1 + c] + s[i1 + 4 + c] + 2) >> 2
    return Image(w, h, px)


# ------------------------------------------------------------ Resolve glue ---
def connect():
    g = globals().get("resolve")
    if g:
        return g
    if sys.platform == "win32":
        api = os.environ.get("RESOLVE_SCRIPT_API", r"C:\ProgramData\Blackmagic Design\DaVinci Resolve\Support\Developer\Scripting")
        lib = os.environ.get("RESOLVE_SCRIPT_LIB", r"C:\Program Files\Blackmagic Design\DaVinci Resolve\fusionscript.dll")
    else:
        api = os.environ.get("RESOLVE_SCRIPT_API", "/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/Scripting")
        lib = os.environ.get("RESOLVE_SCRIPT_LIB", "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Libraries/Fusion/fusionscript.so")
    os.environ.setdefault("RESOLVE_SCRIPT_API", api)
    os.environ.setdefault("RESOLVE_SCRIPT_LIB", lib)
    sys.path.append(os.path.join(api, "Modules"))
    import DaVinciResolveScript as dvr  # noqa: E402

    r = dvr.scriptapp("Resolve")
    if not r:
        raise RuntimeError("can't reach Resolve: is it running, Studio, with Preferences > General > "
                           "External scripting using = Local?")
    return r


class Fu:
    """Small helpers over a Fusion composition."""

    def __init__(self, comp, out_dir):
        self.comp, self.out = comp, Path(out_dir)

    def add(self, reg, **inputs):
        self.comp.Lock()
        t = self.comp.AddTool(reg, -32768, -32768)
        self.comp.Unlock()
        if not t:
            raise RuntimeError(f"Fusion couldn't create a {reg} tool")
        for k, v in inputs.items():
            self.set(t, k, v)
        return t

    @staticmethod
    def set(tool, name, value):
        if isinstance(value, (int, float, str)):
            tool.SetInput(name, value)
        else:  # a tool: connect its main output
            setattr(tool, name, value.Output)

    def render(self, tools, frames, tag):
        """Render `tools` ({name: tool}) at `frames` (list, rendered one by one in that order) or
        (start, end) tuple (one render call). Returns {name: {frame: path}}."""
        savers = {}
        self.comp.Lock()
        for name in tools:
            savers[name] = self.comp.AddTool("Saver", -32768, -32768)
        self.comp.Unlock()
        paths = {}
        try:
            for name, tool in tools.items():
                base = self.out / f"{tag}-{name}-.png"
                savers[name].Clip = str(base)
                savers[name].Input = tool.Output
                paths[name] = base
            ranges = [frames] if isinstance(frames, tuple) else [(f, f) for f in frames]
            for a, b in ranges:
                if not self.comp.Render({"Start": a, "End": b, "Wait": True}):
                    raise RuntimeError(f"Fusion render {tag} {a}-{b} failed (see the Fusion console / NTSCRT_OFX_LOG)")
        finally:
            for s in savers.values():
                s.Delete()
        all_frames = list(range(frames[0], frames[1] + 1)) if isinstance(frames, tuple) else list(frames)
        return {name: {f: Path(str(base).replace("-.png", f"-{f:04d}.png")) for f in all_frames}
                for name, base in paths.items()}


def retry(fn, what, timeout=45.0):
    """Resolve refuses calls (returns None) while it's busy, e.g. right after a Fusion render."""
    end = time.time() + timeout
    while True:
        v = fn()
        if v:
            return v
        if time.time() > end:
            raise RuntimeError(f"Resolve kept refusing: {what} (a dialog open in Resolve?)")
        time.sleep(0.5)


def tc_from_frame(frame, fps):
    fps_i = int(round(fps))
    f = frame % fps_i
    s = frame // fps_i
    return f"{s // 3600:02d}:{s // 60 % 60:02d}:{s % 60:02d}:{f:02d}"


def frame_from_tc(tc, fps):
    hh, mm, ss, ff = (int(x) for x in tc.replace(";", ":").split(":"))
    return ((hh * 60 + mm) * 60 + ss) * int(round(fps)) + ff


# ------------------------------------------------------------------ suite ---
class Suite:
    def __init__(self, resolve, args):
        self.r, self.a = resolve, args
        self.out = Path(args.out)
        self.out.mkdir(parents=True, exist_ok=True)
        self.results = []
        self.schema = json.loads((ROOT / "core/params/schema.json").read_text(encoding="utf-8"))
        self.version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.cli, self.res = self.find_cli()

    # -- setup ----------------------------------------------------------
    def find_cli(self):
        b = Path(self.a.build) if self.a.build else ROOT / "build"
        exe = "ntscrt-cli.exe" if sys.platform == "win32" else "ntscrt-cli"
        for c in (b / "tools/ntscrt-cli/Release" / exe, b / "tools/ntscrt-cli" / exe):
            if c.exists():
                cli = c
                break
        else:
            return None, None
        for r in (b / "ofx/Release/NTSCRT.ofx.bundle/Contents/Resources", b / "ofx/NTSCRT.ofx.bundle/Contents/Resources",
                  b / "resources"):
            if (r / "shaders").is_dir():
                return cli, r
        return cli, None

    def open_project(self):
        pm = self.r.GetProjectManager()
        cur = pm.GetCurrentProject()
        self.prev_project = cur.GetName() if cur else None
        if self.prev_project == PROJECT:
            pm.SaveProject()  # closing an unsaved project asks "save changes?" and blocks scripting
            pm.CloseProject(cur)
            self.prev_project = None
        elif self.prev_project:
            pm.SaveProject()
        # A fresh project every run (deleting timelines from the UI side can raise a
        # confirmation dialog that blocks scripting).
        if PROJECT in (pm.GetProjectListInCurrentFolder() or []):
            pm.DeleteProject(PROJECT)
        if not pm.CreateProject(PROJECT):
            raise RuntimeError(f"couldn't create project {PROJECT} (is a dialog open in Resolve?)")
        p = pm.GetCurrentProject()
        mp = p.GetMediaPool()
        for k, v in (("timelineResolutionWidth", str(W)), ("timelineResolutionHeight", str(H)),
                     ("timelineFrameRate", str(FPS)), ("timelinePlaybackFrameRate", str(FPS))):
            if p.GetSetting(k) != v:
                p.SetSetting(k, v)
        self.p, self.mp = p, mp
        self.fps = float(p.GetSetting("timelineFrameRate") or FPS)

    def new_comp(self, name):
        """A timeline holding one Fusion composition clip; returns Fu over its comp."""
        retry(lambda: self.r.OpenPage("edit"), "open the Edit page")
        tl = retry(lambda: self.mp.CreateEmptyTimeline(name), f"create timeline {name}")
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        item = retry(lambda: tl.InsertFusionCompositionIntoTimeline(), "insert a Fusion composition")
        comp = retry(lambda: item.GetFusionCompByIndex(1), "get the Fusion composition")
        retry(lambda: self.r.OpenPage("fusion"), "open the Fusion page")
        return Fu(comp, self.out), item

    def close(self):
        pm = self.r.GetProjectManager()
        pm.SaveProject()
        if self.prev_project and self.prev_project != PROJECT and not self.a.keep_project:
            pm.LoadProject(self.prev_project)

    # -- reporting --------------------------------------------------------
    def record(self, name, status, metrics=None, notes="", images=()):
        self.results.append({"test": name, "status": status, "metrics": metrics or {}, "notes": notes,
                             "images": [str(Path(i).name) for i in images]})
        print(f"[{status.upper():4}] {name}{': ' + notes if notes else ''}", flush=True)

    def run(self, name, fn):
        if self.a.only and name.split(":")[0] not in self.a.only:
            return
        t0 = time.time()
        try:
            fn()
        except Exception as e:  # a crash in one test doesn't stop the others
            self.record(name, "fail", notes=f"error: {e}")
            traceback.print_exc()
        finally:
            print(f"       ({time.time() - t0:.1f}s)", flush=True)

    # -- scene building -----------------------------------------------------
    def scene(self, fu, w=W, h=H, animated=True, text=True):
        """Opaque source: gradient + (animated noise) + title. Returns the top tool."""
        bg = fu.add("Background", Width=w, Height=h, Type="Corner",
                    TopLeftRed=0.1, TopLeftGreen=0.3, TopLeftBlue=0.8,
                    TopRightRed=0.8, TopRightGreen=0.1, TopRightBlue=0.4,
                    BottomLeftRed=0.1, BottomLeftGreen=0.6, BottomLeftBlue=0.3,
                    BottomRightRed=0.9, BottomRightGreen=0.7, BottomRightBlue=0.1)
        top = bg
        if animated:
            noise = fu.add("FastNoise", Width=w, Height=h, Detail=4, Contrast=1.5, XScale=8, SeetheRate=0.3)
            top = fu.add("Merge", Background=top, Foreground=noise, Blend=0.35)
        if text:
            top = fu.add("Merge", Background=top, Foreground=self.title(fu))
        return top

    def title(self, fu, size=0.16):
        return fu.add("TextPlus", StyledText="NTSCRT", Size=size, Red1=1.0, Green1=0.85, Blue1=0.15)

    def ntscrt(self, fu, source, **params):
        fx = fu.add(OFX_TOOL)
        fx.Source = source.Output
        for k, v in params.items():
            fx.SetInput(k, v)
        return fx

    def crt_presets(self):
        p = next(p for p in self.schema["params"] if p["id"] == "crt.preset")
        return [o["value"] for o in p["options"]]

    # -- tests ----------------------------------------------------------------
    def t_version(self):
        fu, _ = self.new_comp("version")
        fx = self.ntscrt(fu, fu.add("Background"))
        v = fx.GetInput("host_version")
        ok = v == self.version
        self.record("version", "pass" if ok else "fail", {"plugin": v, "VERSION": self.version},
                    "" if ok else "Resolve loaded a different build: install it, or start Resolve with "
                                  "tests/resolve/launch_resolve_dev.ps1")

    def t_alpha(self):
        fu, _ = self.new_comp("alpha")
        tx = self.title(fu)
        fx = self.ntscrt(fu, tx)
        imgs = {}
        for mode, label in ((0, "clip"), (1, "spill")):
            fx.SetInput("alpha_mode", mode)
            r = fu.render({"src": tx, "fx": fx}, [5], f"alpha-{label}")
            imgs[label] = (read_png(r["src"][5]), read_png(r["fx"][5]))
        src, clip = imgs["clip"]
        _, spill = imgs["spill"]
        n = src.w * src.h
        sa, ca, pa = src.channel(3), clip.channel(3), spill.channel(3)
        clip_alpha_err = max(abs(a - b) for a, b in zip(sa, ca))
        empty = [i for i in range(n) if sa[i] == 0]
        spill_below = sum(1 for a, b in zip(sa, pa) if b + 1 < a)
        spill_extra = sum(1 for i in empty if pa[i] > 8) / max(1, len(empty))
        inside = bytearray(1 if a > 250 else 0 for a in sa)
        changed = stats(src, clip, inside)["rgb"]["mean"]
        write_png(self.out / "alpha-clip-diff.png", diff_image(src, clip))
        m = {"clip_alpha_max_err": clip_alpha_err, "spill_pixels_below_source_alpha": spill_below,
             "spill_fraction_of_empty_area_lit": round(spill_extra, 4), "clip_rgb_change_inside": changed,
             "empty_pixels": len(empty)}
        problems = []
        if clip_alpha_err > 2:
            problems.append("Clip: output alpha differs from the source alpha")
        if spill_below > 0:
            problems.append("Spill: output alpha below the source alpha")
        if spill_extra <= 0.001:
            problems.append("Spill: nothing spills outside the element")
        if changed < 1.0:
            problems.append("the effect didn't change the element")
        if not empty:
            problems.append("test setup: the title has no transparent area")
        self.record("alpha", "fail" if problems else "pass", m, "; ".join(problems), ["alpha-clip-diff.png"])

    def t_edit(self):
        """Edit page: NTSCRT applied to a Text+ title (default Clip mode)."""
        effects = [e for e in self.r.GetEffectsList() if "ntscrt" in (e.get("name", "") + e.get("id", "")).lower()]
        if not effects:
            return self.record("edit", "fail", notes="NTSCRT isn't in Resolve's effect list")
        retry(lambda: self.r.OpenPage("edit"), "open the Edit page")
        tl = retry(lambda: self.mp.CreateEmptyTimeline("edit-page"), "create timeline edit-page")
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        item = tl.InsertFusionTitleIntoTimeline("Text+")
        if not item:
            return self.record("edit", "fail", notes="couldn't insert a Text+ title")
        comp = item.GetFusionCompByIndex(1)
        tmpl = next((t for t in comp.GetToolList(False).values() if t.GetAttrs("TOOLS_RegID") == "TextPlus"), None)
        if tmpl:
            tmpl.SetInput("StyledText", "NTSCRT")
            tmpl.SetInput("Size", 0.16)
        eff = item.AddEffect("ofx", effects[0]["id"])
        if not eff:
            return self.record("edit", "fail", notes=f"AddEffect({effects[0]['id']}) failed")
        retry(lambda: self.r.OpenPage("color"), "open the Color page")
        tl.SetCurrentTimecode(tc_from_frame(frame_from_tc(tl.GetStartTimecode(), self.fps) + 10, self.fps))
        shots = {}
        for label, on in (("off", False), ("on", True)):
            eff.SetIsEnabled(on)
            time.sleep(2)
            path = self.out / f"edit-{label}.png"
            if not self.p.ExportCurrentFrameAsStill(str(path)):
                return self.record("edit", "fail", notes="ExportCurrentFrameAsStill failed")
            shots[label] = read_png(path)
        off, on = shots["off"], shots["on"]
        lit = bytearray(1 if max(off.px[i * 4:i * 4 + 3]) > 24 else 0 for i in range(off.w * off.h))
        near = dilate(lit, off.w, off.h, 24)
        far = bytearray(1 - m for m in near)
        outside = stats(off, on, far)["rgb"]
        inside = stats(off, on, lit)["rgb"]
        write_png(self.out / "edit-diff.png", diff_image(off, on))
        m = {"effects_found": len(effects), "outside_mean": outside["mean"], "outside_max": outside["max"],
             "inside_mean": inside["mean"], "title_pixels": sum(lit)}
        problems = []
        if sum(lit) == 0:
            problems.append("test setup: no title visible in the still")
        if outside["mean"] > 0.5:
            problems.append("Clip mode changed the empty area around the title (alpha not respected)")
        if inside["mean"] < 1.0:
            problems.append("the effect didn't change the title")
        self.record("edit", "fail" if problems else "pass", m, "; ".join(problems),
                    ["edit-off.png", "edit-on.png", "edit-diff.png"])

    def order_case(self, label, params):
        fu, _ = self.new_comp(f"order-{label}")
        src = self.scene(fu)
        n = 8 if self.a.quick else 16
        seq_fx = self.ntscrt(fu, src, **params)
        seq = fu.render({"fx": seq_fx}, (0, n - 1), f"order-{label}-seq")["fx"]
        # A fresh tool (fresh plug-in instance, nothing cached) rendering out of order.
        rnd_fx = self.ntscrt(fu, src, **params)
        order = [n - 1, 3, n // 2, 0, n - 2, 1]
        rnd = fu.render({"fx": rnd_fx}, order, f"order-{label}-rnd")["fx"]
        worst = {"mean": 0.0, "max": 0, "frame": None}
        for f in order:
            s = stats(read_png(seq[f]), read_png(rnd[f]))["rgb"]
            if s["mean"] > worst["mean"] or worst["frame"] is None:
                worst = {"mean": s["mean"], "max": s["max"], "frame": f}
        if worst["mean"] > 0.05:
            a, b = read_png(seq[worst["frame"]]), read_png(rnd[worst["frame"]])
            write_png(self.out / f"order-{label}-diff.png", diff_image(a, b))
        ok = worst["mean"] <= 0.05
        self.record(f"order:{label}", "pass" if ok else "fail", {"frames": n, "random_order": order, "worst": worst},
                    "" if ok else f"frame {worst['frame']} differs when rendered out of order",
                    [] if ok else [f"order-{label}-diff.png"])

    def t_order(self):
        self.order_case("default", {})
        self.order_case("digital", {"digital_enabled": 1})
        self.order_case("screenloop", {"screenloop_enabled": 1})

    def run_cli(self, seq_pattern, out_pattern, first, count, look):
        look_path = self.out / "cli-look.json"
        look_path.write_text(json.dumps(look), encoding="utf-8")
        cmd = [str(self.cli), "--seq", str(seq_pattern), "--in", str(seq_pattern).replace("%04d", f"{first:04d}"),
               "--out", str(out_pattern), "--frame", str(first), "--frames", str(count), "--fps", str(self.fps),
               "--look", str(look_path), "--resources", str(self.res)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError(f"ntscrt-cli failed: {r.stderr.strip()}")

    def t_cli(self):
        if not self.cli or not self.res:
            return self.record("cli", "skip", notes="ntscrt-cli or bundle resources not found (pass --build)")
        fu, _ = self.new_comp("cli")
        src = self.scene(fu)
        fx = self.ntscrt(fu, src)
        n = 4 if self.a.quick else 8
        r = fu.render({"src": src, "fx": fx}, (0, n - 1), "cli-default")
        pat = Path(str(r["src"][0]).replace("-0000.png", "-%04d.png"))
        self.run_cli(pat, self.out / "cli-default-ref-%04d.png", 0, n, {})
        worst = max((stats(read_png(r["fx"][f]), read_png(self.out / f"cli-default-ref-{f:04d}.png"))["rgb"]["mean"], f)
                    for f in range(n))
        metrics = {"default_worst_mean": worst[0], "default_worst_frame": worst[1], "presets": {}}
        problems = [] if worst[0] <= 2.0 else [f"default look: frame {worst[1]} mean diff {worst[0]}"]
        presets = self.crt_presets()
        for i, preset in enumerate(presets):
            fx.SetInput("crt_preset", i)
            rr = fu.render({"fx": fx}, [0], f"cli-{preset}")
            self.run_cli(pat, self.out / f"cli-{preset}-ref-%04d.png", 0, 1,
                         {"shader": {"enabled": True, "preset": preset}})
            a, b = read_png(rr["fx"][0]), read_png(self.out / f"cli-{preset}-ref-0000.png")
            s = stats(a, b)["rgb"]["mean"]
            metrics["presets"][preset] = s
            if s > 2.0:
                write_png(self.out / f"cli-{preset}-diff.png", diff_image(a, b))
                problems.append(f"CRT {preset}: mean diff {s}")
        self.record("cli", "fail" if problems else "pass", metrics, "; ".join(problems))

    def t_depth(self):
        fu, _ = self.new_comp("depth")
        tools = []
        src = self.scene(fu)
        for t in fu.comp.GetToolList(False).values():
            if t.GetAttrs("TOOLS_RegID") in ("Background", "FastNoise", "TextPlus", "Merge"):
                tools.append(t)
        fx = self.ntscrt(fu, src)
        out = {}
        for depth, label in ((4, "float32"), (1, "int8"), (2, "int16"), (3, "float16")):
            for t in tools:
                t.SetInput("Depth", depth)
            out[label] = read_png(fu.render({"fx": fx}, [2], f"depth-{label}")["fx"][2])
        m, problems = {}, []
        for label in ("int8", "int16", "float16"):
            s = stats(out["float32"], out[label])["rgb"]
            m[label] = s
            if s["mean"] > 1.5:
                problems.append(f"{label} differs from float32 (mean {s['mean']})")
        self.record("depth", "fail" if problems else "pass", m, "; ".join(problems))

    def t_sizes(self):
        m, problems = {}, []
        for w, h in ((1279, 719), (1080, 1920), (320, 240)):
            fu, _ = self.new_comp(f"size-{w}x{h}")
            src = self.scene(fu, w, h, animated=False, text=False)
            fx = self.ntscrt(fu, src)
            img = read_png(fu.render({"fx": fx}, [0], f"size-{w}x{h}")["fx"][0])
            bright = mean(img.channel(1)[::97])
            m[f"{w}x{h}"] = {"out": f"{img.w}x{img.h}", "mean_green": round(bright, 1)}
            if (img.w, img.h) != (w, h):
                problems.append(f"{w}x{h} rendered as {img.w}x{img.h}")
            elif bright < 5:
                problems.append(f"{w}x{h} rendered black")
        self.record("sizes", "fail" if problems else "pass", m, "; ".join(problems))

    def t_proxy(self):
        fu, _ = self.new_comp("proxy")
        src = self.scene(fu, animated=False)
        fx = self.ntscrt(fu, src)
        full = read_png(fu.render({"fx": fx}, [0], "proxy-full")["fx"][0])
        fx2 = self.ntscrt(fu, src)
        sav = fu.comp.AddTool("Saver", -32768, -32768)
        base = self.out / "proxy-half-.png"
        sav.Clip = str(base)
        sav.Input = fx2.Output
        ok = fu.comp.Render({"Start": 0, "End": 0, "Wait": True, "Proxy": 2})
        sav.Delete()
        path = self.out / "proxy-half-0000.png"
        if not ok or not path.exists():
            return self.record("proxy", "fail", notes="proxy render failed")
        half = read_png(path)
        if (half.w, half.h) == (full.w, full.h):
            return self.record("proxy", "skip", {"size": f"{half.w}x{half.h}"},
                               "Fusion ignored the proxy setting for this render")
        ref = downscale2(full)
        if (ref.w, ref.h) != (half.w, half.h):
            return self.record("proxy", "fail", notes=f"proxy size {half.w}x{half.h}, expected {ref.w}x{ref.h}")
        s = stats(ref, half)["rgb"]
        ok = s["mean"] <= 12
        if not ok:
            write_png(self.out / "proxy-diff.png", diff_image(ref, half))
        self.record("proxy", "pass" if ok else "fail", {"vs_full_downscaled": s},
                    "" if ok else "half-resolution render looks too different from full resolution")

    def t_speed(self):
        fu, _ = self.new_comp("speed")
        src = self.scene(fu, 1920, 1080)
        fx = self.ntscrt(fu, src)
        n = 3 if self.a.quick else 8
        fx.SetAttrs({"TOOLB_PassThrough": True})
        t0 = time.time()
        fu.render({"fx": fx}, (0, n - 1), "speed-base")
        base = (time.time() - t0) / n
        fx.SetAttrs({"TOOLB_PassThrough": False})
        m = {"frames": n, "baseline_ms": round(base * 1000)}
        for i, preset in enumerate(self.crt_presets()):
            fx.SetInput("crt_preset", i)
            t0 = time.time()
            fu.render({"fx": fx}, (n * (i + 1), n * (i + 2) - 1), f"speed-{preset}")
            m[preset] = round(((time.time() - t0) / n - base) * 1000)
        self.record("speed", "pass", m, "ms per 1920x1080 frame added by NTSCRT (informational)")

    # -- main -------------------------------------------------------------------
    def main(self):
        # Batch (quiet) mode: no "render completed" or other Fusion dialogs, which
        # would block every later scripting call until someone clicks them.
        self.fusion = self.r.Fusion()
        if self.fusion:
            self.fusion.SetBatch(True)
        self.open_project()
        try:
            for name, fn in (("version", self.t_version), ("alpha", self.t_alpha), ("edit", self.t_edit),
                             ("order", self.t_order), ("cli", self.t_cli), ("depth", self.t_depth),
                             ("sizes", self.t_sizes), ("proxy", self.t_proxy), ("speed", self.t_speed)):
                self.run(name, fn)
        finally:
            self.close()
            if self.fusion:
                self.fusion.SetBatch(False)
        return self.report()

    def report(self):
        summary = {s: sum(1 for r in self.results if r["status"] == s) for s in ("pass", "fail", "skip")}
        doc = {"when": datetime.datetime.now().isoformat(timespec="seconds"), "resolve": self.r.GetVersionString(),
               "version": self.version, "summary": summary, "results": self.results}
        (self.out / "report.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")
        lines = [f"# NTSCRT Resolve tests — {doc['when']}", "",
                 f"Resolve {doc['resolve']}, NTSCRT {self.version}: **{summary['pass']} passed, "
                 f"{summary['fail']} failed, {summary['skip']} skipped**", "",
                 "| Test | Result | Notes | Metrics |", "|---|---|---|---|"]
        for r in self.results:
            lines.append(f"| {r['test']} | {r['status']} | {r['notes']} | `{json.dumps(r['metrics'])}` |")
        (self.out / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"\n{summary['pass']} passed, {summary['fail']} failed, {summary['skip']} skipped -> {self.out}")
        return 1 if summary["fail"] else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--build", help="CMake build dir (finds ntscrt-cli and the bundle's resources)")
    ap.add_argument("--out", default=None, help="output folder (default: a new temp folder)")
    ap.add_argument("--only", default="", help="comma-separated test names (version,alpha,edit,order,cli,depth,sizes,proxy,speed)")
    ap.add_argument("--quick", action="store_true", help="fewer frames")
    ap.add_argument("--keep-project", action="store_true", help="stay in the test project afterwards")
    a = ap.parse_args()
    a.only = {s.strip() for s in a.only.split(",") if s.strip()}
    if not a.out:
        import tempfile
        a.out = os.path.join(tempfile.gettempdir(), "ntscrt-resolve-tests",
                             datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    try:
        r = connect()
    except Exception as e:
        print(f"setup: {e}", file=sys.stderr)
        return 2
    return Suite(r, a).main()


if __name__ == "__main__":
    sys.exit(main())
