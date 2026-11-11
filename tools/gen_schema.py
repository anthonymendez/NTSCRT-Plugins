#!/usr/bin/env python3
"""Generate core/params/schema.json (format: docs/params.md) from upstream NTSCRT.

Sources (all read, nothing hand-copied except where noted):
  * upstream/NTSCRT Swift sources (pinned submodule):
      Sources/CrtApp/AppState.swift          stage on/off + downscale + crop defaults,
                                             appShaderDefaults
      Sources/CrtApp/Views/DownscalePanel.swift  width presets, sampling labels
      Sources/CrtCore/Downscaler.swift       DownscaleMethod cases
      Sources/CrtCore/SourceCrop.swift       crop ratios, maxScale
      Sources/CrtCore/GlitchParams.swift     GlitchParam.all
      Sources/CrtCodec/DigitalVideoParams.swift  DigitalParam.all, structureIDs, DigitalFormat.all
      Sources/CrtCore/Howlaround.swift       HowlaroundParam.all (Screen Loop)
      Sources/CrtCore/Presets.swift          the 7 CRT presets
      Sources/CrtApp/NtscSettingsModel.swift house relabels / range overrides (mirrored below)
  * core/params/ntsc_descriptors.json: ntsc-rs settings descriptors + the NTSC
    stage's house defaults, dumped by the `ntsc-dump` tool (tests/ntsc). Pass
    --ntsc-dump-exe to refresh it first.
  * libretro/slang-shaders at the commit upstream pins (cb01f2f2): `#pragma
    parameter` lines of every pass of each preset, following #include. Pass
    --slang-shaders DIR for an existing checkout; otherwise a blob-less
    partial clone is made in a temp dir and only the needed files fetched.

Python 3.8+, stdlib only. Re-run after bumping upstream or ntsc-rs:
    python tools/gen_schema.py [--ntsc-dump-exe C:/ntb/A/tests/ntsc/Release/ntsc-dump.exe]
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SLANG_URL = "https://github.com/libretro/slang-shaders"
SLANG_COMMIT = "cb01f2f238700eed7ec1c859be1737a9fbcdcb7c"


def die(msg: str) -> None:
    print("gen_schema: error: " + msg, file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------------------
# A tiny Swift-literal parser: enough for the param tables upstream declares
# (calls with labelled args, enum cases, arrays, dictionaries, strings, numbers).
# ---------------------------------------------------------------------------

class Sym(str):
    """An identifier / dotted member like `.reception` or `Ratio`."""


class Call:
    def __init__(self, name, args):
        self.name = name          # Sym or None
        self.args = args          # list of (label|None, value)

    def kw(self):
        return {k: v for k, v in self.args if k is not None}

    def pos(self):
        return [v for k, v in self.args if k is None]

    def __repr__(self):
        return f"Call({self.name!r}, {self.args!r})"


class SwiftParser:
    def __init__(self, text, i=0):
        self.s = text
        self.i = i

    def ws(self):
        s = self.s
        while self.i < len(s):
            c = s[self.i]
            if c in " \t\r\n":
                self.i += 1
            elif s.startswith("//", self.i):
                j = s.find("\n", self.i)
                self.i = len(s) if j < 0 else j + 1
            elif s.startswith("/*", self.i):
                j = s.find("*/", self.i)
                self.i = len(s) if j < 0 else j + 2
            else:
                break

    def peek(self):
        self.ws()
        return self.s[self.i] if self.i < len(self.s) else ""

    def expect(self, ch):
        self.ws()
        if not self.s.startswith(ch, self.i):
            die(f"swift parse: expected {ch!r} at {self.s[self.i:self.i + 60]!r}")
        self.i += len(ch)

    def string(self):
        self.expect('"')
        out = []
        s = self.s
        while True:
            c = s[self.i]
            if c == '"':
                self.i += 1
                break
            if c == "\\":
                n = s[self.i + 1]
                if n == "u" and s[self.i + 2] == "{":
                    j = s.index("}", self.i)
                    out.append(chr(int(s[self.i + 3:j], 16)))
                    self.i = j + 1
                    continue
                if n == "(":
                    die("swift parse: string interpolation not supported")
                out.append({"n": "\n", "t": "\t", "r": "\r", "0": "\0"}.get(n, n))
                self.i += 2
                continue
            out.append(c)
            self.i += 1
        return "".join(out)

    def value(self):
        c = self.peek()
        s = self.s
        if c == '"':
            return self.string()
        if c == "[":
            self.i += 1
            items, d, is_dict = [], {}, False
            while self.peek() != "]":
                if self.peek() == ":":           # empty dict literal [:]
                    self.i += 1
                    is_dict = True
                    continue
                k = self.value()
                if self.peek() == ":":
                    self.i += 1
                    is_dict = True
                    d[k] = self.value()
                else:
                    items.append(k)
                if self.peek() == ",":
                    self.i += 1
            self.i += 1
            return d if is_dict else items
        if c == "(":                            # tuple
            self.i += 1
            args = self.arglist(")")
            return Call(None, args)
        m = re.compile(r"-?(?:\d+\.\d+|\d+)(?:[eE][-+]?\d+)?").match(s, self.i)
        if m:
            self.i = m.end()
            t = m.group(0).replace("_", "")
            return float(t) if any(ch in t for ch in ".eE") else int(t)
        m = re.compile(r"\.?[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*").match(s, self.i)
        if not m:
            die(f"swift parse: unexpected {s[self.i:self.i + 60]!r}")
        self.i = m.end()
        name = Sym(m.group(0))
        if name in ("true", "false"):
            return name == "true"
        if name == "nil":
            return None
        if self.i < len(s) and s[self.i] == "(":
            self.i += 1
            return Call(name, self.arglist(")"))
        return name

    def arglist(self, close):
        args = []
        while self.peek() != close:
            label = None
            m = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)\s*:(?!:)").match(self.s, self.i)
            if m:
                label = m.group(1)
                self.i = m.end()
            args.append((label, self.value()))
            if self.peek() == ",":
                self.i += 1
        self.i += 1
        return args


def swift_array_after(text: str, marker: str):
    """Parse the array literal that follows `marker ... = [`."""
    i = text.find(marker)
    if i < 0:
        die(f"marker not found: {marker!r}")
    j = text.index("=", i)
    p = SwiftParser(text, j + 1)
    return p.value()


def read(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as e:
        die(f"cannot read {path}: {e}")


def var_default(text: str, name: str):
    m = re.search(r"\bvar\s+" + re.escape(name) + r"\s*:\s*[^=]+=\s*(.+)", text)
    if not m:
        die(f"default of {name} not found")
    v = m.group(1).strip()
    v = re.split(r"\s*\{\s*didSet|\s*//", v)[0].strip()
    return SwiftParser(v).value()


# ---------------------------------------------------------------------------
# helpers for schema entries
# ---------------------------------------------------------------------------

def num(v):
    """JSON-friendly number: ints stay ints, floats keep full precision."""
    if isinstance(v, bool):
        return v
    if isinstance(v, float) and v.is_integer() and abs(v) < 1e15:
        return int(v)
    return v


class Schema:
    def __init__(self):
        self.groups = []
        self.params = []
        self._gids = set()
        self._ids = set()
        self._host_names = {}

    def group(self, gid, label, parent=None, **extra):
        if gid in self._gids:
            die(f"duplicate group {gid}")
        self._gids.add(gid)
        g = {"id": gid, "label": label, "parent": parent}
        g.update(extra)
        self.groups.append(g)
        return gid

    def param(self, **p):
        if p["id"] in self._ids:
            die(f"duplicate param id {p['id']}")
        # Hosts derive param names by mapping non-alphanumerics to "_".
        host = re.sub(r"[^A-Za-z0-9]", "_", p["id"])
        if host in self._host_names:
            die(f"param id {p['id']} collides with {self._host_names[host]} as host name {host}")
        self._host_names[host] = p["id"]
        if p["group"] not in self._gids:
            die(f"param {p['id']} in unknown group {p['group']}")
        self._ids.add(p["id"])
        order = ["id", "group", "label", "hint", "type", "default", "min", "max", "display_min",
                 "display_max", "step", "unit", "percent", "scale", "encode", "options", "path",
                 "animatable", "enabled_if", "visible_if", "preset"]
        out = {k: p[k] for k in order if k in p and p[k] is not None}
        for k in p:
            if k not in out and p[k] is not None:
                out[k] = p[k]
        self.params.append(out)


# ---------------------------------------------------------------------------
# sections
# ---------------------------------------------------------------------------

def gen_downscale(S: Schema, up: Path):
    app = read(up / "Sources/CrtApp/AppState.swift")
    panel = read(up / "Sources/CrtApp/Views/DownscalePanel.swift")
    dsw = read(up / "Sources/CrtCore/Downscaler.swift")

    presets = swift_array_after(panel, "static let presets")
    sampling = swift_array_after(panel, "static let sampling")
    sampling_labels = {}
    for t in sampling:
        label, method = t.pos()[0], t.pos()[1]
        sampling_labels[method.lstrip(".")] = label

    # DownscaleMethod: case names and raw values, display names.
    body = dsw[dsw.index("enum DownscaleMethod"):]
    body = body[:body.index("public var displayName")]
    methods = []  # (case, raw)
    for line in body.splitlines():
        line = line.split("//")[0].strip()
        if not line.startswith("case "):
            continue
        for part in line[5:].split(","):
            part = part.strip()
            if "=" in part:
                case, raw = [x.strip() for x in part.split("=", 1)]
                raw = raw.strip('"')
            else:
                case = raw = part
            methods.append((case, raw))
    # displayName: .nearestAA -> "Nearest+", else rawValue.capitalized
    def display(case, raw):
        if case == "nearestAA":
            return "Nearest+"
        return raw[:1].upper() + raw[1:]

    S.group("downscale", "Downscale")
    S.param(id="downscale.enabled", group="downscale", label="Downscale before shader",
            hint="Reduce the picture to a retro horizontal resolution before the shader; the height follows the source's aspect ratio.",
            type="bool", default=var_default(app, "downscaleEnabled"), path="/downscale/enabled", animatable=False)
    opts = []
    for t in presets:
        label, width = t.pos()
        opts.append({"value": label, "label": label, "applies": {"/downscale/width": width}})
    opts.append({"value": "Custom", "label": "Custom"})
    S.param(id="downscale.preset", group="downscale", label="Horizontal resolution",
            hint="Console preset: sets the width. Purely a label otherwise (\"Custom\" when the width is edited).",
            type="choice", default=var_default(app, "downscalePreset"), options=opts,
            path="/downscale/preset", animatable=False, enabled_if="/downscale/enabled")
    S.param(id="downscale.width", group="downscale", label="Width",
            hint="Horizontal resolution in pixels. Height follows the source's aspect ratio.",
            type="int", default=var_default(app, "downscaleWidth"), min=16, max=4096, step=16, unit="px",
            path="/downscale/width", animatable=False, enabled_if="/downscale/enabled")
    # Upstream enum order (choice option order is part of hosts' saved-project
    # format: append only). The two the upstream UI offers carry its labels.
    m_opts = []
    for case, raw in methods:
        if raw in sampling_labels:
            m_opts.append({"value": raw, "label": sampling_labels[raw]})
        else:
            m_opts.append({"value": raw, "label": display(case, raw), "legacy": True})
    S.param(id="downscale.method", group="downscale", label="Sampling",
            hint="Chunky takes one source pixel per block — hard pixel edges (on detailed video they can shimmer). Smooth averages the whole block — softer, steady.",
            type="choice", default=str(var_default(app, "downscaleMethod")).lstrip("."), options=m_opts,
            path="/downscale/method", animatable=False, enabled_if="/downscale/enabled")


def gen_crop(S: Schema, up: Path):
    app = read(up / "Sources/CrtApp/AppState.swift")
    sc = read(up / "Sources/CrtCore/SourceCrop.swift")
    portrait = swift_array_after(sc, "public static let portrait")
    landscape = swift_array_after(sc, "public static let landscape")
    max_scale = float(re.search(r"maxScale\s*=\s*([\d.]+)", sc).group(1))
    ratios = [(1, 1)] + [tuple(c.pos()) for c in portrait] + [tuple(c.pos()) for c in landscape]
    default_ratio = var_default(app, "cropRatio")
    assert default_ratio == ".square", default_ratio

    S.group("crop", "Crop")
    S.param(id="crop.enabled", group="crop", label="Crop",
            hint="Crop the picture to an aspect ratio before anything else in the chain sees it.",
            type="bool", default=var_default(app, "cropEnabled"), path="/crop/enabled", animatable=False)
    S.param(id="crop.ratio", group="crop", label="Aspect ratio", type="choice", default="1:1",
            options=[{"value": f"{w}:{h}", "label": f"{w}:{h}"} for w, h in ratios],
            path="/crop/ratio", animatable=False, enabled_if="/crop/enabled")
    S.param(id="crop.x", group="crop", label="Position left – right",
            hint="Where the crop sits wherever there's room: 0 the left edge, 1 the right, 0.5 the middle.",
            type="float", default=num(var_default(app, "cropX")), min=0, max=1, step=0.001,
            path="/crop/x", animatable=True, enabled_if="/crop/enabled")
    S.param(id="crop.y", group="crop", label="Position top – bottom",
            hint="Where the crop sits wherever there's room: 0 the top edge, 1 the bottom, 0.5 the middle.",
            type="float", default=num(var_default(app, "cropY")), min=0, max=1, step=0.001,
            path="/crop/y", animatable=True, enabled_if="/crop/enabled")
    S.param(id="crop.scale", group="crop", label="Zoom",
            hint="1: the largest crop for the ratio; more zooms in (2: half its width and height).",
            type="float", default=num(var_default(app, "cropScale")), min=1, max=num(max_scale), step=0.01,
            unit="×", path="/crop/scale", animatable=True, enabled_if="/crop/enabled")


# House layout of the VHS panel (upstream NtscSettingsModel.houseLayout) —
# presentation only. Mirrored here; the generator checks the upstream file
# still contains these entries.
NTSC_RELABELS = {
    "scale_settings": "Intensity",
    "bandwidth_scale": "Horizontal intensity",
    "vertical_scale": "Vertical intensity",
}
NTSC_DISPLAY_RANGE = {"bandwidth_scale": (0.125, 3.0), "vertical_scale": (0.125, 3.0)}
NTSC_CHROMA_SECTION = ["chroma_phase_error", "chroma_phase_noise_intensity",
                       "chroma_delay_horizontal", "chroma_delay_vertical"]
NTSC_NON_ANIMATABLE = {"random_seed"}  # a seed is structure, not a level


def check_ntsc_house(up: Path):
    t = read(up / "Sources/CrtApp/NtscSettingsModel.swift")
    for k, v in NTSC_RELABELS.items():
        if f'"{k}": "{v}"' not in t:
            die(f"NtscSettingsModel relabel changed for {k}; update NTSC_RELABELS")
    for k, (lo, hi) in NTSC_DISPLAY_RANGE.items():
        if f'"{k}": ({lo}, {hi})' not in t:
            die(f"NtscSettingsModel range override changed for {k}")
    for k in NTSC_CHROMA_SECTION:
        if f'"{k}"' not in t:
            die(f"NtscSettingsModel chroma section changed ({k})")


def gen_ntsc(S: Schema, up: Path, dump: dict):
    app = read(up / "Sources/CrtApp/AppState.swift")
    check_ntsc_house(up)
    descs = dump["descriptors"]
    defaults = dump["defaults"]

    # house order: chroma section replaces its members at the first one's spot,
    # then scale_settings moves to the top.
    top = list(descs)
    names = [d["name"] for d in top]
    if all(n in names for n in NTSC_CHROMA_SECTION):
        anchor = names.index(NTSC_CHROMA_SECTION[0])
        members = [d for d in top if d["name"] in NTSC_CHROMA_SECTION]
        top = [d for d in top if d["name"] not in NTSC_CHROMA_SECTION]
        top.insert(min(anchor, len(top)), {"name": "chroma_distortion", "label": "Chroma distortion",
                                            "description": "Phase and delay errors in the color subcarrier.",
                                            "kind": "section", "children": members})
    idx = next((i for i, d in enumerate(top) if d["name"] == "scale_settings"), None)
    if idx is not None:
        top.insert(0, top.pop(idx))

    S.group("ntsc", "NTSC (TV)")
    S.param(id="ntsc.enabled", group="ntsc", label="NTSC signal",
            hint="ntsc-rs analog signal simulation (composite / VHS).",
            type="bool", default=var_default(app, "ntscEnabled"), path="/ntsc/enabled", animatable=False)

    def emit(d, group, enabled_if):
        name = d["name"]
        label = NTSC_RELABELS.get(name, d["label"])
        kind = d["kind"]
        path = "/ntsc/settings/" + name
        hint = d.get("description")
        if kind == "section":
            gid = S.group(f"ntsc.{name}", label, parent=group, hint=hint)
            for c in d["children"]:
                emit(c, gid, enabled_if)
            return
        if kind == "group":
            gid = S.group(f"ntsc.{name}", label, parent=group, hint=hint)
            S.param(id=f"ntsc.{name}", group=gid, label=label, hint=hint, type="bool",
                    default=defaults[name], path=path, animatable=False, enabled_if=enabled_if)
            for c in d["children"]:
                emit(c, gid, path)
            return
        if name not in defaults:
            die(f"ntsc default missing for {name}")
        p = dict(id=f"ntsc.{name}", group=group, label=label, hint=hint, path=path,
                 enabled_if=enabled_if, default=num(defaults[name]))
        if kind == "boolean":
            p.update(type="bool", animatable=False)
        elif kind == "enum":
            p.update(type="choice", animatable=False,
                     options=[{"value": o["index"], "label": o["label"],
                               **({"hint": o["description"]} if o.get("description") else {})}
                              for o in d["options"]])
        elif kind == "int":
            p.update(type="int", min=d["min"], max=d["max"], step=1,
                     animatable=name not in NTSC_NON_ANIMATABLE)
        elif kind == "percentage":
            p.update(type="float", min=0, max=1, step=0.001, percent=True,
                     scale="log" if d.get("logarithmic") else None, animatable=True)
        elif kind == "float":
            p.update(type="float", min=num(d["min"]), max=num(d["max"]), step=0.001,
                     scale="log" if d.get("logarithmic") else None, animatable=True)
            if name in NTSC_DISPLAY_RANGE:
                p["display_min"], p["display_max"] = NTSC_DISPLAY_RANGE[name]
        else:
            die(f"unknown ntsc descriptor kind {kind} ({name})")
        S.param(**p)

    for d in top:
        emit(d, "ntsc", "/ntsc/enabled")


def slider_fields(kind: Call):
    kw = kind.kw()
    out = {"min": num(kw["min"]), "max": num(kw["max"])}
    if kw.get("percent"):
        out["percent"] = True
    if kw.get("unit"):
        out["unit"] = kw["unit"]
    if kw.get("step") is not None:
        out["step"] = num(kw["step"])
    return out


def gen_glitch(S: Schema, up: Path):
    app = read(up / "Sources/CrtApp/AppState.swift")
    t = read(up / "Sources/CrtCore/GlitchParams.swift")
    group_labels = dict(re.findall(r'case\s+(\w+)\s*=\s*"([^"]+)"', t[t.index("enum Group"):t.index("enum Kind")]))
    items = swift_array_after(t, "public static let all: [GlitchParam]")
    S.group("glitch", "Glitch")
    S.param(id="glitch.enabled", group="glitch", label="Glitch",
            hint="TV receiver and VCR transport simulation. Defaults describe a healthy set: enabling changes nothing until a knob is turned.",
            type="bool", default=var_default(app, "glitchEnabled"), path="/glitch/enabled", animatable=False)
    made = set()
    count = 0
    for c in items:
        kw = c.kw()
        g = kw["group"].lstrip(".")
        gid = f"glitch.{g}"
        if gid not in made:
            S.group(gid, group_labels[g], parent="glitch")
            made.add(gid)
        kind = kw["kind"]
        p = dict(id=f"glitch.{kw['id']}", group=gid, label=kw["label"], hint=kw["help"],
                 path=f"/glitch/values/{kw['id']}", enabled_if="/glitch/enabled")
        if isinstance(kind, Sym) and kind == ".toggle":
            p.update(type="bool", default=kw["defaultValue"] >= 0.5, encode="number", animatable=False)
        else:
            f = slider_fields(kind)
            p.update(type="float", default=num(kw["defaultValue"]), step=0.001 if f["max"] - f["min"] <= 2 else 0.01,
                     animatable=True, **f)
        S.param(**p)
        count += 1
    return count


# Digital video controls of the 0.4.0 plug-ins (upstream before 0.19) that
# upstream replaced or reinterpreted. Saved projects store params by id, so
# these ids stay as hidden params written under /digital/legacy only while
# non-zero (visible_if on their own value: they show up only in old projects
# that use them); core/stages/digital/migrate.h maps them onto today's
# controls. Today's Quality (a new curve) therefore needs a new id.
DIGITAL_RENAMED = {"quality": "digital.quality2"}
DIGITAL_LEGACY = [
    # (after this upstream id, legacy id, label, min, max, hint)
    ("aliasing", "lens_blur", "Lens softness (removed)", 0, 1,
     "From NTSCRT 0.4.0. Lens softness is gone (upstream 0.19); kept so older projects load. No effect."),
    ("ccd_response", "exposure", "Exposure (from 0.4.0)", -2, 2,
     "This instance was made with NTSCRT 0.4.0, whose Exposure became CCD response. While CCD response is 0 "
     "it sets CCD response (the stronger of this and Black level decides). Set it to 0 to drop it."),
    ("ccd_response", "black_level", "Black level (from 0.4.0)", 0, 0.25,
     "This instance was made with NTSCRT 0.4.0, whose Black level became CCD response. While CCD response is 0 "
     "it sets CCD response (the stronger of this and Exposure decides). Set it to 0 to drop it."),
    ("quality", "quality", "Quality (from 0.4.0)", 0, 1,
     "This instance was made with NTSCRT 0.4.0, whose Quality slider ran on JPEG's own curve. While Quality "
     "above is at its default, this value (converted to the same quantizer) is used instead. Set it to 0 to drop it."),
]


def gen_digital(S: Schema, up: Path, schema_extra: dict):
    app = read(up / "Sources/CrtApp/AppState.swift")
    t = read(up / "Sources/CrtCodec/DigitalVideoParams.swift")
    group_cases = re.findall(r'case\s+(\w+)\s*=\s*"([^"]+)"', t[t.index("enum Group"):t.index("enum Kind")])
    group_labels = dict(group_cases)
    group_order = [g for g, _ in group_cases]  # upstream Group.allCases = the panel's order
    items = swift_array_after(t, "public static let all: [DigitalParam]")
    structure = set(swift_array_after(t, "public static let structureIDs"))
    formats = swift_array_after(t, "public static let all: [DigitalFormat]")
    # DigitalParam.unusedBy: controls a codec doesn't have (hidden in the panel).
    unused = {}
    m = re.search(r"unusedBy:[^=]*=\s*\[(.*?)\n\s*\]", t, re.S)
    if m:
        for case, ids in re.findall(r"\.(\w+)\s*:\s*\[([^\]]*)\]", m.group(1)):
            unused[case] = re.findall(r'"([^"]+)"', ids)
    codec_cases = re.findall(r"case\s+(\w+)", t[t.index("enum DigitalCodecKind"):t.index("enum ChromaSubsampling")])
    codec_value = {c: i for i, c in enumerate(codec_cases)}

    S.group("digital", "Digital video")
    S.param(id="digital.enabled", group="digital", label="Digital video",
            hint="A small video codec run for real: capture, 8×8 DCT blocks or Microsoft Video 1's 4×4 color blocks, "
                 "quantization, motion-compensated prediction, data loss.",
            type="bool", default=var_default(app, "digitalEnabled"), path="/digital/enabled", animatable=False)
    by_group = {g: [] for g in group_order}
    for c in items:
        by_group[c.kw()["group"].lstrip(".")].append(c)
    legacy_after = {}
    for after, lid, label, lo, hi, hint in DIGITAL_LEGACY:
        legacy_after.setdefault(after, []).append((lid, label, lo, hi, hint))
    count = 0
    for g in group_order:
        if not by_group[g]:
            continue
        gid = S.group(f"digital.{g}", group_labels[g], parent="digital")
        for c in by_group[g]:
            kw = c.kw()
            kind = kw["kind"]
            pid = kw["id"]
            p = dict(id=DIGITAL_RENAMED.get(pid, f"digital.{pid}"), group=gid, label=kw["label"], hint=kw["help"],
                     path=f"/digital/values/{pid}", enabled_if="/digital/enabled",
                     animatable=pid not in structure)
            if isinstance(kind, Call) and kind.name == ".choice":
                labels = kind.pos()[0]
                p.update(type="choice", default=int(kw["defaultValue"]),
                         options=[{"value": i, "label": l} for i, l in enumerate(labels)], animatable=False)
            else:
                f = slider_fields(kind)
                p.update(type="float", default=num(kw["defaultValue"]),
                         step=0.001 if f["max"] - f["min"] <= 2 else 0.01, **f)
            if pid in structure:
                p["structure"] = True
            # Hidden (and so not written) under a codec that doesn't have it.
            for case, ids in unused.items():
                if pid in ids and len(codec_value) == 2:
                    p["visible_if"] = {"path": "/digital/values/codec", "equals": 1 - codec_value[case]}
            S.param(**p)
            count += 1
            for lid, label, lo, hi, hint in legacy_after.get(pid, []):
                S.param(id=f"digital.{lid}", group=gid, label=label, hint=hint, type="float", default=0,
                        min=lo, max=hi, step=0.001 if hi - lo <= 2 else 0.01, path=f"/digital/legacy/{lid}",
                        animatable=True, enabled_if="/digital/enabled",
                        # Shown (and written) only while non-zero; Lens softness never (no effect).
                        visible_if={"path": f"/digital/legacy/{lid}"} if lid != "lens_blur"
                        else {"path": "/digital/legacy/lens_blur", "equals": "never"},
                        legacy=True)
                count += 1

    # Formats: named starting points (upstream DigitalFormat.all), in upstream
    # 0.20 offered in the toolbar's Preset menu. Applying one switches the
    # stage on, writes defaults+values into /digital/values, enables downscale
    # and sets its width and preset label (upstream AppState.applyDigitalFormat
    # + setDigitalEnabled), and clears the 0.4.0 legacy controls.
    defaults = {c.kw()["id"]: num(c.kw()["defaultValue"]) for c in items}
    fl = []
    for f in formats:
        kw = f.kw()
        values = dict(defaults)
        values.update({k: num(v) for k, v in kw["values"].items()})
        applies = {"/digital/enabled": True, "/digital/values": values}
        applies.update({f"/digital/legacy/{lid}": 0 for _, lid, *_ in DIGITAL_LEGACY})
        applies.update({"/downscale/enabled": True, "/downscale/width": kw["width"],
                        "/downscale/preset": f"{kw['name']} ({kw['width']}px)"})
        fl.append({"id": kw["id"], "name": kw["name"], "width": kw["width"], "hint": kw["blurb"],
                   "applies": applies})
    schema_extra["digital_formats"] = fl
    return count, len(fl)


def gen_alpha(S: Schema):
    """NTSCRT-Plugins addition (no upstream equivalent): how the effect uses the
    clip's transparency. "preset": False = presets never set or reset it."""
    S.group("alpha", "Transparency")
    S.param(id="alpha.mode", group="alpha", label="Transparent areas",
            hint="For titles and clips that don't fill the frame. Clip to element: the effect stays inside "
                 "the element's shape. Let effect spill: ghosting, smear and glow extend past the element "
                 "and show as light over what's underneath (like a Screen blend). No difference on full-frame video.",
            type="choice", default="clip",
            options=[{"value": "clip", "label": "Clip to element"},
                     {"value": "spill", "label": "Let effect spill"}],
            path="/alpha/mode", animatable=False, preset=False)


