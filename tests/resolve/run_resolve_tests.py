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
  animated  an animated bundled preset chosen from the Preset menu becomes keyframes: frames match
            ntscrt-cli --timeline (the preset's keys played per frame) and not the frozen look;
            Horizontal resolution and a Digital video format set what they apply
  depth     8-bit int / 16-bit int / 16-bit float / 32-bit float sources give the same result
  precision float projects keep the CRT output finer than 8 bits (a graded ramp isn't banded)
  sizes     odd and portrait sizes render at the right size
  speed     ms per frame at 1920x1080 for every CRT preset (informational)
  debug     "Debug logging" logs every frame (and doesn't change the picture); "Open logs folder"
            is pressed if Fusion scripting can press it (only logged, no window, under --headless)
  debugall  "Debug logging for all effects" (button): with the effect's checkbox off, every frame is
            logged in detail while it is on and not after it is off; the switch is left as found
  bench     (only with --only bench, headless) Edit-page benchmark: render-queue jobs of 300 frames at UHD and
            1080p, 60 fps, default look / Clean CRT / NTSC off; JSON lines with wall time, the plug-in's
            ms per render (median, p95), render paths (CPU / GPU) and phase means
  position  Edit-page items an hour into the timeline: the frame ranges Resolve reports, an animated
            look applied there (time, keyframes, first render) plays over the clip like
            ntscrt-cli --timeline (headless with --bundle only)

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
import shutil
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


def read_png_channel16(path, c):
    """Channel `c` of a 16-bit RGB / RGBA PNG at full precision (0..65535)."""
    return read_png16(path, (c,))[0]


def read_png16(path, channels=(0, 1, 2)):
    """Channels of a 16-bit RGB / RGBA PNG at full precision (0..65535), one list each."""
    data = Path(path).read_bytes()
    pos, idat, ihdr = 8, [], None
    while pos < len(data):
        (ln,) = struct.unpack(">I", data[pos:pos + 4])
        typ, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"IDAT":
            idat.append(body)
    w, h, bd, ct, _, _, interlace = ihdr
    if bd != 16 or ct not in (2, 6) or interlace:
        raise ValueError(f"{path}: not a 16-bit RGB(A) PNG (type {ct}, depth {bd})")
    bpp = (3 if ct == 2 else 4) * 2
    stride = w * bpp
    raw = zlib.decompress(b"".join(idat))
    prev, outs, i = bytearray(stride), [[] for _ in channels], 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            cc = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                line[x] = (line[x] + a) & 255
            elif f == 2:
                line[x] = (line[x] + b) & 255
            elif f == 3:
                line[x] = (line[x] + ((a + b) >> 1)) & 255
            elif f == 4:
                p = a + b - cc
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - cc)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else cc)) & 255
        for out, c in zip(outs, channels):
            out.extend((line[k] << 8) | line[k + 1] for k in range(c * 2, stride, bpp))
        prev = line
    return outs


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


