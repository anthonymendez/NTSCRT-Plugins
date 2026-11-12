#!/usr/bin/env python3
"""Check that the port is consistent with the upstream NTSCRT commit it's pinned at (CI job sync-check).

  python scripts/check_sync.py              # all checks
  python scripts/check_sync.py --no-schema  # skip regenerating the schema (the slow, slang-shaders one)

Fails (exit 1) when:
  a. regenerating the schema (tools/gen_schema.py --slang-shaders vendor/slang-shaders) differs
     from core/params/schema.json;
  b. a file in presets/ differs from upstream's blob at the pin, upstream has a preset we lack, or we
     have one upstream doesn't (unless port-map.json lists it in retained_presets);
  c. schema.json's sources.upstream_ntscrt, tests/golden/manifest.json's or a tests/golden/ref/*.json
     upstream_commit isn't the submodule commit (also: the submodule checkout must be the recorded gitlink);
  d. the slang-shaders commit differs between the vendor/slang-shaders gitlink, SLANG_COMMIT in
     tools/gen_schema.py, schema.json, core/stages/crt/CMakeLists.txt and upstream's own
     Vendor/slang-shaders pin (likewise ntsc-rs: vendor/ntsc-rs vs upstream's Vendor/ntsc-rs);
  e. the librashader commit differs between the vendor/librashader gitlink, LIBRASHADER_COMMIT in
     .github/workflows/golden.yml and core/stages/crt/CMakeLists.txt;
  f. docs/dev/port-map.json leaves an upstream file (under its roots, at the pin) unmapped, is
     malformed, names an "ours" path that doesn't exist, or disagrees with the files gen_schema.py
     reads about which are "generated-input".

Gitlinks are read from the index (= HEAD in CI), so un-initialised vendor submodules are fine for
d / e. a, b, f and upstream's pins need upstream/NTSCRT checked out; a needs vendor/slang-shaders
(else gen_schema.py fetches it). A Dependabot submodule bump fails here until the sync is done:
see docs/dev/UPSTREAM-SYNC.md. Stdlib only, Python 3.9+.
"""
import argparse
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

import upstream_lib as U

ROOT = U.ROOT
CRT_CMAKE = ROOT / "core" / "stages" / "crt" / "CMakeLists.txt"
GOLDEN_YML = ROOT / ".github" / "workflows" / "golden.yml"
GOLDEN_REF = ROOT / "tests" / "golden" / "ref"


class Report:
    def __init__(self):
        self.failed = []

    def check(self, name, problems, ok_msg=""):
        if problems:
            self.failed.append(name)
            print("FAIL  %s" % name)
            for p in problems:
                print("      - %s" % p)
        else:
            print("ok    %s%s" % (name, (": " + ok_msg) if ok_msg else ""))

    def skip(self, name, why):
        print("skip  %s: %s" % (name, why))


def short(s):
    return s[:8] if s else "-"


def check_schema(commit, rep):
    name = "a. schema.json regenerates identically"
    slang = ROOT / "vendor" / "slang-shaders"
    with tempfile.TemporaryDirectory(prefix="ntscrt-sync-") as tmp:
        out = Path(tmp) / "schema.json"
        cmd = [sys.executable, str(U.GEN_SCHEMA), "--out", str(out)]
        if (slang / "crt").is_dir():
            cmd += ["--slang-shaders", str(slang)]
        else:
            print("      (vendor/slang-shaders not checked out: gen_schema.py fetches it)")
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            rep.check(name, ["tools/gen_schema.py failed:\n        " + r.stderr.strip().replace("\n", "\n        ")])
            return
        new = out.read_bytes()
    cur = U.SCHEMA.read_bytes()
    if new == cur:
        rep.check(name, [], "%d bytes" % len(cur))
        return
    probs = ["regenerated schema differs from core/params/schema.json: run "
             "`python tools/gen_schema.py --slang-shaders vendor/slang-shaders` (or scripts/sync_upstream.py) and "
             "commit the result"]
    try:
        a, b = json.loads(cur), json.loads(new)
        if a.get("sources") != b.get("sources"):
            probs.append("sources: committed %s, regenerated %s" % (a.get("sources"), b.get("sources")))
        ida = {p["id"]: p for p in a.get("params", [])}
        idb = {p["id"]: p for p in b.get("params", [])}
        added = sorted(set(idb) - set(ida))
        gone = sorted(set(ida) - set(idb))
        changed = sorted(k for k in set(ida) & set(idb) if ida[k] != idb[k])
        for label, ids in (("params only in the regenerated schema", added),
                           ("params only in the committed schema", gone), ("params that differ", changed)):
            if ids:
                probs.append("%s (%d): %s%s" % (label, len(ids), ", ".join(ids[:12]), " ..." if len(ids) > 12 else ""))
        for k in sorted((set(a) | set(b)) - {"params", "sources"}):
            if a.get(k) != b.get(k):
                probs.append("top-level %r differs" % k)
    except ValueError:
        pass
    rep.check(name, probs)