def gen_screenloop(S: Schema, up: Path):
    t = read(up / "Sources/CrtCore/Howlaround.swift")
    group_labels = dict(re.findall(r'case\s+(\w+)\s*=\s*"([^"]+)"', t[t.index("enum Group"):t.index("enum Kind")]))
    items = swift_array_after(t, "public static let all: [HowlaroundParam]")
    S.group("screenloop", "Screen Loop")
    S.param(id="screenloop.enabled", group="screenloop", label="Screen Loop",
            hint="Video feedback: a camcorder pointed at the TV showing its own picture. Off = the host passes no feedback_json.",
            type="bool", default=False, path="fb:/enabled", animatable=False)
    made = set()
    count = 0
    for c in items:
        kw = c.kw()
        g = kw["group"].lstrip(".")
        gid = f"screenloop.{g}"
        if gid not in made:
            S.group(gid, group_labels[g], parent="screenloop")
            made.add(gid)
        kind = kw["kind"]
        pid = kw["id"]
        p = dict(id=f"screenloop.{pid}", group=gid, label=kw["label"], hint=kw["help"],
                 path=f"fb:/values/{pid}", enabled_if="fb:/enabled")
        if isinstance(kind, Sym) and kind == ".toggle":
            p.update(type="bool", default=kw["defaultValue"] >= 0.5, encode="number", animatable=False)
        elif isinstance(kind, Call) and kind.name == ".position":
            pk = kind.kw()
            p.update(type="float", default=num(kw["defaultValue"]), min=num(pk["min"]), max=num(pk["max"]),
                     step=0.001, animatable=True, position=True)
        else:
            f = slider_fields(kind)
            if f.get("step") == 1:
                p.update(type="int", default=int(kw["defaultValue"]), animatable=False, **f)
            else:
                p.update(type="float", default=num(kw["defaultValue"]),
                         step=0.001 if f["max"] - f["min"] <= 2 else 0.01, animatable=True, **f)
        if "neutralValue" in kw:
            p["neutral"] = num(kw["neutralValue"])
        S.param(**p)
        count += 1
    # Upstream saves the render length of a still with the preset ("seconds",
    # AppState+Howlaround.saveFeedbackPreset; 0.5…60, default howlaroundSeconds).
    app = read(up / "Sources/CrtApp/AppState.swift")
    S.param(id="screenloop.seconds", group="screenloop.movement", label="Length",
            hint="How long the move (drift, push, spin, seamless loop) takes, in seconds. Upstream uses the clip's "
                 "duration for video and this for stills; hosts may set it to the effect's duration.",
            type="float", default=num(var_default(app, "howlaroundSeconds")), min=0.5, max=60, step=0.1, unit="s",
            path="fb:/seconds", animatable=False, enabled_if="fb:/enabled")
    return count


