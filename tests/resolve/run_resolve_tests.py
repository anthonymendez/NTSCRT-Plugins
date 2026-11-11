#!/usr/bin/env python3
"""NTSCRT plug-in tests inside a running DaVinci Resolve (Studio, external scripting on).

Drives Resolve through its scripting API: builds Fusion comps around the NTSCRT
OpenFX tool, renders frames through the render queue (Deliver) and checks them.
No Fusion Savers: a scripted Fusion render pops a modal "Render completed" box.

  python tests/resolve/run_resolve_tests.py [--build C:/ntb/main] [--out DIR] [--only alpha,order]
                                            [--quick] [--keep-project]

Tests (see README.md next to this file):
  version   the loaded plug-in reports the version in VERSION
  alpha     Text+ through NTSCRT: Clip keeps alpha = source alpha, Spill only adds alpha
  edit      Edit-page effect on a Text+ title: the transparent area stays clean
  order     random-access renders == sequential renders (default look, Digital, Screen Loop)
  cli       plug-in output == ntscrt-cli on the same frames (default look + every CRT preset +
            the bundled Digital video looks, chosen from the plug-in's Preset menu)
  depth     8-bit int / 16-bit int / 16-bit float / 32-bit float sources give the same result
  precision float projects keep the CRT output finer than 8 bits (a graded ramp isn't banded)
  sizes     odd and portrait sizes render at the right size
  speed     ms per frame at 1920x1080 for every CRT preset (informational)
  debug     "Debug logging" logs every frame (and doesn't change the picture); "Open logs folder"
            is pressed if Fusion scripting can press it (only logged, no window, under --headless)

Resolve loads the plug-in from its OFX folder, or from OFX_PLUGIN_PATH at startup:
tests/resolve/launch_resolve_dev.ps1 starts Resolve with a build's bundle.
The tests run in their own project ("NTSCRT Resolve Tests"); the project that was
open is saved first and reopened at the end.
Unattended: --headless starts Resolve with -nogui (no UI, so no dialog can block), runs, and
quits it; --bundle <NTSCRT.ofx.bundle> tests a build without installing it.
Exit code: 0 all passed (skips allowed), 1 a test failed, 2 setup error.
"""
import argparse
import datetime
import json
import os
import re
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
    mod = os.path.join(api, "Modules")
    if mod not in sys.path:
        sys.path.append(mod)
    import DaVinciResolveScript as dvr  # noqa: E402

    r = dvr.scriptapp("Resolve")
    if not r:
        raise RuntimeError("can't reach Resolve: is it running, Studio, with Preferences > General > "
                           "External scripting using = Local?")
    return r


RESOLVE_EXE = {
    "win32": r"C:\Program Files\Blackmagic Design\DaVinci Resolve\Resolve.exe",
    "darwin": "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/MacOS/Resolve",
}.get(sys.platform, "/opt/resolve/bin/resolve")


def resolve_running():
    try:
        if sys.platform == "win32":
            out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq Resolve.exe", "/NH"], capture_output=True, text=True).stdout
            return "Resolve.exe" in out
        return subprocess.run(["pgrep", "-x", "Resolve"], capture_output=True).returncode == 0
    except OSError:
        return False