def start_headless(bundle, log_dir, only=None):
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
        # copy, not copy2: a fresh modification time, so Resolve describes the plug-in again instead
        # of reusing what it cached for the last copy (path, mtime and size). Fusion keeps the GPU
        # render flags it saw then: an NTSCRT_OFX_GPU=0 session would leave the next one on CPU images.
        shutil.copytree(b, dev / "NTSCRT.ofx.bundle", copy_function=shutil.copy)
        env["OFX_PLUGIN_PATH"] = str(dev)
        # An installed NTSCRT of the same version would otherwise win over the copy (Resolve keeps
        # one plug-in per id + version): this marks the copy as a dev build that outranks it.
        env["NTSCRT_OFX_DEV_BUNDLE"] = str(dev)
    env.setdefault("NTSCRT_OFX_LOG", str(Path(log_dir) / "ntscrt-ofx.log"))
    # "Debug logging for all effects" lives in --out, not the user's settings folder, and the
    # shell's NTSCRT_DEBUG_LOG doesn't turn detailed logging on behind the debug tests' back.
    env.setdefault("NTSCRT_SETTINGS_DIR", str(log_dir))
    env.pop("NTSCRT_DEBUG_LOG", None)
    if "bench" in (only or ()):
        env["NTSCRT_OFX_LOG_VERBOSE"] = "1"  # every render's ms and path, for the bench
    # The "position" test's switch: while this file exists the dev copy applies the look it names to
    # new effects (scripts can't set OpenFX params on the Edit page).
    env["NTSCRT_OFX_TEST_PRESET_FILE"] = str(Path(log_dir) / "test-preset.txt")
    os.environ["NTSCRT_OFX_TEST_PRESET_FILE"] = env["NTSCRT_OFX_TEST_PRESET_FILE"]
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

    def render_timeline(self, tl, frames, tag, codec="RGB8"):
        """Render frames of `tl` (offsets from its start) to PNG through the render queue.
        frames: list (one job per frame, in that order) or inclusive (a, b) tuple (one job).
        Returns {frame: path}, files renamed to <tag>-<frame:04d>.png."""
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        start = tl.GetStartFrame()
        ranges = [frames] if isinstance(frames, tuple) else [(f, f) for f in frames]
        out = {}
        for a, b in ranges:
            custom = f"{tag}-r{a}-"
            retry(lambda: self.p.SetCurrentRenderFormatAndCodec("png", codec), "set the render format")
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
        in their shipped order (hostparams/src/params.cpp released_preset_order), then newer ones
        alphabetically."""
        src = (ROOT / "hostparams/src/params.cpp").read_text(encoding="utf-8")
        block = src[src.index("released_preset_order() {"):]
        block = block[:block.index("};")]
        released = re.findall(r'"([^"]+)"', block)
        looks = []
        for f in sorted((ROOT / "presets").rglob("*.json")):
            rel = f.relative_to(ROOT / "presets").with_suffix("").as_posix()
            if json.loads(f.read_text(encoding="utf-8")).get("kind") != "ntscrt-video-feedback":
                looks.append(rel)
        looks += ["Digital video/" + f["name"] for f in self.schema.get("digital_formats", [])]
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

    def run_cli(self, seq_pattern, out_pattern, first, count, look, timeline=False):
        look_path = self.out / "cli-look.json"
        look_path.write_text(json.dumps(look), encoding="utf-8")
        cmd = [str(self.cli), "--seq", str(seq_pattern), "--in", str(seq_pattern).replace("%04d", f"{first:04d}"),
               "--out", str(out_pattern), "--frame", str(first), "--frames", str(count), "--fps", str(self.fps),
               "--look", str(look_path), "--resources", str(self.res)] + (["--timeline"] if timeline else [])
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

    # An animated bundled look: 2 s of keys, every key Linear, NTSC and CRT values moving.
    ANIMATED_LOOK = "Glitch pop"

    def t_animated(self):
        if not self.cli or not self.res:
            return self.record("animated", "skip", notes="ntscrt-cli or bundle resources not found (pass --build)")
        fu, _ = self.new_comp("animated")
        src = self.scene(fu)
        name = self.ANIMATED_LOOK
        slug = re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()
        fx = self.ntscrt(fu, src)
        log = self.plugin_log()
        mark = self.log_size(log)
        t0 = time.time()
        fx.SetInput("host_preset", self.look_preset_index(name))
        apply_s = time.time() - t0
        look = json.loads((ROOT / "presets" / f"{name}.json").read_text(encoding="utf-8"))
        problems, notes = [], []
        # The preset became keyframes: a moving value differs between frames as the timeline says.
        pass_frames = max(1, round(look["timeline"]["duration"] * self.fps))
        keyed = {f: fx.GetInput("ntsc_composite_preemphasis", f) for f in (0, 12, 25)}
        m = {"keyed_composite_preemphasis": keyed, "pass_frames": pass_frames}
        if len({round(v or 0, 6) for v in keyed.values()}) < 2:
            problems.append(f"Composite preemphasis doesn't move over time: {keyed}")
        # Keyed values against the timeline evaluated here (this look's keys are all Linear; toggles hold).
        keys = sorted(look["timeline"]["keys"], key=lambda k: k["t"])

        def expected(name, f):
            t = (f % pass_frames) / (pass_frames - 1)
            a = max((k for k in keys if k["t"] <= t), key=lambda k: k["t"], default=keys[0])
            later = [k for k in keys if k["t"] > t]
            va = a["ntsc"][name]
            if not later or isinstance(va, bool):
                return float(va)
            b = later[0]
            return va + (b["ntsc"][name] - va) * (t - a["t"]) / (b["t"] - a["t"])
        # Informational: Fusion's GetInput(name, time) reads the spline at comp time, which needn't be
        # the render frame (the rendered frames are the check).
        table = {}
        for name in ("vhs_chroma_loss", "ringing", "vhs_edge_wave_enabled", "composite_preemphasis"):
            table[name] = {f: [fx.GetInput(f"ntsc_{name}", f), round(expected(name, f), 6)]
                           for f in (0, 1, 3, 5, 6, 7, 9, 10, 12, 20, 36, 39)}
        m["keyed_values_got_vs_timeline"] = table
        # How Fusion interpolates the sparse keys (0.5.0-rc.3): the largest gap between the spline and the
        # timeline over a pass, as a fraction of the value's swing (informational; the frames are the check).
        vals = [(fx.GetInput("ntsc_composite_preemphasis", f), expected("composite_preemphasis", f)) for f in range(pass_frames)]
        vals = [(a, b) for a, b in vals if a is not None]
        if vals:
            swing = (max(b for _, b in vals) - min(b for _, b in vals)) or 1.0
            m["fusion_spline_max_dev_of_swing"] = round(max(abs(a - b) for a, b in vals) / swing, 5)
        m["apply_preset_s"] = round(apply_s, 3)
        ap = re.search(r"animated preset at t=.*?(\d+) keyframes on (\d+) params in (\d+) ms", self.log_since(log, mark))
        if ap:
            m.update({"keyframes": int(ap.group(1)), "plugin_apply_ms": int(ap.group(3))})
        n = 16 if self.a.quick else 40
        frames = (0, n - 1)
        # source and effect from the same render job (Fusion's source frames differ slightly between jobs)
        r = fu.render({"src": src, "fx": fx}, frames, f"anim-{slug}")
        pat = Path(str(r["src"][0]).replace("-0000.png", "-%04d.png"))
        self.run_cli(pat, self.out / f"anim-{slug}-ref-%04d.png", 0, n, look, timeline=True)
        self.run_cli(pat, self.out / f"anim-{slug}-frozen-%04d.png", 0, n, look)
        worst, frozen_max, moving = (0.0, 0), (0.0, 0), 0
        for f in range(n):
            a = read_png(r["fx"][f])
            s = stats(a, read_png(self.out / f"anim-{slug}-ref-{f:04d}.png"))["rgb"]["mean"]
            z = stats(a, read_png(self.out / f"anim-{slug}-frozen-{f:04d}.png"))["rgb"]["mean"]
            if s >= worst[0]:
                worst = (s, f)
                if s > 2.0:
                    write_png(self.out / f"anim-{slug}-diff.png",
                              diff_image(a, read_png(self.out / f"anim-{slug}-ref-{f:04d}.png")))
            if z >= frozen_max[0]:
                frozen_max = (z, f)
            moving += z > 4.0
        m.update({"frames": n, "worst_mean_vs_timeline": worst[0], "worst_frame": worst[1],
                  "max_mean_vs_frozen": frozen_max[0], "frames_unlike_frozen": moving})
        if worst[0] > 2.0:
            problems.append(f"frame {worst[1]}: mean diff {worst[0]} vs ntscrt-cli --timeline")
        if frozen_max[0] <= 4.0:
            problems.append("the output never departs from the frozen look: the animation isn't applied")
        # Horizontal resolution sets the width; a Digital video format switches the stage on.
        fx2 = self.ntscrt(fu, src)
        ds = next(p for p in self.schema["params"] if p["id"] == "downscale.preset")
        labels = [o["value"] for o in ds["options"]]
        fx2.SetInput("downscale_preset", labels.index("Arcade (384px)"))
        w = fx2.GetInput("downscale_width")
        m["width_after_arcade"] = w
        if w != 384:
            problems.append(f"Horizontal resolution Arcade set the width to {w}, not 384")
        fx2.SetInput("host_preset", self.look_preset_index("Digital video/MiniDV"))
        got = {k: fx2.GetInput(k) for k in ("digital_enabled", "downscale_width", "downscale_preset")}
        m["after_minidv"] = got
        if got["digital_enabled"] != 1 or got["downscale_width"] != 720 or \
                got["downscale_preset"] != labels.index("MiniDV (720px)"):
            problems.append(f"Digital video/MiniDV: {got}")
        self.record("animated", "fail" if problems else "pass", m, "; ".join(problems + notes),
                    [f"anim-{slug}-diff.png"] if worst[0] > 2.0 else [])

    # An animated look on the Edit page, far into a timeline (Resolve's default 01:00:00:00 start).
    POSITION_LOOK = "Glitch pop"

    def edit_page_items(self, tl):
        """Effect targets an hour into `tl`: a Fusion composition clip (media-like: Resolve gives it
        times from 0 at its start) and a Solid Color generator and an Adjustment Clip (no media) when
        scripting can make them take an OpenFX effect. Returns [(kind, item)]."""
        items = []
        tl.SetCurrentTimecode("01:00:10:00")
        comp_item = tl.InsertFusionCompositionIntoTimeline()
        if comp_item:
            items.append(("comp clip", comp_item))
        for kind, make in (("generator", lambda: tl.InsertGeneratorIntoTimeline("Solid Color")),
                           ("adjustment clip", lambda: tl.InsertGeneratorIntoTimeline("Adjustment Clip"))):
            tl.SetCurrentTimecode("01:00:20:00" if kind == "generator" else "01:00:30:00")
            try:
                it = make()
            except Exception:
                it = None
            if it:
                items.append((kind, it))
        return items

    def sample_keys(self, text):
        """The keys of one param the plug-in logged when it applied a look: (host name, [(t, v)])."""
        mk = re.search(r"; keys of ([A-Za-z0-9_]+):([^\r\n]*)", text)
        if not mk:
            return None, []
        keys = []
        for kv in mk.group(2).split(","):
            t, _, v = kv.partition("=")
            try:
                keys.append((float(t), float(v)))
            except ValueError:
                pass
        return mk.group(1), keys

    @staticmethod
    def linear(keys, t):
        if t <= keys[0][0]:
            return keys[0][1]
        for (ta, va), (tb, vb) in zip(keys, keys[1:]):
            if ta <= t <= tb:
                return va + (vb - va) * (t - ta) / (tb - ta) if tb > ta else vb
        return keys[-1][1]

    def interpolation(self, tl, item, fx_id, log):
        """How Resolve interpolates the plug-in's keyframes, on the Fusion page and the Edit page: an
        eased look's sparse keys (no guard keys), the values the host hands the plug-in between keys
        against straight lines between them, as a fraction of the value's swing (0 = linear, which
        the sparse keys assume; hostparams preset_keys)."""
        out = {}
        name = "5 second blips"
        preset_file = os.environ["NTSCRT_OFX_TEST_PRESET_FILE"]
        # Fusion: the instance applies the look when the tool is created; GetInput reads the spline.
        fu, _ = self.new_comp("interp-fusion")
        src = self.scene(fu)
        mark = self.log_size(log)
        Path(preset_file).write_text(f"{name}\n0\n", encoding="utf-8")
        fx = self.ntscrt(fu, src)
        os.remove(preset_file)
        pname, keys = self.sample_keys(self.log_since(log, mark))
        if keys and len(keys) > 2:
            span = (max(v for _, v in keys) - min(v for _, v in keys)) or 1.0
            last = int(min(keys[-1][0], 299))
            devs = [abs((fx.GetInput(pname, f) or 0) - self.linear(keys, f)) for f in range(0, last + 1)]
            out["fusion"] = {"param": pname, "keys": len(keys), "max_dev_of_swing": round(max(devs) / span, 5)}
        # Edit page: another comp clip; every render logs its look (debug logging for all effects).
        retry(lambda: self.p.SetCurrentTimeline(tl), "switch timeline")
        tl.SetCurrentTimecode("01:00:40:00")
        clip = tl.InsertFusionCompositionIntoTimeline()
        if not clip:
            return out
        cfu = Fu(self, clip.GetFusionCompByIndex(1), tl)
        cfu.media_out.Input = self.scene(cfu).Output
        mark = self.log_size(log)
        Path(preset_file).write_text(f"{name}\n0\n", encoding="utf-8")
        eff = clip.AddEffect("ofx", fx_id)
        os.remove(preset_file)
        if not eff:
            return out
        switch = Path(self.a.out) / "debug-logging-all-effects.txt"
        switch.write_text("on\n", encoding="utf-8")
        time.sleep(1.5)  # the plug-in re-reads the switch once a second
        try:
            rel = clip.GetStart() - tl.GetStartFrame()
            out["edit_clip"] = [clip.GetStart(), clip.GetEnd(), len(clip.GetEffects("ofx") or [])]
            self.render_timeline(tl, (rel + 5, rel + 64), "interp-edit")
        finally:
            switch.unlink()
        text = self.log_since(log, mark)
        out["edit_renders_logged"] = len(re.findall(r" render #\d+ ", text))
        pname, keys = self.sample_keys(text)
        p = next((q for q in self.schema["params"] if re.sub(r"[^A-Za-z0-9_]", "_", q["id"]) == pname), None)
        if keys and len(keys) > 2 and p:
            span = (max(v for _, v in keys) - min(v for _, v in keys)) or 1.0
            ptr = [k for k in p["path"].split("/") if k]
            devs = []
            for t, lk in re.findall(r" debug render #\d+ t=([\d.]+) .*? look=(\{.*?\}) (?:feedback=|$)", text, re.M):
                try:
                    v = json.loads(lk)
                    for k in ptr:
                        v = v[k]
                    devs.append(abs(float(v) - self.linear(keys, float(t))))
                except (ValueError, KeyError, TypeError):
                    pass
            if devs:
                out["edit_page"] = {"param": pname, "keys": len(keys), "frames": len(devs),
                                    "max_dev_of_swing": round(max(devs) / span, 5)}
        return out

    def t_position(self):
        """The effect on Edit-page items starting at 01:00:10:00 and later: what frame range and
        times Resolve reports (logged by the plug-in), and an animated look applied there (through
        the dev copy's test preset file, as scripts can't set OpenFX params on the Edit page): the
        time it takes, the keyframes written, the first render, and the frames against
        ntscrt-cli --timeline (same tolerance as the cli test) and against the frozen look."""
        if not self.cli or not self.res:
            return self.record("position", "skip", notes="ntscrt-cli or bundle resources not found (pass --build)")
        preset_file = os.environ.get("NTSCRT_OFX_TEST_PRESET_FILE") if self.a.headless else None
        if not preset_file:
            return self.record("position", "skip", notes="needs --headless --bundle (the dev copy's test preset file)")
        fx_ids = [e["id"] for e in self.r.GetEffectsList() if "ntscrt" in (e.get("name", "") + e.get("id", "")).lower()]
        if not fx_ids:
            return self.record("position", "fail", notes="NTSCRT isn't in Resolve's effect list")
        log = self.plugin_log()
        tl = self.new_timeline("position")
        st = tl.GetStartFrame()
        name = self.POSITION_LOOK
        look = json.loads((ROOT / "presets" / f"{name}.json").read_text(encoding="utf-8"))
        slug = re.sub(r"[^A-Za-z0-9]+", "-", name).strip("-").lower()
        m = {"timeline_start_frame": st, "timeline_start_tc": tl.GetStartTimecode(), "items": {}}
        problems = []
        targets = self.edit_page_items(tl)
        if not targets:
            return self.record("position", "fail", notes="couldn't put any item on the timeline")
        comp = next((it for k, it in targets if k == "comp clip"), None)
        if comp:
            fu = Fu(self, comp.GetFusionCompByIndex(1), tl)
            fu.media_out.Input = self.scene(fu).Output
        n = 8 if self.a.quick else 24
        for kind, item in targets:
            info = {"start": item.GetStart(), "end": item.GetEnd()}
            m["items"][kind] = info
            rel = item.GetStart() - st
            # Source frames first (no effect), then the effect with the look applied at the clip's 6th frame.
            if kind != "adjustment clip":
                srcs = self.render_timeline(tl, (rel + 5, rel + 5 + n - 1), f"pos-{kind[:4]}-src")
            mark = self.log_size(log)
            # The playhead the look is applied at, in the times Resolve gives the effect: from 0 at
            # the clip's start for a clip, timeline frames for items without media (an Adjustment
            # Clip: the report's log). Whether that holds shows in the coverage check below.
            at = 5 if kind == "comp clip" else item.GetStart() + 5
            Path(preset_file).write_text(f"{name}\n{at}\n", encoding="utf-8")
            t0 = time.time()
            eff = item.AddEffect("ofx", fx_ids[0])
            info["add_effect_s"] = round(time.time() - t0, 2)
            try:
                os.remove(preset_file)
            except OSError:
                pass
            if not eff:
                info["note"] = "AddEffect refused (scripting can't apply OpenFX to this item)"
                continue
            t0 = time.time()
            outs = self.render_timeline(tl, (rel + 5, rel + 5 + n - 1), f"pos-{kind[:4]}-fx")
            info["render_s"] = round(time.time() - t0, 2)
            text = self.log_since(log, mark)
            created = re.findall(r"instance #\d+ created.*?;( src_range=.*)", text)
            info["reported_at_create"] = created[0].strip() if created else None
            first = re.search(r"render #1 t=([\d.]+) frame=(-?\d+) .*? ms=(\d+).*?;( src_range=.*)", text)
            if first:
                info.update({"first_render_t": float(first.group(1)), "first_render_frame": int(first.group(2)),
                             "first_render_ms": int(first.group(3)), "reported_at_render": first.group(4).strip()})
            ap = re.search(r"animated preset at t=([\d.]+): (\d+) frames per pass, frames ([\d.]+)\.\.([\d.]+) keyed "
                           r"\(origin ([\d.]+)([^)]*)\), (\d+) keyframes on (\d+) params in (\d+) ms", text)
            if ap:
                info.update({"preset_t": float(ap.group(1)), "keyed_from": float(ap.group(3)), "keyed_to": float(ap.group(4)),
                             "origin": float(ap.group(5)), "range_unknown": bool(ap.group(6)), "keyframes": int(ap.group(7)),
                             "apply_ms": int(ap.group(9))})
            else:
                problems.append(f"{kind}: the look wasn't applied (no 'animated preset' line in the plug-in log)")
                continue
            if kind == "adjustment clip" or not first:
                continue
            # The frames the plug-in rendered against ntscrt-cli --timeline at the same frame numbers
            # (frame = host time - origin, as the plug-in logged it).
            # Renders of the job: the earliest host time is the job's first frame; t - frame = the origin.
            shift = info["first_render_t"] - info["first_render_frame"]
            t_first = min(float(t) for t in re.findall(r"render #\d+ t=([\d.]+) frame=", text))
            base_frame = int(round(t_first - shift))
            seq = self.out / f"pos-{slug}-{kind[:4]}-seq"
            seq.mkdir(exist_ok=True)
            for k in range(n):
                shutil.copyfile(srcs[rel + 5 + k], seq / f"s-{base_frame + k:04d}.png")
            pat = seq / "s-%04d.png"
            self.run_cli(pat, self.out / f"pos-{kind[:4]}-ref-%04d.png", base_frame, n, look, timeline=True)
            self.run_cli(pat, self.out / f"pos-{kind[:4]}-frozen-%04d.png", base_frame, n, look)
            worst, frozen_max = (0.0, 0), 0.0
            for k in range(n):
                a = read_png(outs[rel + 5 + k])
                ref = read_png(self.out / f"pos-{kind[:4]}-ref-{base_frame + k:04d}.png")
                s = stats(a, ref)["rgb"]["mean"]
                if s >= worst[0]:
                    worst = (s, base_frame + k)
                    if s > 2.0:
                        write_png(self.out / f"pos-{kind[:4]}-diff.png", diff_image(a, ref))
                frozen_max = max(frozen_max, stats(a, read_png(self.out / f"pos-{kind[:4]}-frozen-{base_frame + k:04d}.png"))["rgb"]["mean"])
            info.update({"cli_first_frame": base_frame, "worst_mean_vs_timeline": worst[0], "worst_frame": worst[1],
                         "max_mean_vs_frozen": frozen_max})
            if worst[0] > 2.0:
                problems.append(f"{kind}: frame {worst[1]} mean diff {worst[0]} vs ntscrt-cli --timeline")
            if frozen_max <= 4.0:
                problems.append(f"{kind}: the output never departs from the frozen look (the animation isn't there)")
            if not (info["keyed_from"] <= t_first <= info["keyed_to"]):
                problems.append(f"{kind}: keyed {info['keyed_from']}..{info['keyed_to']} doesn't cover the clip's frames ({t_first})")
            if info["keyframes"] > 20000:
                problems.append(f"{kind}: {info['keyframes']} keyframes")
        try:
            m["interpolation"] = self.interpolation(tl, None, fx_ids[0], log)
        except Exception as e:  # informational
            m["interpolation"] = f"error: {e}"
        self.record("position", "fail" if problems else "pass", m, "; ".join(problems))

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
        """Float projects keep the CRT shader's output finer than 8 bits. A ramp through Glow (Gauss) at
        180 lines (so the scanline profile varies smoothly) is delivered as a 16-bit PNG: from an int8
        project every value is k*257 (an 8-bit level), from float32 / float16 projects most aren't."""
        codecs = self.p.GetRenderCodecs("png") or {}
        codec = next((c for c in codecs.values() if "16" in str(c)), None)
        if not codec:
            return self.record("precision", "skip", {"png_codecs": codecs}, "no 16-bit PNG render codec")
        fu, _ = self.new_comp("precision")
        ramp = fu.add("Background", UseFrameFormatSettings=0, Width=W, Height=H, Type="Horizontal",
                      TopLeftRed=0.0, TopLeftGreen=0.0, TopLeftBlue=0.0,
                      TopRightRed=1.0, TopRightGreen=1.0, TopRightBlue=1.0)
        fx = self.ntscrt(fu, ramp, ntsc_enabled=0, glitch_enabled=0, downscale_width=320,
                         crt_preset=self.crt_presets().index("glow_gauss"))
        fu.media_out.Input = fx.Output
        m, problems = {"codec": codec}, []
        for depth, label in ((4, "float32"), (3, "float16"), (1, "int8")):
            ramp.SetInput("Depth", depth)
            path = self.render_timeline(fu.tl, [2], f"precision-{label}", codec=codec)[2]
            g = read_png_channel16(path, 1)
            lit = [v for v in g if 2 * 257 <= v <= 253 * 257]  # away from black and white
            off = sum(1 for v in lit if v % 257 != 0)
            m[label] = {"lit": len(lit), "off_8bit_levels": round(off / max(1, len(lit)), 3),
                        "levels": len(set(lit))}
        if m["int8"]["lit"] < W * H // 4:
            problems.append(f"test setup: the ramp is mostly black or white ({m['int8']})")
        if m["int8"]["off_8bit_levels"] > 0.02:
            problems.append(f"test setup: the int8 reference isn't on 8-bit levels ({m['int8']})")
        for label in ("float32", "float16"):
            if m[label]["off_8bit_levels"] < 0.5:
                problems.append(f"{label}: output is still on 8-bit levels ({m[label]})")
            if m[label]["levels"] < 3 * m["int8"]["levels"]:
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
        m.update(self.screenloop_seek_cost(fu, src))
        self.record("speed", "pass", m, "ms per 1920x1080 frame added by NTSCRT; seek/edit/screenloop: the "
                    "plug-in's own time for one frame (informational)")

    def t_gpupath(self):
        """Frames for the GPU-path A/B (--gpu-ab runs this test once with NTSCRT_OFX_GPU=0 and once
        with the GPU path, then compares): Fusion comps at 1920x1080 (the default look: ntsc-rs, so the
        picture comes down once; NTSC off: all on the GPU; Clean CRT; 8-bit and half-float sources;
        a title over transparency in Clip and Spill mode) and the effect on an Edit-page clip, each
        delivered as 16-bit PNGs, and the sources without the effect (gpupath-src-*: the A/B checks
        Resolve gave both sessions the same pictures). Every Fusion tool works in float: with CPU images
        Fusion hands a plug-in its tools' own depth, with GPU images always float (the int8 / float16
        cases show that difference). Passes when it rendered; the comparison is --gpu-ab's."""
        codecs = self.p.GetRenderCodecs("png") or {}
        codec = next((c for c in codecs.values() if "16" in str(c)), None) or "RGB8"
        frames = (0, 3) if self.a.quick else (0, 5)
        made = []
        fu, _ = self.new_comp("gpupath")
        src = self.scene(fu, 1920, 1080)
        tools = [t for t in fu.comp.GetToolList(False).values()
                 if t.GetAttrs("TOOLS_RegID") in ("Background", "FastNoise", "TextPlus", "Merge")]
        for t in tools:
            t.SetInput("Depth", 4)
        fu.media_out.Input = src.Output
        made += self.render_timeline(fu.tl, frames, "gpupath-src", codec=codec).values()
        cases = [("default", {}), ("ntsc-off", {"ntsc_enabled": 0}),
                 ("clean-crt", {"host_preset": self.look_preset_index("Clean CRT")})]
        for label, params in cases:
            fx = self.ntscrt(fu, src, **params)
            fu.media_out.Input = fx.Output
            made += self.render_timeline(fu.tl, frames, f"gpupath-{label}", codec=codec).values()
        for depth, label in ((1, "int8"), (3, "float16")):
            for t in tools:
                t.SetInput("Depth", depth)
            fx = self.ntscrt(fu, src, ntsc_enabled=0)
            fu.media_out.Input = fx.Output
            made += self.render_timeline(fu.tl, (2, 3), f"gpupath-{label}", codec=codec).values()
        for t in tools:
            t.SetInput("Depth", 4)
        afu, _ = self.new_comp("gpupath-alpha")
        bg = afu.add("Background", TopLeftRed=0.2, TopLeftGreen=0.2, TopLeftBlue=0.25, TopLeftAlpha=1, Depth=4)
        title = self.title(afu, 0.2)
        title.SetInput("Depth", 4)
        m = afu.add("Merge", Background=bg, Foreground=title, Depth=4)
        afu.media_out.Input = m.Output
        made += self.render_timeline(afu.tl, (1, 2), "gpupath-src-alpha", codec=codec).values()
        for mode in (0, 1):  # Clip, Spill: the title over a background after the effect
            fx = self.ntscrt(afu, title, alpha_mode=mode)
            m = afu.add("Merge", Background=bg, Foreground=fx, Depth=4)
            afu.media_out.Input = m.Output
            made += self.render_timeline(afu.tl, (1, 2), f"gpupath-alpha{mode}", codec=codec).values()
        fx_ids = [e["id"] for e in self.r.GetEffectsList() if "ntscrt" in (e.get("name", "") + e.get("id", "")).lower()]
        tl = self.new_timeline("gpupath-edit")
        clip = retry(lambda: tl.InsertFusionCompositionIntoTimeline(), "insert a Fusion composition")
        cfu = Fu(self, clip.GetFusionCompByIndex(1), tl)
        cfu.media_out.Input = self.scene(cfu).Output
        made += self.render_timeline(tl, (0, 3), "gpupath-src-edit", codec=codec).values()
        if fx_ids and clip.AddEffect("ofx", fx_ids[0]):
            made += self.render_timeline(tl, (0, 3), "gpupath-edit", codec=codec).values()
        log = self.plugin_log().read_text(encoding="utf-8", errors="replace") if self.plugin_log().exists() else ""
        paths = {}
        for pth in re.findall(r" ms=[\d.]+ (\S+)", log):
            paths[pth] = paths.get(pth, 0) + 1
        self.record("gpupath", "pass", {"images": len(made), "codec": codec, "paths": paths},
                    "frames for --gpu-ab")

    BENCH_LOOKS = (("default", None, ""), ("clean-crt", "Clean CRT", ""), ("ntsc-off", "(defaults)", "ntsc_enabled=0"))
    BENCH_SIZES = {"uhd": (3840, 2160), "1080p": (1920, 1080)}

    def t_bench(self):
        """Edit-page benchmark (only with --only bench, headless): for each size in --bench-sizes a
        project of its own made at that size and --bench-fps (a project's settings are set before it
        has timelines; timelines' resolutions are never changed), one timeline per look with a Fusion
        composition clip (animated gradient + noise + title at the timeline's size) and NTSCRT added
        to it (the look through the dev copy's test preset file), rendered by one render-queue job of
        --bench-frames frames. Also the clip without the effect (base). Prints one JSON line per run
        ("BENCH {...}"): wall time, the plug-in's own ms per render (median / p95, from the verbose
        log: NTSCRT_OFX_LOG_VERBOSE=1 is set by --headless with --only bench), the paths renders took
        and the mean of each phase (ntscrt_last_timings; NTSCRT_PROFILE=1 in the environment separates
        GPU phases). Informational: never fails on speed."""
        if not self.a.headless:
            return self.record("bench", "skip", notes="needs --headless (its own projects, verbose plug-in log)")
        fx_ids = [e["id"] for e in self.r.GetEffectsList() if "ntscrt" in (e.get("name", "") + e.get("id", "")).lower()]
        if not fx_ids:
            return self.record("bench", "fail", notes="NTSCRT isn't in Resolve's effect list")
        log = self.plugin_log()
        preset_file = os.environ["NTSCRT_OFX_TEST_PRESET_FILE"]
        pm = self.r.GetProjectManager()
        n = self.a.bench_frames
        results = []
        for size in [s for s in self.a.bench_sizes.split(",") if s]:
            w, h = self.BENCH_SIZES[size]
            name = f"NTSCRT Resolve Bench {size}"
            pm.SaveProject()
            if name in (pm.GetProjectListInCurrentFolder() or []):
                pm.DeleteProject(name)
            p = retry(lambda: pm.CreateProject(name), f"create project {name}")
            for k, v in (("timelineResolutionWidth", str(w)), ("timelineResolutionHeight", str(h)),
                         ("timelineFrameRate", str(self.a.bench_fps)), ("timelinePlaybackFrameRate", str(self.a.bench_fps))):
                p.SetSetting(k, v)
            mp = p.GetMediaPool()
            codecs = p.GetRenderCodecs("mp4") or {}
            codec = next((c for d, c in codecs.items() if "NVIDIA" in d and "264" in d), None) or \
                next((c for d, c in codecs.items() if "264" in d), None)
            fmt = ("mp4", codec) if codec else ("png", "RGB8")
            variants = [("base", None, "")] + list(self.BENCH_LOOKS)
            for label, look, opts in variants:
                for rep in range(self.a.bench_repeat):
                    retry(lambda: self.r.OpenPage("edit"), "open the Edit page")
                    tl = retry(lambda: mp.CreateEmptyTimeline(f"bench {label} {rep} {self.run_id}"), "create a timeline")
                    retry(lambda: p.SetCurrentTimeline(tl), "switch timeline")
                    clips = []
                    while sum(c.GetDuration() for c in clips) < n:  # comp clips are a few seconds long
                        c = retry(lambda: tl.InsertFusionCompositionIntoTimeline(), "insert a Fusion composition")
                        cfu = Fu(self, c.GetFusionCompByIndex(1), tl)
                        cfu.media_out.Input = self.scene(cfu, w, h).Output
                        clips.append(c)
                    mark = self.log_size(log)
                    if label != "base":
                        for c in clips:
                            if look:
                                Path(preset_file).write_text(f"{look}\n0\n{opts}\n", encoding="utf-8")
                            try:
                                if not c.AddEffect("ofx", fx_ids[0]):
                                    raise RuntimeError("AddEffect refused")
                            finally:
                                if look:
                                    os.remove(preset_file)
                    start = tl.GetStartFrame()
                    retry(lambda: p.SetCurrentRenderFormatAndCodec(*fmt), "set the render format")
                    retry(lambda: p.SetRenderSettings({
                        "SelectAllFrames": False, "MarkIn": start, "MarkOut": start + n - 1, "TargetDir": str(self.out),
                        "CustomName": f"bench-{size}-{label}-{rep}", "FormatWidth": w, "FormatHeight": h}),
                        "set render settings")
                    job = retry(lambda: p.AddRenderJob(), "add a render job")
                    t0 = time.time()
                    retry(lambda: p.StartRendering([job], False), "start rendering")
                    while p.IsRenderingInProgress():
                        if time.time() - t0 > 1800:
                            raise RuntimeError("bench render took over 30 minutes")
                        time.sleep(0.05)
                    wall = time.time() - t0
                    st = p.GetRenderJobStatus(job) or {}
                    p.DeleteRenderJob(job)
                    time.sleep(0.5)
                    text = self.log_since_rotated(log, mark)
                    ms, paths, phases = [], {}, {}
                    for m in re.finditer(r" render #\d+ t=\S+ .*? ms=([\d.]+)(?: (\S+))?([^\r\n]*)", text):
                        ms.append(float(m.group(1)))
                        path = m.group(2) if m.group(2) and not m.group(2).startswith("look=") else "path=?"
                        paths[path] = paths.get(path, 0) + 1
                        for k, v in re.findall(r" (in|digital|ntsc|upload|downscale|glitch|crt|loop|readback|out)=([\d.]+)", m.group(3)):
                            phases.setdefault(k, []).append(float(v))
                    ms.sort()
                    q = lambda f: round(ms[min(len(ms) - 1, int(f * len(ms)))], 2) if ms else None  # noqa: E731
                    row = {"bench": self.a.bench_label, "size": f"{w}x{h}", "fps": self.a.bench_fps, "look": label,
                           "repeat": rep, "frames": n, "job": st.get("JobStatus"), "codec": "/".join(fmt),
                           "wall_s": round(wall, 2), "renders": len(ms), "ms_median": q(0.5), "ms_p95": q(0.95),
                           "paths": {k.replace("path=", ""): v for k, v in paths.items()},
                           "phase_mean_ms": {k: round(sum(v) / len(v), 2) for k, v in phases.items()},
                           "profile": bool(os.environ.get("NTSCRT_PROFILE")),
                           "gpu_env": os.environ.get("NTSCRT_OFX_GPU", "")}
                    print("BENCH " + json.dumps(row), flush=True)
                    results.append(row)
        (self.out / "bench.jsonl").write_text("".join(json.dumps(r) + "\n" for r in results), encoding="utf-8")
        self.record("bench", "pass", {"runs": len(results)}, "see BENCH lines / bench.jsonl")

    def log_since_rotated(self, p, offset):
        """log_since, following one rotation (the plug-in's log moves to *.old.log at 1 MB)."""
        if self.log_size(p) >= offset:
            return self.log_since(p, offset)
        old = p.with_name(p.stem + ".old" + p.suffix)
        return self.log_since(old, offset) + self.log_since(p, 0)

    def screenloop_seek_cost(self, fu, src):
        """Screen Loop on (default look; the Bottom to top loop: zoomed in, auto exposure): a new
        instance renders frame 140, 141, then back to 125 and 100, then 141 again. Resolve renders
        other frames in between (the comp's current frame: 0), so each job is a jump from there;
        the loop resumes from its checkpoints (every 30 frames) instead of re-running from frame 0.
        Every render of the instance is listed as frame:ms (the plug-in's own time, debug log)."""
        log = self.plugin_log()
        start = self.log_size(log)
        loop = json.loads((ROOT / "presets/Screen Loop/Bottom to top.json").read_text(encoding="utf-8"))
        params = {"screenloop_enabled": 1}
        params.update({f"screenloop_{k}": v for k, v in loop["values"].items()})
        fx = self.ntscrt(fu, src, **params)
        if fx.GetInput("host_debug_logging") is None:
            return {}
        fx.SetInput("host_debug_logging", 1)
        t0 = time.time()
        for f in (140, 141, 125, 100, 141):
            fu.render({"fx": fx}, [f], f"speed-loop-{f}")
        wall = time.time() - t0
        time.sleep(0.3)
        fx.SetInput("host_debug_logging", 0)
        text = self.log_since(log, start)
        ids = re.findall(r"instance #(\d+) created", text)
        if not ids:
            return {}
        renders = [f"{m.group(1)}:{round(float(m.group(2)))}"
                   for m in re.finditer(rf"instance #{ids[-1]} debug render #\d+ t=(\d+) .*? ms=([\d.]+)", text)]
        return {"screenloop_renders_frame_ms": " ".join(renders), "screenloop_jobs_s": round(wall, 1)}

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

    def debug_switch_file(self):
        """The "Debug logging for all effects" switch file the plug-in in this Resolve reads
        (start_headless points NTSCRT_SETTINGS_DIR into --out; hostparams/ntscrt_host/debug_switch.h)."""
        name = "debug-logging-all-effects.txt"
        if self.a.headless:
            return Path(os.environ.get("NTSCRT_SETTINGS_DIR") or self.a.out) / name
        if os.environ.get("NTSCRT_SETTINGS_DIR"):  # only right if Resolve was started with the same env
            return Path(os.environ["NTSCRT_SETTINGS_DIR"]) / name
        if sys.platform == "win32":
            return Path(os.environ.get("LOCALAPPDATA", "")) / "NTSCRT" / name
        return Path.home() / "Library/Logs/NTSCRT" / name

    def t_debugall(self):
        """"Debug logging for all effects" (host_debug_all): with the effect's own checkbox off, every
        frame is logged in detail while the switch is on, and not after it is turned off again. The
        switch is left as it was found (under --headless it lives in --out, not the user's)."""
        log, sw = self.plugin_log(), self.debug_switch_file()
        fu, _ = self.new_comp("debugall")
        src = self.scene(fu)
        fx = self.ntscrt(fu, src)
        if fx.GetInput("host_debug_all_status") is None:
            self.record("debugall", "fail", {"log": str(log)}, "the loaded NTSCRT has no 'Debug logging for all "
                        "effects' input: Resolve loaded an older build")
            return
        was_on = sw.is_file()
        fx.SetInput("host_debug_logging", 0)
        n = 3 if self.a.quick else 4
        m = {"log": str(log), "switch": str(sw), "switch_was_on": was_on}
        problems, notes = [], []

        def press(want_on):
            """Press the button until the plug-in logs that the switch is now `want_on`."""
            start = self.log_size(log)
            for value in (1, 0, 1):
                try:
                    fx.SetInput("host_debug_all", value)
                except Exception:
                    pass
                time.sleep(1)
                if f"debug logging for all NTSCRT effects turned {'on' if want_on else 'off'}" in self.log_since(log, start):
                    return True
            return False

        try:
            phases = [False, True] if was_on else [True, False]
            for k, on in enumerate(phases):
                if not press(on):
                    if k == 0:
                        self.record("debugall", "skip", m, "Fusion scripting couldn't press the 'Debug logging for "
                                    "all effects' button (no log line)")
                        return
                    problems.append(f"second press didn't turn it {'on' if on else 'off'}")
                    break
                status = fx.GetInput("host_debug_all_status")
                m[f"status_{'on' if on else 'off'}"] = status
                if sw.is_file() != on:
                    problems.append(f"switch file {'missing' if on else 'still there'} after turning it {'on' if on else 'off'}")
                if isinstance(status, str) and (status.startswith("ON") != on):
                    problems.append(f"status line says {status!r} with the switch {'on' if on else 'off'}")
                first = 2 + k * (n + 1)
                start = self.log_size(log)
                fu.render({"fx": fx}, (first, first + n - 1), f"debugall-{'on' if on else 'off'}")
                time.sleep(0.5)
                lines = [l for l in self.log_since(log, start).splitlines() if " debug render #" in l]
                times = {l.split(" t=")[1].split()[0] for l in lines if " t=" in l}
                m[f"debug_lines_{'on' if on else 'off'}"] = len(lines)
                if on:
                    if len(times) < n:
                        problems.append(f"switch on, effect's checkbox off: {len(times)} frames logged in detail, expected {n}")
                    if not any("debug_by=all" in l and "core_ms=" in l for l in lines):
                        problems.append("no detailed line marked debug_by=all")
                    if not any(" look={" in l for l in lines):
                        problems.append("no line with the full look")
                elif lines:
                    problems.append(f"{len(lines)} detailed lines with the switch off")
        finally:
            # Never leave the switch other than it was (the user's own, outside --headless).
            try:
                if sw.is_file() != was_on:
                    if was_on:
                        sw.parent.mkdir(parents=True, exist_ok=True)
                        sw.write_text("NTSCRT debug logging for all effects (restored by the Resolve tests)\n")
                    else:
                        sw.unlink()
                    notes.append("switch restored by the test")
            except OSError as e:
                problems.append(f"couldn't restore the switch file {sw}: {e}")
        m["switch_on_after"] = sw.is_file()
        self.record("debugall", "fail" if problems else "pass", m, "; ".join(problems + notes))

    # -- main -------------------------------------------------------------------
    def main(self):
        self.open_project()
        try:
            for name, fn in (("version", self.t_version), ("alpha", self.t_alpha), ("edit", self.t_edit),
                             ("order", self.t_order), ("cli", self.t_cli),
                             ("animated", self.t_animated), ("depth", self.t_depth), ("precision", self.t_precision),
                             ("sizes", self.t_sizes), ("speed", self.t_speed), ("debug", self.t_debug),
                             ("debugall", self.t_debugall), ("position", self.t_position)):
                self.run(name, fn)
            if "bench" in self.a.only:  # long: only when asked for
                self.run("bench", self.t_bench)
            if "gpupath" in self.a.only:  # --gpu-ab
                self.run("gpupath", self.t_gpupath)
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
    ap.add_argument("--only", default="", help="comma-separated test names (version,alpha,edit,order,cli,animated,depth,precision,sizes,speed,debug,debugall,position)")
    ap.add_argument("--quick", action="store_true", help="fewer frames")
    ap.add_argument("--keep-project", action="store_true", help="stay in the test project afterwards")
    ap.add_argument("--headless", action="store_true",
                    help="start Resolve with -nogui for the run and quit it afterwards (Resolve must be closed)")
    ap.add_argument("--bundle", help="with --headless: test this NTSCRT.ofx.bundle without installing it")
    ap.add_argument("--bench-frames", type=int, default=300, help="bench: frames per render job")
    ap.add_argument("--bench-sizes", default="uhd,1080p", help="bench: comma-separated uhd, 1080p")
    ap.add_argument("--bench-fps", type=int, default=60, help="bench: project frame rate")
    ap.add_argument("--bench-repeat", type=int, default=1, help="bench: render jobs per look and size")
    ap.add_argument("--bench-label", default="", help="bench: label copied into each JSON line (e.g. the build)")
    ap.add_argument("--gpu-ab", action="store_true",
                    help="with --headless: render the gpupath frames with NTSCRT_OFX_GPU=0 (CPU path) and with the GPU "
                         "path in two Resolve sessions and compare them (docs/dev/HOST-GPU.md tolerance)")
    ap.add_argument("--ab-modes", default="cpu,gpu",
                    help="--gpu-ab: the two sessions (cpu or gpu each); cpu,cpu / gpu,gpu measure Resolve's own "
                         "session-to-session noise")
    a = ap.parse_args()
    a.only = {s.strip() for s in a.only.split(",") if s.strip()}
    if a.gpu_ab:
        return gpu_ab(a)
    return run_session(a)


