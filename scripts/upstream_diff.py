#!/usr/bin/env python3
"""Report what an upstream NTSCRT range changes for this port, as Markdown.

  python scripts/upstream_diff.py                       # pin (schema.json) .. origin/main
  python scripts/upstream_diff.py --to v0.21.0          # pin .. a tag or commit
  python scripts/upstream_diff.py --from 2b3013d --to c6ab639
  python scripts/upstream_diff.py --latest-tag          # pin .. upstream's newest vX.Y.Z tag
  python scripts/upstream_diff.py --fetch --strict --out report.md

Every changed upstream file under the port map's roots (docs/dev/port-map.json) is sorted into:
handled automatically (presets, schema inputs: scripts/sync_upstream.py), hand-port needed (with
+/- lines and our target files), tests to port, app-only (collapsed) and UNMAPPED (a file the port
map doesn't know: add an entry; --strict exits 1). Also lists the upstream commits in the range
and upstream's Vendor pins (ntsc-rs, slang-shaders) against ours in vendor/.

Reads the upstream/NTSCRT submodule only; no network unless --fetch. Changes nothing.
--github-output FILE appends ahead=, from=, to=, tag= lines (the upstream-watch workflow).
Stdlib only, Python 3.9+. See docs/dev/UPSTREAM-SYNC.md.
"""
import argparse
import sys

import upstream_lib as U


def changed_files(old, new):
    """[(status, path, old_path, added, deleted)] between two upstream commits (renames detected).
    added / deleted are None for binary files."""
    raw = U.upstream_git("diff", "--name-status", "-z", "-M", old, new, binary=True).decode("utf-8", "replace")
    parts = raw.split("\0")
    status = {}
    i = 0
    while i < len(parts) and parts[i]:
        st = parts[i]
        if st[0] in "RC":
            status[parts[i + 2]] = (st[0], parts[i + 1])
            i += 3
        else:
            status[parts[i + 1]] = (st[0], parts[i + 1])
            i += 2
    raw = U.upstream_git("diff", "--numstat", "-z", "-M", old, new, binary=True).decode("utf-8", "replace")
    parts = raw.split("\0")
    counts = {}
    i = 0
    while i < len(parts) and parts[i]:
        a, d, path = parts[i].split("\t", 2)
        if path == "":           # rename: "a\td\t" NUL old NUL new
            path = parts[i + 2]
            i += 3
        else:
            i += 1
        counts[path] = (None if a == "-" else int(a), None if d == "-" else int(d))
    out = []
    for path, (st, old_path) in status.items():
        a, d = counts.get(path, (None, None))
        out.append((st, path, old_path, a, d))
    out.sort(key=lambda r: r[1].lower())
    return out


def build(old, new, pm=None):
    """Structured report of old..new (full SHAs)."""
    pm = pm or U.PortMap()
    rep = {"old": old, "new": new, "old_name": U.describe(old), "new_name": U.describe(new),
           "auto_presets": [], "auto_schema": [], "hand": [], "tests": [], "app": [], "unmapped": [],
           "outside": [], "commits": [], "vendor": [], "preset_deletions": []}
    if old == new:
        pass
    elif not U.git_ok("merge-base", "--is-ancestor", old, new, cwd=U.UPSTREAM):
        rep["not_ancestor"] = True
    for st, path, old_path, a, d in (changed_files(old, new) if old != new else []):
        key = old_path if st == "D" else path
        row = {"status": st, "path": path, "old_path": old_path, "added": a, "deleted": d}
        if not pm.in_roots(key) and not (old_path and pm.in_roots(old_path)):
            rep["outside"].append(row)
            continue
        e = pm.match(key) or (pm.match(old_path) if old_path else None)
        if e is None:
            rep["unmapped"].append(row)
            continue
        row["entry"] = e
        kinds = e["kind"]
        if "preset" in kinds:
            rep["auto_presets"].append(row)
            if st == "D":
                rep["preset_deletions"].append(path)
        if "generated-input" in kinds:
            rep["auto_schema"].append(row)
        if "hand-port" in kinds or "shader-port" in kinds:
            rep["hand"].append(row)
        if "test-port" in kinds:
            rep["tests"].append(row)
        # a UI file that also feeds the schema (DownscalePanel) shows under both
        if "app-only" in kinds and not {"hand-port", "shader-port", "test-port"} & set(kinds):
            rep["app"].append(row)
    if old != new:
        log = U.upstream_git("log", "--format=%h%x09%ad%x09%s", "--date=short", "%s..%s" % (old, new))
        rep["commits"] = [line.split("\t", 2) for line in log.splitlines() if line.strip()]
    for up_path, ours_path in pm.vendor_pins.items():
        try:
            ours = U.gitlink(ours_path)
        except U.Fail:
            ours = None
        rep["vendor"].append({"upstream": up_path, "ours_path": ours_path,
                              "old": U.upstream_vendor_pin(old, up_path), "new": U.upstream_vendor_pin(new, up_path),
                              "ours": ours})
    return rep