# ---------------------------------------------------------------------------
# CRT shader params from slang-shaders
# ---------------------------------------------------------------------------

class SlangSource:
    def __init__(self, local_dir: Path | None):
        self.dir = local_dir
        self.tmp = None
        self.cache = {}

    def _ensure_clone(self):
        if self.dir is not None or self.tmp is not None:
            return
        self.tmp = Path(tempfile.mkdtemp(prefix="slang-shaders-"))
        print(f"gen_schema: fetching {SLANG_URL} @ {SLANG_COMMIT[:8]} (blob-less) into {self.tmp}", file=sys.stderr)
        subprocess.run(["git", "init", "-q", str(self.tmp)], check=True)
        subprocess.run(["git", "-C", str(self.tmp), "remote", "add", "origin", SLANG_URL], check=True)
        subprocess.run(["git", "-C", str(self.tmp), "-c", "protocol.version=2", "fetch", "-q", "--depth=1",
                        "--filter=blob:none", "origin", SLANG_COMMIT], check=True)

    def read(self, rel: str) -> str:
        rel = os.path.normpath(rel).replace("\\", "/")
        if rel in self.cache:
            return self.cache[rel]
        if self.dir is not None:
            data = (self.dir / rel).read_text(encoding="utf-8", errors="replace")
        else:
            self._ensure_clone()
            r = subprocess.run(["git", "-C", str(self.tmp), "show", f"{SLANG_COMMIT}:{rel}"],
                               capture_output=True, check=False)
            if r.returncode != 0:
                die(f"slang-shaders: cannot read {rel}: {r.stderr.decode(errors='replace').strip()}")
            data = r.stdout.decode("utf-8", errors="replace")
        self.cache[rel] = data
        return data

    def close(self):
        if self.tmp is not None:
            shutil.rmtree(self.tmp, ignore_errors=True)