def run_session(a):
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
                proc = start_headless(a.bundle, a.out, a.only)
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


def gpu_ab(a):
    """--gpu-ab: the gpupath frames from a CPU-path session and a GPU-path session, compared."""
    import copy
    import tempfile
    if not a.headless:
        print("--gpu-ab needs --headless (one Resolve session per path)", file=sys.stderr)
        return 2
    base = Path(a.out or os.path.join(tempfile.gettempdir(), "ntscrt-resolve-tests",
                                      datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + "-gpu-ab"))
    keep = os.environ.get("NTSCRT_OFX_GPU")
    dirs = {}
    try:
        modes = [m.strip() for m in a.ab_modes.split(",")][:2]
        sessions = [(("cpu" if m == "cpu" else "gpu") + ("2" if i and m == modes[0] else ""), "0" if m == "cpu" else None)
                    for i, m in enumerate(modes)]
        for label, env in sessions:
            if env is None:
                os.environ.pop("NTSCRT_OFX_GPU", None)
            else:
                os.environ["NTSCRT_OFX_GPU"] = env
            sub = copy.copy(a)
            sub.gpu_ab, sub.only, sub.out = False, {"gpupath"}, str(base / label)
            dirs[label] = Path(sub.out)
            os.environ.pop("NTSCRT_OFX_LOG", None)
            rc = run_session(sub)
            if rc != 0:
                print(f"--gpu-ab: the {label} session failed ({rc})", file=sys.stderr)
                return rc
    finally:
        if keep is None:
            os.environ.pop("NTSCRT_OFX_GPU", None)
        else:
            os.environ["NTSCRT_OFX_GPU"] = keep
    first, second = sessions[0][0], sessions[1][0]
    results, worst = [], 0
    for ref in sorted(dirs[first].glob("gpupath-*.png")):
        got = dirs[second] / ref.name
        if not got.exists():
            results.append({"image": ref.name, "missing": True})
            continue
        try:
            ra, ga = read_png16(ref), read_png16(got)
            scale = 1
        except ValueError:  # an 8-bit PNG codec
            ra = [read_png(ref).channel(c) for c in range(3)]
            ga = [read_png(got).channel(c) for c in range(3)]
            scale = 257
        diffs = [abs(x - y) * scale for ca, cb in zip(ra, ga) for x, y in zip(ca, cb)]
        mx = max(diffs)
        off = sum(1 for d in diffs if d > 0)
        worst = max(worst, mx)
        results.append({"image": ref.name, "max_diff_of_255": round(mx / 257, 3), "values_differing": off,
                        "values": len(diffs), "mean_diff_of_255": round(sum(diffs) / len(diffs) / 257, 5)})
    gpu_log = (dirs[second] / "ntscrt-ofx.log").read_text(encoding="utf-8", errors="replace")
    cpu_log = (dirs[first] / "ntscrt-ofx.log").read_text(encoding="utf-8", errors="replace")
    count = lambda t, k: len(re.findall(k, t))  # noqa: E731
    summary = {"images": len(results), "worst_max_diff_of_255": round(worst / 257, 3),
               "gpu_session_renders": {"gpu-direct": count(gpu_log, r" path=gpu-direct"),
                                       "gpu-download": count(gpu_log, r" path=gpu-download"),
                                       "gpu-cpuconvert": count(gpu_log, r" path=gpu-cpuconvert"),
                                       "cpu": count(gpu_log, r" path=cpu"), "fallback": count(gpu_log, r"cpu-fallback")},
               "cpu_session_renders": {"cpu": count(cpu_log, r" path=cpu"), "gpu": count(cpu_log, r" path=gpu-")}}
    # Informational: the sources themselves (they should match: Resolve rendered the same pictures), and
    # the int8 / float16 Fusion cases, where the GPU session's plug-in gets float images (Fusion converts
    # for GPU plug-ins) and so writes a half-float result where the CPU session's writes 8 / 16 bits.
    for r in results:
        r["checked"] = not any(r["image"].startswith(p) for p in ("gpupath-src", "gpupath-int8", "gpupath-float16"))
    problems = [r["image"] for r in results if r["checked"] and (r.get("missing") or r["max_diff_of_255"] > 1.0 + 1e-6 or
                r["values_differing"] > 0.002 * r["values"])]
    problems += [f"source {r['image']} differs between the sessions" for r in results
                 if r["image"].startswith("gpupath-src") and not r.get("missing") and r["max_diff_of_255"] > 0]
    summary["sessions"] = [first, second]
    if second.startswith("gpu") and summary["gpu_session_renders"]["gpu-direct"] + summary["gpu_session_renders"]["gpu-download"] == 0:
        problems.append("the GPU session never took the GPU path")
    if second.startswith("gpu") and (summary["gpu_session_renders"]["cpu"] or summary["gpu_session_renders"]["fallback"]):
        problems.append("renders of the GPU session got CPU images or fell back to the CPU path")
    if first.startswith("cpu") and summary["cpu_session_renders"]["gpu"]:
        problems.append("the CPU session took the GPU path")
    doc = {"summary": summary, "problems": problems, "results": results}
    (base / "gpu-ab.json").write_text(json.dumps(doc, indent=2), encoding="utf-8")
    print("GPU_AB " + json.dumps(summary))
    for r in results:
        print("GPU_AB_IMAGE " + json.dumps(r))
    print(f"{'FAIL' if problems else 'PASS'} gpu-ab: {', '.join(problems) if problems else 'GPU path matches the CPU path'}"
          f" -> {base}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
