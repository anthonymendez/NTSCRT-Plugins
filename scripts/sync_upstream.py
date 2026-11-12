#!/usr/bin/env python3
"""Do the mechanical part of an upstream NTSCRT sync (docs/dev/UPSTREAM-SYNC.md, step 3).

  python scripts/sync_upstream.py --to v0.21.0 --dry-run   # what would change; changes nothing
  python scripts/sync_upstream.py --to v0.21.0             # do it
  python scripts/sync_upstream.py --to v0.21.0 --fetch     # fetch upstream tags first
  python scripts/sync_upstream.py --to v0.21.0 --allow-breaking   # passed to gen_schema.py

Steps: refuse a dirty tree (--allow-dirty to re-run after fixing something); move the upstream/NTSCRT
submodule to --to; copy upstream presets/ byte for byte (adds and updates; upstream deletions are
reported, not deleted); regenerate core/params/schema.json (tools/gen_schema.py: its refusal of a
breaking change stops the sync with its exit code); add FxPlug ids (fxplug/tools/update_param_ids.py);
update the version strings (README "Synced with upstream NTSCRT vX", docs/params.md, the pin lines in
docs/dev/UPSTREAM-SYNC.md and docs/dev/AGENTS.md, tests/golden/manifest.json upstream_commit). Then prints the hand-port list (scripts/upstream_diff.py) and the
manual steps left. Never commits or pushes: review `git status` / `git diff` and commit yourself.
Stdlib only, Python 3.9+.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

import upstream_lib as U
import upstream_diff

ROOT = U.ROOT
SLANG = ROOT / "vendor" / "slang-shaders"
UPDATE_IDS = ROOT / "fxplug" / "tools" / "update_param_ids.py"

# (file, regex with groups (prefix)(version)(suffix?), what it is). The version group is replaced.
VERSION_SITES = [
    ("README.md", r"(Synced with upstream NTSCRT )(v[0-9][\w.\-]*)(\.)", "README status line"),
    ("docs/params.md", r"(synced with upstream\s+NTSCRT )(v[0-9][\w.\-]*)()", "docs/params.md look-format line"),
]
# (file, regex with groups (prefix)(version)(middle)(short sha)(suffix), what it is): version and sha replaced.
PIN_SITES = [
    ("docs/dev/UPSTREAM-SYNC.md", r"(Current pin: \*\*)(v[0-9][\w.\-]*|[0-9a-f]{7,40})(\*\* \(`)([0-9a-f]{7,40})(`\))",
     "pin line in docs/dev/UPSTREAM-SYNC.md"),
    ("docs/dev/AGENTS.md", r"(submodule, upstream )(v[0-9][\w.\-]*|[0-9a-f]{7,40})(, commit )([0-9a-f]{7,40})(;)",
     "pin line in docs/dev/AGENTS.md"),
]
# (file, regex with groups (prefix)(full sha)(suffix), what it is): the full sha replaced.
SHA_SITES = [
    # render_golden.py records the commit it builds; this is the manifest's own note of it
    # (check_sync.py compares it with the pin).
    ("tests/golden/manifest.json", r'("upstream_commit": ")([0-9a-f]{40})(")', "tests/golden/manifest.json upstream_commit"),
]


def say(msg=""):
    print(msg, flush=True)


def run(cmd, **kw):
    say("  $ " + " ".join(str(c) if " " not in str(c) else '"%s"' % c for c in cmd))
    return subprocess.run([str(c) for c in cmd], **kw)


def dirty():
    return U.git("status", "--porcelain").strip()


def sync_presets(old, new, dry):
    """Copy upstream presets at `new` into presets/ (bytes); report deletions. Returns (changes, deletions)."""
    up = {p: sha for p, (mode, sha) in U.tree_files(new, "presets").items() if mode != "160000"}
    old_up = set(U.tree_files(old, "presets")) if old else set()
    changes = []
    for p in sorted(up):
        dest = ROOT / p
        cur = U.blob_sha(dest.read_bytes()) if dest.is_file() else None
        if cur == up[p]:
            continue
        changes.append(("add" if cur is None else "update", p))
        if not dry:
            data = U.upstream_git("cat-file", "blob", up[p], binary=True)
            dest.parent.mkdir(parents=True, exist_ok=True)
            with open(dest, "wb") as f:
                f.write(data)
    ours = {f.relative_to(ROOT).as_posix() for f in U.PRESETS.rglob("*") if f.is_file()}
    deletions = sorted(p for p in ours - set(up))
    return changes, deletions, sorted(old_up - set(up))


def extract_upstream(commit, dest):
    """Upstream's Sources/ at `commit` into `dest` (for a dry-run schema without moving the submodule)."""
    data = U.upstream_git("archive", "--format=tar", commit, "--", "Sources", binary=True)
    Path(dest).mkdir(parents=True, exist_ok=True)
    tar_path = Path(dest) / "src.tar"
    tar_path.write_bytes(data)
    with tarfile.open(tar_path) as t:
        for m in t.getmembers():
            if m.name.startswith("/") or ".." in Path(m.name).parts:
                raise U.Fail("unexpected path in git archive: %s" % m.name)
        try:
            t.extractall(dest, filter="data")
        except TypeError:   # Python without extraction filters (< 3.9.17 / 3.10.12 / 3.11.4)
            t.extractall(dest)
    tar_path.unlink()