PRAGMA_RE = re.compile(
    r'^\s*#pragma\s+parameter\s+(\w+)\s+"([^"]*)"\s+([-+.\deE]+)\s+([-+.\deE]+)\s+([-+.\deE]+)(?:\s+([-+.\deE]+))?')
INCLUDE_RE = re.compile(r'^\s*#include\s+"([^"]+)"')


def slang_params(src: SlangSource, rel: str, seen_files=None):
    """Yield (name, desc, initial, min, max, step) in source order, following includes
    (librashader's preprocessor expands includes without evaluating #if)."""
    seen_files = seen_files if seen_files is not None else set()
    if rel in seen_files:
        return
    seen_files.add(rel)
    base = os.path.dirname(rel)
    for line in src.read(rel).splitlines():
        m = INCLUDE_RE.match(line)
        if m:
            yield from slang_params(src, os.path.normpath(os.path.join(base, m.group(1))).replace("\\", "/"),
                                    seen_files)
            continue
        m = PRAGMA_RE.match(line)
        if m:
            step = m.group(6)
            yield (m.group(1), m.group(2), float(m.group(3)), float(m.group(4)), float(m.group(5)),
                   float(step) if step is not None else None)


def parse_slangp(text: str):
    kv = {}
    for line in text.splitlines():
        line = line.split("#")[0].strip()
        if "=" not in line:
            continue
        k, v = [x.strip() for x in line.split("=", 1)]
        kv[k] = v.strip('"')
    return kv