def check_presets(commit, pm, rep):
    name = "b. presets/ = upstream presets/ (byte for byte)"
    up = {p: sha for p, (mode, sha) in U.tree_files(commit, "presets").items() if mode != "160000"}
    ours = {}
    for f in sorted(U.PRESETS.rglob("*")):
        if f.is_file():
            ours[f.relative_to(ROOT).as_posix()] = U.blob_sha(f.read_bytes())
    probs = []
    for p in sorted(up):
        if p not in ours:
            probs.append("missing here: %s (upstream has it; run scripts/sync_upstream.py)" % p)
        elif ours[p] != up[p]:
            probs.append("differs from upstream's blob: %s (ours %s, upstream %s): restore it with "
                         "`git -C upstream/NTSCRT show %s:\"%s\"`, copied as bytes" % (p, short(ours[p]), short(up[p]),
                                                                                       commit[:8], p))
    for p in sorted(set(ours) - set(up)):
        if p not in pm.retained_presets:
            probs.append("not in upstream at the pin: %s (upstream deleted it, or it's new here): delete it, or keep "
                         "it on purpose by listing it in port-map.json retained_presets" % p)
    rep.check(name, probs, "%d files" % len(up))


def check_pins(commit, sub_head, rep):
    name = "c. upstream pin recorded consistently"
    probs = []
    if sub_head and sub_head != commit:
        probs.append("upstream/NTSCRT is checked out at %s but recorded at %s: `git add upstream/NTSCRT` after a sync, "
                     "or `git submodule update upstream/NTSCRT`" % (short(sub_head), short(commit)))
    try:
        pin = U.schema_pin()
        if pin != commit:
            probs.append("core/params/schema.json sources.upstream_ntscrt is %s, the submodule is %s: regenerate the "
                         "schema (tools/gen_schema.py)" % (short(pin), short(commit)))
    except U.Fail as e:
        probs.append(str(e))
    manifest = GOLDEN_REF.parent / "manifest.json"
    try:
        mc = json.loads(manifest.read_text(encoding="utf-8")).get("upstream_commit")
        if mc != commit:
            probs.append("tests/golden/manifest.json upstream_commit is %s, the submodule is %s: set it to the pin "
                         "(scripts/sync_upstream.py does)" % (short(mc), short(commit)))
    except (OSError, ValueError) as e:
        probs.append("tests/golden/manifest.json: %s" % e)
    refs = sorted(GOLDEN_REF.glob("*.json"))
    stale = []
    for f in refs:
        try:
            uc = json.loads(f.read_text(encoding="utf-8")).get("upstream_commit")
        except ValueError:
            probs.append("%s: not JSON" % f.relative_to(ROOT).as_posix())
            continue
        if uc != commit:
            stale.append("%s (%s)" % (f.name, short(uc)))
    if stale:
        probs.append("%d golden reference(s) rendered from another upstream commit than %s: refresh them "
                     "(`gh workflow run golden --ref <branch>`, tests/golden/README.md): %s"
                     % (len(stale), short(commit), ", ".join(stale)))
    rep.check(name, probs, "%s; schema.json and %d golden refs agree" % (short(commit), len(refs)))


def _cmake_pin(label):
    text = CRT_CMAKE.read_text(encoding="utf-8")
    m = re.search(r'"%s[^"]*pinned ([0-9a-f]{7,40})\)' % re.escape(label), text)
    return m.group(1) if m else None


def _same(full, cand):
    return bool(cand) and bool(full) and full.startswith(cand)


def check_slang(commit, have_up, rep):
    name = "d. slang-shaders / ntsc-rs pins agree"
    probs = []
    link = U.gitlink("vendor/slang-shaders")
    gs = re.search(r'^SLANG_COMMIT\s*=\s*"([0-9a-f]{40})"', U.GEN_SCHEMA.read_text(encoding="utf-8"), re.M)
    gs = gs.group(1) if gs else None
    sch = json.loads(U.SCHEMA.read_text(encoding="utf-8")).get("sources", {}).get("slang_shaders")
    cm = _cmake_pin("libretro slang-shaders")
    found = [("vendor/slang-shaders gitlink", link), ("SLANG_COMMIT in tools/gen_schema.py", gs),
             ("schema.json sources.slang_shaders", sch), ("core/stages/crt/CMakeLists.txt (pinned ...)", cm)]
    if have_up:
        found.append(("upstream's Vendor/slang-shaders at the pin", U.upstream_vendor_pin(commit, "Vendor/slang-shaders")))
    for label, v in found[1:]:
        if not _same(link, v):
            probs.append("slang-shaders: %s is %s, the vendor/slang-shaders gitlink is %s" % (label, v or "missing",
                                                                                           short(link)))
    nlink = U.gitlink("vendor/ntsc-rs")
    if have_up:
        un = U.upstream_vendor_pin(commit, "Vendor/ntsc-rs")
        if un != nlink:
            probs.append("ntsc-rs: vendor/ntsc-rs gitlink is %s, upstream pins Vendor/ntsc-rs at %s: bump ours "
                         "(rendering + golden refs depend on it)" % (short(nlink), short(un)))
    rep.check(name, probs, "slang-shaders %s, ntsc-rs %s" % (short(link), short(nlink)))