def gen_schema(upstream_dir, out, allow_breaking, extra=()):
    cmd = [sys.executable, U.GEN_SCHEMA, "--upstream", upstream_dir, "--out", out]
    if (SLANG / "crt").is_dir():
        cmd += ["--slang-shaders", SLANG]
    else:
        say("  (vendor/slang-shaders not checked out: gen_schema.py fetches it; "
            "`git submodule update --init vendor/slang-shaders` avoids that)")
    if allow_breaking:
        cmd.append("--allow-breaking")
    cmd += list(extra)
    return run(cmd)


def schema_summary(old_text, new_text):
    a, b = json.loads(old_text), json.loads(new_text)
    ida = {p["id"]: p for p in a.get("params", [])}
    idb = {p["id"]: p for p in b.get("params", [])}
    out = []
    for label, ids in (("new params", sorted(set(idb) - set(ida))), ("removed params", sorted(set(ida) - set(idb))),
                       ("changed params", sorted(k for k in set(ida) & set(idb) if ida[k] != idb[k]))):
        if ids:
            out.append("%s (%d): %s" % (label, len(ids), ", ".join(ids)))
    for k in sorted((set(a) | set(b)) - {"params"}):
        if a.get(k) != b.get(k):
            out.append("%s changed" % k)
    return out