def gen_crt(S: Schema, up: Path, src: SlangSource):
    app = read(up / "Sources/CrtApp/AppState.swift")
    pt = read(up / "Sources/CrtCore/Presets.swift")
    presets = swift_array_after(pt, "public static let all")
    house = swift_array_after(app, "static let appShaderDefaults")
    m = re.search(r'Presets\.all\.first\s*\{\s*\$0\.id\s*==\s*"(\w+)"', app)
    default_preset = m.group(1)

    S.group("crt", "CRT")
    S.param(id="crt.enabled", group="crt", label="CRT shader",
            hint="Run the CRT shader chain (librashader). Off: the picture is scaled to the output without a shader.",
            type="bool", default=var_default(app, "shaderEnabled"), path="/shader/enabled", animatable=False)
    S.param(id="crt.preset", group="crt", label="Shader", type="choice", default=default_preset,
            options=[{"value": p.kw()["id"], "label": p.kw()["displayName"],
                      "slangp": p.kw()["relativePath"]} for p in presets],
            path="/shader/preset", animatable=False, enabled_if="/shader/enabled")
    report = {}
    for p in presets:
        kw = p.kw()
        pid, rel = kw["id"], kw["relativePath"]
        sp = parse_slangp(src.read(rel))
        n = int(sp["shaders"])
        overrides = {k: v for k, v in sp.items() if k == "parameters"}
        if overrides:
            print(f"gen_schema: note: {rel} overrides parameters ({sp['parameters']}); upstream UI uses "
                  "#pragma initial values (librashader runtime params)", file=sys.stderr)
        gid = S.group(f"crt.{pid}", kw["displayName"], parent="crt",
                      visible_if={"path": "/shader/preset", "equals": pid})
        seen = {}
        base = os.path.dirname(rel)
        for i in range(n):
            pass_rel = os.path.normpath(os.path.join(base, sp[f"shader{i}"])).replace("\\", "/")
            for name, desc, init, lo, hi, step in slang_params(src, pass_rel):
                if name in seen:
                    if seen[name] != (init, lo, hi, step):
                        print(f"gen_schema: note: {pid}: {name} redeclared differently in {pass_rel}; "
                              "keeping the first", file=sys.stderr)
                    continue
                seen[name] = (init, lo, hi, step)
                default = house.get(pid, {}).get(name, init)
                integral = step is not None and step >= 1 and float(step).is_integer() and \
                    float(lo).is_integer() and float(hi).is_integer() and float(init).is_integer()
                S.param(id=f"crt.{pid}.{name}", group=gid, label=desc.strip() or name,
                        type="float", default=num(float(default)), min=num(lo), max=num(hi),
                        step=num(step) if step is not None else None,
                        integral=True if integral else None,
                        path=f"/shader/params/{name}", animatable=True, enabled_if="/shader/enabled",
                        visible_if={"path": "/shader/preset", "equals": pid})
        report[pid] = len(seen)
    return report


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--upstream", default=str(ROOT / "upstream/NTSCRT"))
    ap.add_argument("--ntsc-dump", default=str(ROOT / "core/params/ntsc_descriptors.json"),
                    help="ntsc-dump output (descriptors + defaults)")
    ap.add_argument("--ntsc-dump-exe", help="run this ntsc-dump binary first to refresh --ntsc-dump")
    ap.add_argument("--slang-shaders", help="existing slang-shaders checkout at the pinned commit")
    ap.add_argument("--out", default=str(ROOT / "core/params/schema.json"))
    ap.add_argument("--allow-breaking", action="store_true",
                    help="allow removing params / reordering choice options vs the existing schema")
    a = ap.parse_args()

    up = Path(a.upstream)
    if not (up / "Sources").is_dir():
        die(f"{up} has no Sources/: run `git submodule update --init upstream/NTSCRT`")
    if a.ntsc_dump_exe:
        subprocess.run([a.ntsc_dump_exe, a.ntsc_dump], check=True)
    dump = json.loads(Path(a.ntsc_dump).read_text(encoding="utf-8"))

    src = SlangSource(Path(a.slang_shaders) if a.slang_shaders else None)
    try:
        S = Schema()
        extra = {}
        # UI order follows the pipeline: crop, downscale (upstream's shared
        # sidebar section), digital, ntsc, glitch, crt, then Screen Loop.
        gen_crop(S, up)
        gen_downscale(S, up)
        n_digital, n_formats = gen_digital(S, up, extra)
        # A Digital video format names the Downscale preset after itself ("MiniDV
        # (720px)", upstream applyDigitalFormat): offer those labels too, appended
        # after "Custom" (options are append-only), so a look with one keeps it.
        ds = next(p for p in S.params if p["id"] == "downscale.preset")
        for f in extra.get("digital_formats", []):
            label = f["applies"]["/downscale/preset"]
            if all(o["value"] != label for o in ds["options"]):
                ds["options"].append({"value": label, "label": label, "applies": {"/downscale/width": f["width"]},
                                      "hint": f"Set by the Digital video format {f['name']}."})
        gen_ntsc(S, up, dump)
        n_glitch = gen_glitch(S, up)
        crt = gen_crt(S, up, src)
        n_fb = gen_screenloop(S, up)
        gen_alpha(S)  # appended last: keeps existing UI / host param order
    finally:
        src.close()

    upstream_rev = ""
    try:
        upstream_rev = subprocess.run(["git", "-C", str(up), "rev-parse", "HEAD"], capture_output=True,
                                      text=True, check=False).stdout.strip()
    except OSError:
        pass
    doc = {
        "version": 1,
        "generated_by": "tools/gen_schema.py",
        "sources": {"upstream_ntscrt": upstream_rev, "slang_shaders": SLANG_COMMIT,
                    "ntsc_rs": "vendor/ntsc-rs (see .gitmodules / submodule pin)"},
        # Fixed values hosts write alongside the params (not user-editable).
        # /version 2: upstream look version since 0.19 (Digital video's Quality curve).
        "constants": {"/version": 2, "/ntsc/settings/version": 1,
                      "fb:/kind": "ntscrt-video-feedback", "fb:/version": 2},
        "groups": S.groups,
        "params": S.params,
    }
    doc.update(extra)

    # Saved-project compatibility: hosts store choices by index and params by
    # id, so against the previous schema, choice options may only be appended
    # and params never removed (override with --allow-breaking).
    out_path = Path(a.out)
    if out_path.exists() and not a.allow_breaking:
        try:
            old = json.loads(out_path.read_text(encoding="utf-8"))
        except ValueError:
            old = {}
        new_by_id = {p["id"]: p for p in S.params}
        problems = []
        for op in old.get("params", []):
            np_ = new_by_id.get(op["id"])
            if np_ is None:
                if old.get("note", "").startswith("Wave-0 seed"):
                    continue
                problems.append(f"param removed: {op['id']}")
                continue
            if "options" in op:
                ov = [o["value"] for o in op["options"]]
                nv = [o["value"] for o in np_.get("options", [])]
                if nv[:len(ov)] != ov and not old.get("note", "").startswith("Wave-0 seed"):
                    problems.append(f"choice options reordered/removed: {op['id']} {ov} -> {nv}")
        if problems:
            die("breaking schema change (hosts save choices by index):\n  " + "\n  ".join(problems) +
                "\nre-run with --allow-breaking only before the first release")

    text = json.dumps(doc, indent=1, ensure_ascii=False) + "\n"
    Path(a.out).write_text(text, encoding="utf-8", newline="\n")
    n_ntsc = sum(1 for p in S.params if p["id"].startswith("ntsc."))
    print(f"gen_schema: wrote {a.out}: {len(S.params)} params, {len(S.groups)} groups "
          f"(ntsc {n_ntsc}, glitch {n_glitch}, digital {n_digital} + {n_formats} formats, "
          f"screen loop {n_fb}, crt {crt})", file=sys.stderr)


if __name__ == "__main__":
    main()