def _lines(r):
    if r["added"] is None:
        return "binary"
    return "+%d / -%d" % (r["added"], r["deleted"])


def _name(r):
    st = {"A": "added", "D": "deleted", "M": "modified", "R": "renamed", "C": "copied", "T": "type changed"}
    s = st.get(r["status"], r["status"])
    if r["status"] in "RC" and r["old_path"] != r["path"]:
        return "`%s` (%s from `%s`)" % (r["path"], s, r["old_path"])
    return "`%s` (%s)" % (r["path"], s)


def _cell(text):
    return text.replace("|", "\\|").replace("\n", " ")


def hand_port_lines(rep):
    """Plain-text hand-port list (sync_upstream.py prints it)."""
    out = []
    for r in rep["hand"]:
        tag = "/".join(k for k in r["entry"]["kind"] if k in ("hand-port", "shader-port", "generated-input"))
        out.append("  %-58s %-14s [%s] -> %s" % (r["path"], _lines(r), tag, ", ".join(r["entry"]["ours"]) or "-"))
    for r in rep["tests"]:
        out.append("  %-58s %-14s [test-port] -> %s" % (r["path"], _lines(r), ", ".join(r["entry"]["ours"]) or "-"))
    for r in rep["unmapped"]:
        out.append("  %-58s %-14s [UNMAPPED] add it to docs/dev/port-map.json" % (r["path"], _lines(r)))
    return out