def start_headless(bundle, log_dir):
    """Start Resolve with -nogui (no windows, so no dialog can block the tests). With `bundle`,
    a copy of that NTSCRT.ofx.bundle is loaded through OFX_PLUGIN_PATH instead of / next to the
    installed one. Returns the Popen."""
    import shutil
    import tempfile
    if resolve_running():
        raise RuntimeError("--headless starts its own Resolve: quit DaVinci Resolve first")
    env = dict(os.environ)
    if bundle:
        b = Path(bundle)
        if not (b / "Contents").is_dir():
            raise RuntimeError(f"--bundle: {b} is not an NTSCRT.ofx.bundle")
        dev = Path(tempfile.gettempdir()) / "ntscrt-ofx-dev"
        shutil.rmtree(dev, ignore_errors=True)
        shutil.copytree(b, dev / "NTSCRT.ofx.bundle")
        env["OFX_PLUGIN_PATH"] = str(dev)
        # An installed NTSCRT of the same version would otherwise win over the copy (Resolve keeps
        # one plug-in per id + version): this marks the copy as a dev build that outranks it.
        env["NTSCRT_OFX_DEV_BUNDLE"] = str(dev)
    env.setdefault("NTSCRT_OFX_LOG", str(Path(log_dir) / "ntscrt-ofx.log"))
    if not Path(RESOLVE_EXE).exists():
        raise RuntimeError(f"Resolve not found at {RESOLVE_EXE}")
    print(f"starting Resolve headless ({RESOLVE_EXE} -nogui)" + (f" with {bundle}" if bundle else ""), flush=True)
    return subprocess.Popen([RESOLVE_EXE, "-nogui"], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class Fu:
    """Small helpers over the Fusion composition of a timeline clip. Frames are captured
    through Resolve's render queue (Deliver), not Fusion Savers: a scripted Fusion render
    pops a modal "Render completed" box that blocks scripting until someone clicks it."""

    def __init__(self, suite, comp, tl):
        self.suite, self.comp, self.tl, self.out = suite, comp, tl, suite.out
        self.media_out = next(t for t in comp.GetToolList(False).values() if t.GetAttrs("TOOLS_RegID") == "MediaOut")
        self.mattes = None

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

    def capture(self, tool, frames, tag):
        """Route `tool` to MediaOut and render `frames` (a list rendered one job per frame in that
        order, or an inclusive (start, end) tuple as one job). Returns {frame: png path}."""
        self.media_out.Input = tool.Output
        return self.suite.render_timeline(self.tl, frames, tag)

    def render(self, tools, frames, tag, alpha=False):
        """{name: {frame: path}} for each of `tools` ({name: tool}). alpha=True recovers the tool's
        alpha from renders over black and over white (straight RGBA PNGs are written)."""
        out = {}
        for name, tool in tools.items():
            if not alpha:
                out[name] = self.capture(tool, frames, f"{tag}-{name}")
                continue
            if not self.mattes:
                black = self.add("Background", TopLeftRed=0, TopLeftGreen=0, TopLeftBlue=0, TopLeftAlpha=1)
                white = self.add("Background", TopLeftRed=1, TopLeftGreen=1, TopLeftBlue=1, TopLeftAlpha=1)
                self.mattes = (self.add("Merge", Background=black), self.add("Merge", Background=white))
            mb, mw = self.mattes
            mb.Foreground = tool.Output
            mw.Foreground = tool.Output
            ob = self.capture(mb, frames, f"{tag}-{name}-overblack")
            ow = self.capture(mw, frames, f"{tag}-{name}-overwhite")
            out[name] = {}
            for f in ob:
                b, w = read_png(ob[f]), read_png(ow[f])
                px = bytearray(b.px)
                # over black = c*a (premultiplied); over white = c*a + (1 - a), so a = 1 - (white - black)
                px[3::4] = bytes(max(0, min(255, 255 - (wv - bv))) for wv, bv in zip(w.channel(1), b.channel(1)))
                path = self.out / f"{tag}-{name}-{f:04d}.png"
                write_png(path, Image(b.w, b.h, px))
                out[name][f] = path
        return out


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
        if self.prev_project and self.prev_project.startswith("Untitled Project"):
            # Resolve's blank startup project: saving it opens a modal "name this project" dialog.
            self.prev_project = None
        # Closing, deleting or deleting timelines from the open project raises
        # confirmation dialogs that block scripting, so: reuse the test project when
        # it's open (each run's timelines get unique names), otherwise recreate it.
        if self.prev_project != PROJECT:
            if self.prev_project:
                pm.SaveProject()  # a named project: saves silently
            if PROJECT in (pm.GetProjectListInCurrentFolder() or []):
                pm.DeleteProject(PROJECT)
            if not pm.CreateProject(PROJECT) and not pm.LoadProject(PROJECT):
                raise RuntimeError(f"couldn't create project {PROJECT} (is a dialog open in Resolve?)")
        self.run_id = datetime.datetime.now().strftime("%H%M%S")
        p = pm.GetCurrentProject()
        mp = p.GetMediaPool()
        for k, v in (("timelineResolutionWidth", str(W)), ("timelineResolutionHeight", str(H)),
                     ("timelineFrameRate", str(FPS)), ("timelinePlaybackFrameRate", str(FPS))):
            if p.GetSetting(k) != v:
                p.SetSetting(k, v)
        self.p, self.mp = p, mp
        self.fps = float(p.GetSetting("timelineFrameRate") or FPS)

    def new_timeline(self, name):
        # Every timeline keeps the project size: changing a timeline's resolution from a
        # script crashed Resolve 21.1.2. Other sizes are made inside Fusion instead.
        retry(lambda: self.r.OpenPage("edit"), "open the Edit page")
        name = f"{name} {self.run_id}"
        tl = retry(lambda: self.mp.CreateEmptyTimeline(name), f"create timeline {name}")
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        return tl

    def new_comp(self, name):
        """A timeline holding one Fusion composition clip; returns Fu over its comp."""
        tl = self.new_timeline(name)
        item = retry(lambda: tl.InsertFusionCompositionIntoTimeline(), "insert a Fusion composition")
        comp = retry(lambda: item.GetFusionCompByIndex(1), "get the Fusion composition")
        return Fu(self, comp, tl), item

    def render_timeline(self, tl, frames, tag):
        """Render frames of `tl` (offsets from its start) to PNG through the render queue.
        frames: list (one job per frame, in that order) or inclusive (a, b) tuple (one job).
        Returns {frame: path}, files renamed to <tag>-<frame:04d>.png."""
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        start = tl.GetStartFrame()
        ranges = [frames] if isinstance(frames, tuple) else [(f, f) for f in frames]
        out = {}
        for a, b in ranges:
            custom = f"{tag}-r{a}-"
            retry(lambda: self.p.SetCurrentRenderFormatAndCodec("png", "RGB8"), "set the render format")
            retry(lambda: self.p.SetRenderSettings({
                "SelectAllFrames": False, "MarkIn": start + a, "MarkOut": start + b, "TargetDir": str(self.out),
                "CustomName": custom, "FormatWidth": W, "FormatHeight": H}), "set render settings")
            job = retry(lambda: self.p.AddRenderJob(), "add a render job")
            try:
                retry(lambda: self.p.StartRendering([job], False), "start rendering")
                t0 = time.time()
                while self.p.IsRenderingInProgress():
                    if time.time() - t0 > 600:
                        raise RuntimeError(f"render {tag} {a}-{b} took over 10 minutes")
                    time.sleep(0.1)
                st = self.p.GetRenderJobStatus(job) or {}
                if st.get("JobStatus") != "Complete":
                    raise RuntimeError(f"render {tag} {a}-{b}: {st} (see NTSCRT_OFX_LOG / Resolve's log)")
            finally:
                self.p.DeleteRenderJob(job)
            for f in range(a, b + 1):
                src = self.out / f"{custom}{start + f:08d}.png"
                if not src.exists():
                    raise RuntimeError(f"render {tag}: {src.name} missing")
                dst = self.out / f"{tag}-{f:04d}.png"
                os.replace(src, dst)
                out[f] = dst
        return out

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
        bg = fu.add("Background", UseFrameFormatSettings=0, Width=w, Height=h, Type="Corner",
                    TopLeftRed=0.1, TopLeftGreen=0.3, TopLeftBlue=0.8,
                    TopRightRed=0.8, TopRightGreen=0.1, TopRightBlue=0.4,
                    BottomLeftRed=0.1, BottomLeftGreen=0.6, BottomLeftBlue=0.3,
                    BottomRightRed=0.9, BottomRightGreen=0.7, BottomRightBlue=0.1)
        top = bg
        if animated:
            noise = fu.add("FastNoise", UseFrameFormatSettings=0, Width=w, Height=h, Detail=4, Contrast=1.5, XScale=8, SeetheRate=0.3)
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

    # Bundled looks with Digital video (the cli test applies them through the Preset menu).
    DIGITAL_LOOKS = ("CD-ROM (low quality)", "Chunky webcam", "Webcam (low quality)")

    def look_preset_index(self, name):
        """Index of a bundled look in the plug-in's Preset menu (0 = "(none)"): released presets
        in their shipped order (ofx/ofx_params.cpp released_preset_order), then newer ones
        alphabetically."""
        src = (ROOT / "ofx/ofx_params.cpp").read_text(encoding="utf-8")
        block = src[src.index("released_preset_order() {"):]
        block = block[:block.index("};")]
        released = re.findall(r'"([^"]+)"', block)
        looks = []
        for f in sorted((ROOT / "presets").rglob("*.json")):
            rel = f.relative_to(ROOT / "presets").with_suffix("").as_posix()
            if json.loads(f.read_text(encoding="utf-8")).get("kind") != "ntscrt-video-feedback":
                looks.append(rel)
        order = sorted(looks, key=lambda n: (released.index(n) if n in released else len(released), n.encode()))
        return order.index(name) + 1

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
            r = fu.render({"src": tx, "fx": fx}, [5], f"alpha-{label}", alpha=True)
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
        tl = self.new_timeline("edit-page")
        item = tl.InsertFusionTitleIntoTimeline("Text+")
        if not item:
            return self.record("edit", "fail", notes="couldn't insert a Text+ title")
        comp = item.GetFusionCompByIndex(1)
        tmpl = next((t for t in comp.GetToolList(False).values() if t.GetAttrs("TOOLS_RegID") == "TextPlus"), None)
        if tmpl:
            for k, v in (("StyledText", "NTSCRT"), ("Size", 0.16), ("Red1", 1.0), ("Green1", 0.85), ("Blue1", 0.15)):
                tmpl.SetInput(k, v)
        eff = item.AddEffect("ofx", effects[0]["id"])
        if not eff:
            return self.record("edit", "fail", notes=f"AddEffect({effects[0]['id']}) failed")
        shots = {}
        for label, on in (("off", False), ("on", True)):
            eff.SetIsEnabled(on)
            shots[label] = read_png(self.render_timeline(tl, [10], f"edit-{label}")[10])
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
                    ["edit-off-0010.png", "edit-on-0010.png", "edit-diff.png"])

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
        # Digital video looks: a fresh tool with the look chosen from the Preset menu, several
        # frames (the codec's stream reaches back), against ntscrt-cli given the preset file.
        metrics["looks"] = {}
        for name in self.DIGITAL_LOOKS:
            slug = re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()
            lf = self.ntscrt(fu, src)
            lf.SetInput("host_preset", self.look_preset_index(name))
            # The source from the same render job: Fusion's source frames can differ slightly
            # between jobs (mean ~0.2), and a look with full Aliasing point-samples them.
            rr = fu.render({"src": src, "fx": lf}, (0, n - 1), f"cli-look-{slug}")
            lpat = Path(str(rr["src"][0]).replace("-0000.png", "-%04d.png"))
            look = json.loads((ROOT / "presets" / f"{name}.json").read_text(encoding="utf-8"))
            self.run_cli(lpat, self.out / f"cli-look-{slug}-ref-%04d.png", 0, n, look)
            worst = (0.0, 0)
            for f in range(n):
                a, b = read_png(rr["fx"][f]), read_png(self.out / f"cli-look-{slug}-ref-{f:04d}.png")
                s = stats(a, b)["rgb"]["mean"]
                if s >= worst[0]:
                    worst = (s, f)
                if s > 2.0 and s >= worst[0]:
                    write_png(self.out / f"cli-look-{slug}-diff.png", diff_image(a, b))
            metrics["looks"][name] = {"worst_mean": worst[0], "frame": worst[1]}
            if worst[0] > 2.0:
                problems.append(f"look {name}: frame {worst[1]} mean diff {worst[0]}")
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

    def t_precision(self):
        """Float projects keep the CRT shader's output finer than 8 bits: a dark ramp through Glow (Gauss),
        then a x16 gain (a strong grade). An 8-bit result graded that way only holds multiples of 16 (bands);
        at half-float precision most pixels fall in between. The int8 project is the 8-bit reference."""
        fu, _ = self.new_comp("precision")
        ramp = fu.add("Background", UseFrameFormatSettings=0, Width=W, Height=H, Type="Horizontal",
                      TopLeftRed=0.0, TopLeftGreen=0.0, TopLeftBlue=0.0,
                      TopRightRed=1 / 16, TopRightGreen=1 / 16, TopRightBlue=1 / 16)
        fx = self.ntscrt(fu, ramp, ntsc_enabled=0, glitch_enabled=0,
                         crt_preset=self.crt_presets().index("glow_gauss"))
        grade = fu.add("BrightnessContrast", Input=fx, Gain=16.0)
        m, problems = {}, []
        for depth, label in ((4, "float32"), (3, "float16"), (1, "int8")):
            for t in (ramp, grade):
                t.SetInput("Depth", depth)
            img = read_png(fu.render({"grade": grade}, [2], f"precision-{label}")["grade"][2])
            g = img.channel(1)
            lit = [v for v in g if 8 <= v <= 247]  # away from black and clipped white
            between = sum(1 for v in lit if min(v % 16, 16 - v % 16) > 1)
            m[label] = {"lit": len(lit), "between_multiples_of_16": round(between / max(1, len(lit)), 3),
                        "levels": len(set(lit))}
        if m["int8"]["lit"] < W * H // 8:
            problems.append("test setup: the graded ramp is mostly black or white")
        if m["int8"]["between_multiples_of_16"] > 0.15:
            problems.append(f"test setup: the int8 reference isn't banded ({m['int8']})")
        for label in ("float32", "float16"):
            if m[label]["between_multiples_of_16"] < 0.5:
                problems.append(f"{label}: graded output is still 8-bit banded ({m[label]})")
            if m[label]["levels"] < 2 * m["int8"]["levels"]:
                problems.append(f"{label}: {m[label]['levels']} levels vs {m['int8']['levels']} in int8")
        self.record("precision", "fail" if problems else "pass", m, "; ".join(problems))

    def t_sizes(self):
        """Odd and portrait source sizes inside Fusion. The plug-in renders them at their own size;
        the timeline then fits the result into the 640x360 frame, so this checks the render works and
        the picture lands where it should (not black, not shifted) against the bypassed source."""
        m, problems = {}, []
        for w, h in ((1279, 719), (1080, 1920), (321, 241)):
            fu, _ = self.new_comp(f"size-{w}x{h}")
            src = self.scene(fu, w, h, animated=False, text=False)
            fx = self.ntscrt(fu, src)
            fx.SetAttrs({"TOOLB_PassThrough": True})
            ref = read_png(fu.render({"fx": fx}, [0], f"size-{w}x{h}-src")["fx"][0])
            fx.SetAttrs({"TOOLB_PassThrough": False})
            img = read_png(fu.render({"fx": fx}, [0], f"size-{w}x{h}")["fx"][0])
            d = stats(ref, img)["rgb"]
            bright = mean(img.channel(1)[::97])
            m[f"{w}x{h}"] = {"vs_source": d, "mean_green": round(bright, 1)}
            if bright < 5:
                problems.append(f"{w}x{h} rendered black")
            elif d["mean"] > 40:
                problems.append(f"{w}x{h}: far from the source (mean {d['mean']}): wrong size or offset?")
        self.record("sizes", "fail" if problems else "pass", m, "; ".join(problems))

    def t_speed(self):
        fu, _ = self.new_comp("speed")
        src = self.scene(fu, 1920, 1080)
        fx = self.ntscrt(fu, src)
        n = 3 if self.a.quick else 8
        fu.render({"fx": fx}, (0, 1), "speed-warmup")  # first frames load shaders / caches
        fx.SetAttrs({"TOOLB_PassThrough": True})
        fu.render({"fx": fx}, (2, 3), "speed-warmup-base")
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
        m.update(self.seek_edit_cost(fu, src))
        self.record("speed", "pass", m, "ms per 1920x1080 frame added by NTSCRT; seek/edit: the plug-in's own "
                    "time for one frame (informational)")

    def seek_edit_cost(self, fu, src, frame=140):
        """Glitch on (the default): an instance at frame 0 jumping to `frame` (it simulates every
        frame before it), then the same frame after a CRT edit and after a Glitch edit, then with a
        keyframed Glitch value. Timed from the plug-in's log (debug logging): its own ms for that
        frame, without Resolve's render-job overhead or the other frames Resolve renders meanwhile."""
        log = self.plugin_log()
        fx = self.ntscrt(fu, src)
        if fx.GetInput("host_debug_logging") is None:
            return {}
        fx.SetInput("host_debug_logging", 1)
        fu.render({"fx": fx}, [0], "speed-seek-warmup")  # device, shaders

        def timed(tag):
            start = self.log_size(log)
            fu.render({"fx": fx}, [frame], tag)
            time.sleep(0.3)
            lines = [l for l in self.log_since(log, start).splitlines()
                     if " debug render #" in l and f" t={frame} " in l and " ms=" in l]
            ms = [float(l.split(" ms=")[1].split()[0]) for l in lines]
            looks = [int(l.split(" look_builds=")[1].split()[0]) for l in lines if " look_builds=" in l]
            return {"ms": round(ms[0]) if ms else None, "renders": len(lines),
                    "looks_built": looks[0] if looks else None}

        out = {f"seek_0_to_{frame}": timed("speed-seek")}
        fx.SetInput("crt_glow_gauss_BOOST", 1.3)
        out["crt_edit"] = timed("speed-crt-edit")
        fx.SetInput("glitch_signal_strength", 0.7)
        out["glitch_edit"] = timed("speed-glitch-edit")
        try:  # keyframed: every frame's look differs, so the history is read from the host
            setattr(fx, "glitch_signal_strength", fu.comp.BezierSpline({}))
            fx.SetInput("glitch_signal_strength", 1.0, 0)
            fx.SetInput("glitch_signal_strength", 0.4, 150)
            out["keyed_glitch"] = timed("speed-keyed")
            fx.SetInput("crt_glow_gauss_BOOST", 1.1)
            out["keyed_then_crt_edit"] = timed("speed-keyed-crt-edit")
        except Exception as e:  # scripting can't keyframe here: informational only
            out["keyed_glitch"] = f"skipped: {e}"
        fx.SetInput("host_debug_logging", 0)
        return out

    def plugin_log(self):
        """The log the plug-in in this Resolve writes (start_headless points it into --out)."""
        if self.a.headless:
            return Path(os.environ.get("NTSCRT_OFX_LOG") or Path(self.a.out) / "ntscrt-ofx.log")
        if os.environ.get("NTSCRT_OFX_LOG"):  # only right if Resolve was started with the same env
            return Path(os.environ["NTSCRT_OFX_LOG"])
        if sys.platform == "win32":
            return Path(os.environ.get("LOCALAPPDATA", "")) / "NTSCRT" / "ntscrt-ofx.log"
        return Path.home() / "Library/Logs/NTSCRT/ntscrt-ofx.log"

    @staticmethod
    def log_size(p):
        try:
            return p.stat().st_size
        except OSError:
            return 0

    @staticmethod
    def log_since(p, offset):
        try:
            with open(p, "rb") as f:
                f.seek(offset)
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def t_debug(self):
        log = self.plugin_log()
        fu, _ = self.new_comp("debug")
        src = self.scene(fu)
        fx = self.ntscrt(fu, src)
        if fx.GetInput("host_debug_logging") is None:
            self.record("debug", "fail", {"log": str(log)}, "the loaded NTSCRT has no Debug logging input: Resolve "
                        "loaded an older build (an installed copy of the same version wins without NTSCRT_OFX_DEV_BUNDLE)")
            return
        n = 3 if self.a.quick else 5
        off = read_png(fu.render({"fx": fx}, [2], "debug-off")["fx"][2])
        start = self.log_size(log)
        fx.SetInput("host_debug_logging", 1)
        on = fu.render({"fx": fx}, (2, 2 + n - 1), "debug-on")["fx"]
        time.sleep(0.5)
        text = self.log_since(log, start)
        lines = [l for l in text.splitlines() if " debug render #" in l]
        times = {l.split(" t=")[1].split()[0] for l in lines if " t=" in l}
        same = stats(off, read_png(on[2]))["rgb"]["max"]
        fx.SetInput("host_debug_logging", 0)
        start_off = self.log_size(log)
        fu.render({"fx": fx}, (2 + n, 2 + n + 1), "debug-after")
        after = [l for l in self.log_since(log, start_off).splitlines() if " debug render #" in l]
        m = {"log": str(log), "debug_lines": len(lines), "distinct_times": len(times), "full_look_lines": sum(" look={" in l for l in lines),
             "lines_after_off": len(after), "max_rgb_diff_on_vs_off": same}
        problems = []
        if len(times) < n:
            problems.append(f"{len(times)} frames logged in detail, expected {n}")
        if not any(" look={" in l and "core_ms=" in l for l in lines):
            problems.append("no line with the full look and timings")
        if after:
            problems.append(f"{len(after)} detailed lines after turning it off")
        if same > 0:
            problems.append(f"debug logging changed the picture (max diff {same})")

        # "Open logs folder": Fusion scripting may not be able to press OFX push buttons. With
        # -nogui the plug-in must only log the press (no Explorer / Finder window).
        start_btn = self.log_size(log)
        pressed = False
        for value in (1, True):
            try:
                fx.SetInput("host_open_logs", value)
            except Exception:
                pass
            time.sleep(1)
            if "open logs folder:" in self.log_since(log, start_btn):
                pressed = True
                break
        btn_text = self.log_since(log, start_btn)
        alive = bool(self.r.GetVersionString()) and bool(fu.render({"fx": fx}, [2 + n + 2], "debug-alive")["fx"])
        m.update({"button_pressed_from_script": pressed, "resolve_responsive_after": alive})
        notes = []
        if not alive:
            problems.append("Resolve stopped answering after the button")
        if pressed:
            if self.a.headless and "(no GUI: not opened)" not in btn_text:
                problems.append("button tried to open a window in headless Resolve")
            notes.append("button press logged" + (" (no GUI: not opened)" if "no GUI" in btn_text else ""))
        else:
            notes.append("Fusion scripting can't press OFX push buttons (SetInput is ignored): button not tested")
        if not self.a.headless and not log.exists():
            notes.append(f"no log at {log} (Resolve started with another NTSCRT_OFX_LOG?)")
        self.record("debug", "fail" if problems else "pass", m, "; ".join(problems + notes))

    # -- main -------------------------------------------------------------------
    def main(self):
        self.open_project()
        try:
            for name, fn in (("version", self.t_version), ("alpha", self.t_alpha), ("edit", self.t_edit),
                             ("order", self.t_order), ("cli", self.t_cli), ("depth", self.t_depth),
                             ("precision", self.t_precision), ("sizes", self.t_sizes), ("speed", self.t_speed), ("debug", self.t_debug)):
                self.run(name, fn)
        finally:
            self.close()
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
    ap.add_argument("--only", default="", help="comma-separated test names (version,alpha,edit,order,cli,depth,precision,sizes,speed,debug)")
    ap.add_argument("--quick", action="store_true", help="fewer frames")
    ap.add_argument("--keep-project", action="store_true", help="stay in the test project afterwards")
    ap.add_argument("--headless", action="store_true",
                    help="start Resolve with -nogui for the run and quit it afterwards (Resolve must be closed)")
    ap.add_argument("--bundle", help="with --headless: test this NTSCRT.ofx.bundle without installing it")
    a = ap.parse_args()
    a.only = {s.strip() for s in a.only.split(",") if s.strip()}
    if not a.out:
        import tempfile
        a.out = os.path.join(tempfile.gettempdir(), "ntscrt-resolve-tests",
                             datetime.datetime.now().strftime("%Y%m%d-%H%M%S"))
    proc = None
    try:
        if a.headless:
            Path(a.out).mkdir(parents=True, exist_ok=True)
            r = None
            for attempt in (1, 2):  # Resolve's script server occasionally fails to start: retry once
                proc = start_headless(a.bundle, a.out)
                end = time.time() + 180
                while not r and time.time() < end and proc.poll() is None:  # Resolve takes a while to start
                    time.sleep(3)
                    try:
                        r = connect()
                    except Exception:
                        r = None
                if r:
                    break
                print(f"headless Resolve didn't answer (attempt {attempt}); restarting it", flush=True)
                proc.kill()
                proc.wait(60)
                time.sleep(5)
            if not r:
                proc = None
                raise RuntimeError("headless Resolve didn't answer twice (its scripting server didn't start: "
                                   "see davinci_resolve.log, 'script server')")
        else:
            r = connect()
    except Exception as e:
        print(f"setup: {e}", file=sys.stderr)
        if proc:
            proc.kill()
        return 2
    try:
        return Suite(r, a).main()
    finally:
        if proc:
            r.Quit()
            try:
                proc.wait(60)
            except subprocess.TimeoutExpired:
                proc.kill()


if __name__ == "__main__":
    sys.exit(main())