def update_versions(version, sha, dry):
    changed = []
    for rel, rx, what in VERSION_SITES:
        p = ROOT / rel
        text = p.read_bytes().decode("utf-8")
        m = re.search(rx, text)
        if not m:
            say("  WARNING: %s: no match for the %s; update it by hand" % (rel, what))
            continue
        if m.group(2) == version:
            continue
        changed.append("%s: %s -> %s (%s)" % (rel, m.group(2), version, what))
        if not dry:
            text = text[:m.start(2)] + version + text[m.end(2):]
            with open(p, "w", encoding="utf-8", newline="") as f:
                f.write(text)
    for rel, rx, what in PIN_SITES:
        p = ROOT / rel
        text = p.read_bytes().decode("utf-8")
        m = re.search(rx, text)
        if not m:
            say("  WARNING: %s: no match for the %s; update it by hand" % (rel, what))
        elif m.group(2) != version or m.group(4) != sha[:7]:
            changed.append("%s: %s (%s) -> %s (%s) (%s)" % (rel, m.group(2), m.group(4), version, sha[:7], what))
            if not dry:
                text = text[:m.start(2)] + version + m.group(3) + sha[:7] + text[m.end(4):]
                with open(p, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
    for rel, rx, what in SHA_SITES:
        p = ROOT / rel
        text = p.read_bytes().decode("utf-8")
        m = re.search(rx, text)
        if not m:
            say("  WARNING: %s: no match for the %s; update it by hand" % (rel, what))
        elif m.group(2) != sha:
            changed.append("%s: %s -> %s (%s)" % (rel, m.group(2)[:7], sha[:7], what))
            if not dry:
                text = text[:m.start(2)] + sha + text[m.end(2):]
                with open(p, "w", encoding="utf-8", newline="") as f:
                    f.write(text)
    return changed


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--to", required=True, help="upstream tag or commit to sync to (e.g. v0.21.0)")
    ap.add_argument("--fetch", action="store_true", help="git fetch upstream tags first")
    ap.add_argument("--dry-run", action="store_true", help="print what would change; change nothing")
    ap.add_argument("--allow-breaking", action="store_true", help="pass --allow-breaking to tools/gen_schema.py")
    ap.add_argument("--allow-dirty", action="store_true",
                    help="skip the clean-tree check (re-running after fixing what stopped a sync)")
    a = ap.parse_args(argv)
    U.utf8_stdio()
    dry = a.dry_run
    would = "would: " if dry else ""   # message prefix (a real run says what it did)
    try:
        U.need_upstream()
        if a.fetch:
            U.fetch_upstream()
        st = dirty()
        if st and not a.allow_dirty:
            if dry:
                say("NOTE: the working tree has uncommitted changes; a real run would refuse:\n" + st)
            else:
                raise U.Fail("the working tree has uncommitted changes: commit or stash them first "
                             "(or --allow-dirty when re-running a sync)\n" + st)
        new = U.resolve(a.to)
        cur = U.upstream_git("rev-parse", "HEAD").strip()
        pin = U.schema_pin()
        recorded = U.gitlink(U.UPSTREAM_REL)
        old = pin if U.git_ok("cat-file", "-e", pin + "^{commit}", cwd=U.UPSTREAM) else recorded
        version = U.exact_tag(new) or U.describe(new)
        say("== upstream: %s (%s) -> %s (%s)%s" % (U.describe(old), old[:7], version, new[:7],
                                                   "   [dry run: nothing is changed]" if dry else ""))
        if not U.exact_tag(new):
            say("  NOTE: %s is not a release tag; version strings will say %s" % (new[:7], version))
        if not U.git_ok("merge-base", "--is-ancestor", old, new, cwd=U.UPSTREAM):
            say("  WARNING: %s is not an ancestor of %s (a downgrade, or rewritten history)" % (old[:7], new[:7]))
        todo = 0

        say("\n== 1. submodule upstream/NTSCRT")
        if cur == new and recorded == new:
            say("  already at %s" % new[:7])
        else:
            if cur != new:
                todo += 1
                say("  %scheckout %s -> %s" % (would, cur[:7], new[:7]))
                if not dry:
                    U.upstream_git("checkout", "--quiet", new)
            if recorded != new:
                say("  gitlink: recorded %s; `git add upstream/NTSCRT` when you commit the sync" % recorded[:7])
                todo += 1 if cur == new else 0

        say("\n== 2. presets/ (byte for byte)")
        changes, extra, up_deleted = sync_presets(old, new, dry)
        for kind, p in changes:
            say("  %s%s %s" % (would, kind, p))
        todo += len(changes)
        pm = U.PortMap()
        for p in extra:
            why = "kept (retained_presets)" if p in pm.retained_presets else \
                "NOT deleted: upstream %s it; delete it or list it in port-map.json retained_presets" % (
                    "deleted" if p in up_deleted else "doesn't have")
            say("  %s: %s" % (p, why))
        if not changes and not extra:
            say("  in sync")

        say("\n== 3. schema (tools/gen_schema.py)")
        old_schema = U.SCHEMA.read_bytes()
        with tempfile.TemporaryDirectory(prefix="ntscrt-sync-") as tmp:
            tmp = Path(tmp)
            if dry:
                extract_upstream(new, tmp / "up")
                shutil.copyfile(U.SCHEMA, tmp / "schema.json")   # so gen_schema's breaking-change check runs
                r = gen_schema(tmp / "up", tmp / "schema.json", a.allow_breaking)
                if r.returncode != 0:
                    say("  gen_schema.py refused (exit %d): a real sync stops here" % r.returncode)
                    return r.returncode
                doc = json.loads((tmp / "schema.json").read_text(encoding="utf-8"))
                doc["sources"]["upstream_ntscrt"] = new   # the extracted tree isn't a git checkout
                new_schema = (json.dumps(doc, indent=1, ensure_ascii=False) + "\n").encode("utf-8")
                (tmp / "schema.json").write_bytes(new_schema)
                schema_path = tmp / "schema.json"
            else:
                r = gen_schema(U.UPSTREAM, U.SCHEMA, a.allow_breaking)
                if r.returncode != 0:
                    say("\nsync_upstream: tools/gen_schema.py refused (exit %d). The submodule and presets are "
                        "already moved. Fix it (a removed control stays as a legacy param, see DIGITAL_LEGACY in "
                        "tools/gen_schema.py and UPSTREAM-SYNC.md's compatibility rules), or pass "
                        "--allow-breaking only before a first release; then re-run with --allow-dirty."
                        % r.returncode)
                    return r.returncode
                new_schema = U.SCHEMA.read_bytes()
                schema_path = U.SCHEMA
            if new_schema == old_schema:
                say("  schema.json unchanged")
            else:
                todo += 1
                for line in schema_summary(old_schema.decode("utf-8"), new_schema.decode("utf-8")):
                    say("  %s%s" % (would, line))

            say("\n== 4. FxPlug parameter ids (fxplug/tools/update_param_ids.py)")
            cmd = [sys.executable, UPDATE_IDS, "--schema", schema_path] + (["--check"] if dry else [])
            r = run(cmd, capture_output=True, text=True)
            out = (r.stdout + r.stderr).strip()
            if out:
                say("  " + out.replace("\n", "\n  "))
            if dry and r.returncode != 0:
                todo += 1
                say("  (a real run adds those ids)")
            elif not dry and r.returncode != 0:
                raise U.Fail("update_param_ids.py failed (exit %d)" % r.returncode)

        say("\n== 5. version strings")
        vchanges = update_versions(version, new, dry)
        for c in vchanges:
            say("  %s%s" % (would, c))
        if not vchanges:
            say("  already %s" % version)
        todo += len(vchanges)

        rep = upstream_diff.build(old, new, pm)
        say("\n== hand-port list (%s..%s; full report: python scripts/upstream_diff.py --from %s --to %s)"
            % (U.describe(old), version, old[:7], new[:7]))
        lines = upstream_diff.hand_port_lines(rep)
        say("\n".join(lines) if lines else "  nothing to port by hand")
        for v in rep["vendor"]:
            if v["old"] != v["new"] or (v["new"] and v["ours"] != v["new"]):
                say("  VENDOR: upstream pins %s at %s, ours %s is %s: bump it (check_sync.py lists every place)"
                    % (v["upstream"], (v["new"] or "-")[:8], v["ours_path"], (v["ours"] or "-")[:8]))
        if rep["preset_deletions"]:
            say("  PRESETS deleted upstream: %s" % ", ".join(rep["preset_deletions"]))

        if dry:
            say("\n== dry run: %s" % ("nothing to change" if todo == 0 else "%d change(s) above" % todo))
            if todo == 0 and not lines:
                return 0
        say("""
== left to do by hand (docs/dev/UPSTREAM-SYNC.md)
  - port the files above (keep upstream's arithmetic and order; tests next to the existing ones)
  - new bundled looks / Digital video formats: append to released_preset_order() in
    hostparams/src/params.cpp when releasing (never reorder)
  - looks using a stage the Resolve cli test doesn't cover: DIGITAL_LOOKS in tests/resolve/run_resolve_tests.py
  - host tests: tests/hostparams/ofx_reference.json is a frozen snapshot (new params: add them to its default
    look at their defaults; new looks: their menu entry and case), and the released_preset_order() size check
    in tests/hostparams/test_params_timeline.cpp
  - rebuild an existing build tree so it copies new presets (CMake re-globs presets/ on build)
  - self-goldens: add new looks to tests/golden/self/manifest.json; regenerate only entries whose look
    legitimately changed (NTSCRT_GOLDEN_UPDATE=1 NTSCRT_GOLDEN_ONLY=<id>); the rest must stay byte-identical
  - presets gallery: scripts/make_presets_gallery.py --only <new slugs> (docs/PRESETS-GALLERY.md)
  - CHANGELOG.md line under [Unreleased] (changed defaults, what happens to existing projects) and the
    sync log at the end of docs/dev/UPSTREAM-SYNC.md
  - build + ctest; then refresh the golden references: gh workflow run golden --ref <branch>,
    gh run download <id> -n golden-refs -D tests/golden/ref (tests/golden/README.md)
  - python scripts/check_sync.py (CI job sync-check) until it passes; git add upstream/NTSCRT; commit""")
    except U.Fail as e:
        print("sync_upstream: error: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