def markdown(rep, max_commits=200):
    o = []
    o.append("## Upstream NTSCRT %s -> %s" % (rep["old_name"], rep["new_name"]))
    o.append("")
    o.append("Range `%s..%s` in `upstream/NTSCRT` (map: `docs/dev/port-map.json`). Generated by "
             "`scripts/upstream_diff.py`." % (rep["old"][:7], rep["new"][:7]))
    if rep.get("not_ancestor"):
        o.append("")
        o.append("> **Warning:** `%s` is not an ancestor of `%s` (upstream history was rewritten, or the range is "
                 "reversed); the file list is a plain tree diff." % (rep["old"][:7], rep["new"][:7]))
    o.append("")
    n = lambda k: len(rep[k])  # noqa: E731
    o.append("**Summary:** %d hand-port, %d tests, %d presets and %d schema inputs handled automatically, "
             "%d app-only, **%d unmapped**, %d outside the mapped roots, %d commits."
             % (n("hand"), n("tests"), n("auto_presets"), n("auto_schema"), n("app"), n("unmapped"),
                n("outside"), n("commits")))
    o.append("")
    if rep["unmapped"]:
        o.append("### UNMAPPED (add to docs/dev/port-map.json)")
        o.append("")
        for r in rep["unmapped"]:
            o.append("- %s, %s" % (_name(r), _lines(r)))
        o.append("")
    o.append("### Handled automatically (`scripts/sync_upstream.py`)")
    o.append("")
    if not rep["auto_presets"] and not rep["auto_schema"]:
        o.append("Nothing.")
    for r in rep["auto_presets"]:
        what = "**deleted upstream: not deleted here, decide by hand** (see `retained_presets`)" \
            if r["status"] == "D" else "copied byte for byte"
        o.append("- preset %s: %s" % (_name(r), what))
    for r in rep["auto_schema"]:
        o.append("- schema input %s, %s: `tools/gen_schema.py` regenerates `core/params/schema.json`"
                 % (_name(r), _lines(r)))
    o.append("")
    o.append("### Hand-port needed")
    o.append("")
    if rep["hand"]:
        o.append("| Upstream file | Lines | Kind | Our targets | Notes |")
        o.append("|---|---|---|---|---|")
        for r in rep["hand"]:
            e = r["entry"]
            o.append("| %s | %s | %s | %s | %s |" % (
                _cell(_name(r)), _lines(r), ", ".join(e["kind"]),
                _cell(", ".join("`%s`" % p for p in e["ours"]) or "-"), _cell(e["notes"])))
    else:
        o.append("Nothing.")
    o.append("")
    o.append("### Tests to port")
    o.append("")
    if rep["tests"]:
        for r in rep["tests"]:
            e = r["entry"]
            o.append("- %s, %s -> %s%s" % (_name(r), _lines(r), ", ".join("`%s`" % p for p in e["ours"]) or "-",
                                          (" (" + e["notes"] + ")") if e["notes"] else ""))
    else:
        o.append("Nothing.")
    o.append("")
    o.append("### Vendor pins (upstream `Vendor/` vs our `vendor/`)")
    o.append("")
    o.append("| Upstream submodule | Upstream at %s | Upstream at %s | Ours | |" % (rep["old_name"], rep["new_name"]))
    o.append("|---|---|---|---|---|")
    for v in rep["vendor"]:
        s = lambda x: ("`%s`" % x[:8]) if x else "-"  # noqa: E731
        notes = []
        if v["old"] != v["new"]:
            notes.append("**upstream moved it**: bump `%s` (and every place check_sync.py compares)" % v["ours_path"])
        if v["new"] and v["ours"] and v["new"] != v["ours"]:
            notes.append("differs from ours")
        o.append("| `%s` | %s | %s | %s (`%s`) | %s |" % (v["upstream"], s(v["old"]), s(v["new"]), s(v["ours"]),
                                                       v["ours_path"], "; ".join(notes) or "same"))
    o.append("")
    if rep["app"]:
        o.append("<details><summary>App-only (%d files: read for behaviour, port nothing)</summary>" % len(rep["app"]))
        o.append("")
        for r in rep["app"]:
            o.append("- %s, %s" % (_name(r), _lines(r)))
        o.append("")
        o.append("</details>")
        o.append("")
    if rep["outside"]:
        o.append("<details><summary>Outside the mapped roots (%d files: docs, scripts, other projects)</summary>"
                 % len(rep["outside"]))
        o.append("")
        for r in rep["outside"][:300]:
            o.append("- %s, %s" % (_name(r), _lines(r)))
        if len(rep["outside"]) > 300:
            o.append("- ... and %d more" % (len(rep["outside"]) - 300))
        o.append("")
        o.append("</details>")
        o.append("")
    o.append("### Upstream commits (%d)" % len(rep["commits"]))
    o.append("")
    for c in rep["commits"][:max_commits]:
        o.append("- `%s` %s %s" % (c[0], c[1], c[2] if len(c) > 2 else ""))
    if len(rep["commits"]) > max_commits:
        o.append("- ... and %d more" % (len(rep["commits"]) - max_commits))
    o.append("")
    return "\n".join(o)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--from", dest="old", help="old upstream ref (default: core/params/schema.json upstream_ntscrt)")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--to", dest="new", help="new upstream ref: tag or commit (default: origin/main)")
    g.add_argument("--latest-tag", action="store_true", help="new = upstream's newest vX.Y.Z tag")
    ap.add_argument("--fetch", action="store_true", help="git fetch upstream (tags, origin) first")
    ap.add_argument("--strict", action="store_true", help="exit 1 when a changed file is unmapped")
    ap.add_argument("--out", help="write the Markdown here instead of stdout")
    ap.add_argument("--max-chars", type=int, default=0, help="truncate the Markdown (GitHub issue bodies: 65536)")
    ap.add_argument("--github-output", help="append ahead=/from=/to=/tag= to this file ($GITHUB_OUTPUT)")
    a = ap.parse_args(argv)
    U.utf8_stdio()
    try:
        U.need_upstream()
        if a.fetch:
            U.fetch_upstream()
        old = U.resolve(a.old) if a.old else U.schema_pin()
        if a.latest_tag:
            new_ref = U.newest_tag()
        else:
            new_ref = a.new or "origin/main"
        new = U.resolve(new_ref)
        rep = build(old, new)
        md = markdown(rep)
        if a.max_chars and len(md) > a.max_chars:
            md = md[:a.max_chars - 200].rsplit("\n", 1)[0] + "\n\n... (truncated: run scripts/upstream_diff.py locally)\n"
        if a.out:
            with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                f.write(md)
        else:
            sys.stdout.write(md)
        if a.github_output:
            ahead = old != new and U.git_ok("merge-base", "--is-ancestor", old, new, cwd=U.UPSTREAM)
            with open(a.github_output, "a", encoding="utf-8", newline="\n") as f:
                f.write("ahead=%s\nfrom=%s\nto=%s\nfrom_name=%s\ntag=%s\n"
                        % ("true" if ahead else "false", old, new, rep["old_name"], new_ref if a.latest_tag
                           else rep["new_name"]))
        if rep["unmapped"]:
            print("upstream_diff: %d changed file(s) not in docs/dev/port-map.json: %s"
                  % (len(rep["unmapped"]), ", ".join(r["path"] for r in rep["unmapped"])), file=sys.stderr)
            if a.strict:
                return 1
    except U.Fail as e:
        print("upstream_diff: error: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