def check_librashader(rep):
    name = "e. librashader pins agree"
    probs = []
    link = U.gitlink("vendor/librashader")
    m = re.search(r"^\s*LIBRASHADER_COMMIT:\s*([0-9a-f]{7,40})\s*$", GOLDEN_YML.read_text(encoding="utf-8"), re.M)
    gy = m.group(1) if m else None
    if gy != link:
        probs.append("LIBRASHADER_COMMIT in .github/workflows/golden.yml is %s, the vendor/librashader gitlink is %s "
                     "(golden.yml needs the full SHA)" % (gy or "missing", link))
    cm = _cmake_pin("librashader source")
    if not _same(link, cm):
        probs.append("core/stages/crt/CMakeLists.txt says librashader is pinned %s, the gitlink is %s"
                     % (cm or "(missing)", short(link)))
    rep.check(name, probs, short(link))


def gen_schema_inputs():
    """Upstream files tools/gen_schema.py reads (`read(up / "...")`)."""
    text = U.GEN_SCHEMA.read_text(encoding="utf-8")
    return set(re.findall(r'\bup\s*/\s*"([^"]+/[^"/]+\.[A-Za-z]+)"', text))


def check_map(commit, pm, rep):
    name = "f. port-map.json covers upstream at the pin"
    probs = list(pm.problems())
    files = U.tree_files(commit, *pm.roots)
    unmapped = sorted(p for p in files if pm.match(p) is None)
    for p in unmapped:
        probs.append("unmapped upstream file: %s (add an entry to docs/dev/port-map.json)" % p)
    for e in pm.entries:
        for o in e["ours"]:
            if not (ROOT / o.rstrip("/")).exists():
                probs.append("%s: 'ours' path %s doesn't exist" % (e["upstream"], o))
    reads = gen_schema_inputs()
    gen = {e["upstream"] for e in pm.entries if "generated-input" in e["kind"] and not U.is_glob(e["upstream"])}
    for p in sorted(reads - gen):
        probs.append("tools/gen_schema.py reads %s but port-map.json doesn't mark it generated-input" % p)
    for p in sorted(gen - reads):
        probs.append("port-map.json marks %s generated-input but tools/gen_schema.py doesn't read it" % p)
    stale = sorted(u for u in pm.exact if pm.in_roots(u) and u not in files)
    for u in stale:
        print("      note: port-map entry %s matches no upstream file at the pin (kept for older ranges?)" % u)
    rep.check(name, probs, "%d upstream files, %d entries" % (len(files), len(pm.entries)))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--no-schema", action="store_true", help="skip check a (regenerating the schema)")
    a = ap.parse_args(argv)
    U.utf8_stdio()
    rep = Report()
    try:
        pm = U.PortMap()
        commit = U.gitlink(U.UPSTREAM_REL)
        have_up = U.upstream_initialised()
        sub_head = U.upstream_git("rev-parse", "HEAD").strip() if have_up else None
        print("upstream/NTSCRT recorded at %s%s" % (commit[:12], (" (%s)" % U.describe(commit)) if have_up else ""))
        if have_up and not U.git_ok("cat-file", "-e", commit + "^{commit}", cwd=U.UPSTREAM):
            raise U.Fail("upstream/NTSCRT lacks the recorded commit %s: git submodule update upstream/NTSCRT" % commit)
        if not have_up:
            for n in ("a. schema", "b. presets", "f. port map"):
                rep.skip(n, "upstream/NTSCRT not checked out (git submodule update --init upstream/NTSCRT)")
            rep.failed.append("upstream/NTSCRT not checked out")
        if have_up and not a.no_schema:
            if sub_head != commit:
                rep.check("a. schema.json regenerates identically",
                          ["upstream/NTSCRT is checked out at %s, not the recorded %s: can't regenerate"
                           % (short(sub_head), short(commit))])
            else:
                check_schema(commit, rep)
        elif a.no_schema:
            rep.skip("a. schema", "--no-schema")
        if have_up:
            check_presets(commit, pm, rep)
        check_pins(commit, sub_head, rep)
        check_slang(commit, have_up, rep)
        check_librashader(rep)
        if have_up:
            check_map(commit, pm, rep)
    except U.Fail as e:
        print("check_sync: error: %s" % e, file=sys.stderr)
        return 2
    if rep.failed:
        print("\ncheck_sync: %d check(s) failed. The sync routine: docs/dev/UPSTREAM-SYNC.md "
              "(scripts/sync_upstream.py does the mechanical part)." % len(rep.failed))
        return 1
    print("\ncheck_sync: all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
