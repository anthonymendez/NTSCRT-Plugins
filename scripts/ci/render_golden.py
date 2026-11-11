#!/usr/bin/env python3
"""Render golden reference PNGs with UPSTREAM NTSCRT's crt-smoke.

For every entry of tests/golden/manifest.json this runs

  crt-smoke <TestAssets/input> <slang-shaders/shader> <out/id.png> <librashader.dylib>
            <inW> <inH> [<downW> <downH> <method>] [--ntsc default|settings.json]
            [--ntsc-dylib ntscrs_capi.dylib] --frame N [--set NAME=VALUE ...]

(output size = input size, matching ntscrt's contract) and writes a sidecar
out/<id>.json recording the shader parameters crt-smoke overrode. crt-smoke
always sets the preset's FIRST runtime parameter to (min+max)/2 ("set/get
NAME: wrote V") before applying --set overrides; test_golden replays those
values through look.shader.params so both renderers see the same settings.

Usage: render_golden.py --manifest M --upstream DIR --smoke BIN --out DIR
"""
import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    return struct.unpack(">II", head[16:24])


def downscale_height(width, src_w, src_h):
    # upstream AppState.downscaleHeight
    aspect = max(0.05, src_w / src_h)
    return max(16, 2 * int(round_half_away(width / aspect / 2.0)))


def round_half_away(x):
    # Swift .rounded() = schoolbook rounding (half away from zero), not banker's
    return int(x + 0.5) if x >= 0 else -int(-x + 0.5)


# upstream AppState.appShaderDefaults: the app (and ntscrt_core, core/stages/crt/crt.cpp
# house_defaults) starts these presets from these values, not the .slangp initials.
# crt-smoke doesn't, so they're passed as --set (entry shader_params still win).
APP_SHADER_DEFAULTS = {
    "crt/crtglow_gauss.slangp": {"BOOST": 1.1, "GLOW_ROLLOFF": 2.4, "BLOOM_STRENGTH": 0.1},
    "crt/crtglow_lanczos.slangp": {"BOOST": 1.1, "GLOW_ROLLOFF": 2.4, "BLOOM_STRENGTH": 0.1},
}

SET_GET = re.compile(r"^set/get (\S+): wrote (\S+), read back (\S+)")
OVERRIDE = re.compile(r"^override (\S+) = (\S+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--upstream", required=True, help="upstream NTSCRT checkout (built)")
    ap.add_argument("--smoke", required=True, help="crt-smoke binary")
    ap.add_argument("--out", required=True)
    ap.add_argument("--presets", help="look presets dir (default <upstream>/presets)")
    ap.add_argument("--only", help="render just this entry id")
    a = ap.parse_args()

    up = os.path.abspath(a.upstream)
    presets = os.path.abspath(a.presets or os.path.join(up, "presets"))
    librashader = os.path.join(up, "Vendor", "librashader", "librashader.dylib")
    ntscrs = os.path.join(up, "Vendor", "ntscrs-capi", "ntscrs_capi.dylib")
    shaders = os.path.join(up, "Vendor", "slang-shaders")
    for p in (a.smoke, librashader, shaders):
        if not os.path.exists(p):
            sys.exit(f"error: missing {p}")

    with open(a.manifest, encoding="utf-8") as f:
        manifest = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    tmp = tempfile.mkdtemp(prefix="golden-ntsc-")
    failures = []
    index = []

    for e in manifest["entries"]:
        eid = e["id"]
        if a.only and eid != a.only:
            continue
        src = os.path.join(up, "TestAssets", e["input"])
        w, h = png_size(src)
        out_png = os.path.join(a.out, eid + ".png")
        cmd = [a.smoke, src, os.path.join(shaders, e["shader"]), out_png, librashader, str(w), str(h)]
        down_h = None
        if e.get("downscale"):
            d = e["downscale"]
            down_h = downscale_height(d["width"], w, h)
            cmd += [str(d["width"]), str(down_h), d.get("method", "area")]
        ntsc = e.get("ntsc")
        if ntsc is not None:
            if not os.path.exists(ntscrs):
                sys.exit(f"error: {eid} needs ntsc-rs but {ntscrs} is missing (run scripts/build-ntscrs.sh)")
            if ntsc == "default":
                cmd += ["--ntsc", "default"]
            else:
                with open(os.path.join(presets, ntsc["preset"] + ".json"), encoding="utf-8") as f:
                    settings = json.load(f)["ntsc"]["settings"]
                sp = os.path.join(tmp, eid + ".ntsc.json")
                with open(sp, "w", encoding="utf-8") as f:
                    json.dump(settings, f)
                cmd += ["--ntsc", sp]
            cmd += ["--ntsc-dylib", ntscrs]
        cmd += ["--frame", str(e.get("frame", 1))]
        params = dict(APP_SHADER_DEFAULTS.get(e["shader"], {}))
        params.update(e.get("shader_params") or {})
        for k, v in params.items():
            cmd += ["--set", f"{k}={v}"]

        print("::group::" + eid)
        print(" ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        print("::endgroup::")
        if r.returncode != 0 or not os.path.exists(out_png):
            failures.append(f"{eid}: crt-smoke exit {r.returncode}")
            continue
        ow, oh = png_size(out_png)
        if (ow, oh) != (w, h):
            failures.append(f"{eid}: output {ow}x{oh} != input {w}x{h}")

        params = {}
        for line in r.stdout.splitlines():
            m = SET_GET.match(line.strip())
            if m:
                params[m.group(1)] = float(m.group(3))  # value the chain actually holds
            m = OVERRIDE.match(line.strip())
            if m:
                params[m.group(1)] = float(m.group(2))
        sidecar = {
            "id": eid,
            "shader_params": params,
            "width": w,
            "height": h,
            "downscale_height": down_h,
            "smoke_args": [os.path.relpath(c, up) if os.path.isabs(c) else c for c in cmd[1:]],
            "upstream_commit": manifest.get("upstream_commit"),
            "librashader_commit": manifest.get("librashader_commit"),
        }
        with open(os.path.join(a.out, eid + ".json"), "w", encoding="utf-8") as f:
            json.dump(sidecar, f, indent=2, sort_keys=True)
            f.write("\n")
        index.append(eid)

    with open(os.path.join(a.out, "INDEX.txt"), "w", encoding="utf-8") as f:
        f.write("Rendered by upstream NTSCRT crt-smoke; copy *.png and *.json into tests/golden/ref/\n")
        f.writelines(i + "\n" for i in index)
    if failures:
        print("\n".join("::error::" + x for x in failures))
        sys.exit(1)
    print(f"rendered {len(index)} references into {a.out}")


if __name__ == "__main__":
    main()
