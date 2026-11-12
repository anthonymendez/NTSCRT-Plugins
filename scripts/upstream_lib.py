"""Shared helpers for the upstream-sync scripts (upstream_diff.py, sync_upstream.py, check_sync.py).

Not a command: imported from the scripts next to it. Stdlib only, Python 3.9+, Windows and Linux.
The port map (docs/dev/port-map.json) says where each upstream NTSCRT file lands here; see
docs/dev/UPSTREAM-SYNC.md.
"""
import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
UPSTREAM = ROOT / "upstream" / "NTSCRT"
UPSTREAM_REL = "upstream/NTSCRT"
PORT_MAP = ROOT / "docs" / "dev" / "port-map.json"
SCHEMA = ROOT / "core" / "params" / "schema.json"
GEN_SCHEMA = ROOT / "tools" / "gen_schema.py"
PRESETS = ROOT / "presets"

KINDS = ("hand-port", "generated-input", "preset", "app-only", "test-port", "shader-port")


def utf8_stdio():
    """Markdown and paths with non-ASCII characters print on a Windows console too."""
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except (AttributeError, ValueError):
            pass


class Fail(Exception):
    """A user-facing error: printed without a traceback."""


def git(*args, cwd=ROOT, check=True, binary=False):
    """Run git; return stdout (str, or bytes with binary=True). Raises Fail when check and git fails."""
    cmd = ["git", "-c", "core.quotepath=off", "-C", str(cwd), *args]
    r = subprocess.run(cmd, capture_output=True)
    if check and r.returncode != 0:
        raise Fail("`git %s` failed (in %s): %s" % (" ".join(args), cwd, r.stderr.decode("utf-8", "replace").strip()))
    if binary:
        return r.stdout
    return r.stdout.decode("utf-8", "replace")


def git_ok(*args, cwd=ROOT):
    return subprocess.run(["git", "-C", str(cwd), *args], capture_output=True).returncode == 0


def upstream_git(*args, **kw):
    return git(*args, cwd=UPSTREAM, **kw)


def upstream_initialised():
    return (UPSTREAM / ".git").exists() and git_ok("rev-parse", "--verify", "HEAD^{commit}", cwd=UPSTREAM)


def need_upstream():
    if not upstream_initialised():
        raise Fail("upstream/NTSCRT is not checked out: git submodule update --init upstream/NTSCRT")


def gitlink(path):
    """Commit a submodule is recorded at (the index, which is HEAD unless a bump is staged).
    Works without the submodule being initialised."""
    out = git("ls-files", "--stage", "--", path).split()
    if len(out) >= 2 and out[0] == "160000":
        return out[1]
    out = git("ls-tree", "HEAD", "--", path).split()
    if len(out) >= 3 and out[1] == "commit":
        return out[2]
    raise Fail("%s is not a submodule (no gitlink in the index or HEAD)" % path)


def resolve(ref):
    """Full commit SHA of `ref` in the upstream submodule."""
    need_upstream()
    for cand in (ref, "refs/tags/" + ref, "origin/" + ref):
        if git_ok("rev-parse", "--verify", "--quiet", cand + "^{commit}", cwd=UPSTREAM):
            return upstream_git("rev-parse", cand + "^{commit}").strip()
    raise Fail("upstream ref %r not found in upstream/NTSCRT (try --fetch, or a tag / commit)" % ref)


def describe(sha):
    """'v0.20.0' when sha is a tag, else 'v0.20.0-3-gabcdef1' (or the short sha)."""
    exact = git("describe", "--tags", "--exact-match", "--match", "v*", sha, cwd=UPSTREAM, check=False).strip()
    if exact:
        return exact
    d = git("describe", "--tags", "--match", "v*", sha, cwd=UPSTREAM, check=False).strip()
    return d or sha[:7]


def exact_tag(sha):
    return git("describe", "--tags", "--exact-match", "--match", "v*", sha, cwd=UPSTREAM, check=False).strip() or None


def version_key(tag):
    """Sort key for vX.Y.Z[-suffix] tags (a suffix sorts before the plain release)."""
    m = re.match(r"^v(\d+)\.(\d+)\.(\d+)(.*)$", tag)
    if not m:
        return None
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), 0 if m.group(4) else 1, m.group(4))


def newest_tag(release_only=True):
    tags = [t for t in upstream_git("tag", "--list", "v*").split() if version_key(t)]
    if release_only:
        tags = [t for t in tags if re.match(r"^v\d+\.\d+\.\d+$", t)]
    if not tags:
        raise Fail("upstream/NTSCRT has no v* tags (fetch them: git -C upstream/NTSCRT fetch --tags origin)")
    return max(tags, key=version_key)


def fetch_upstream():
    need_upstream()
    print("fetching upstream (origin, tags)...", file=sys.stderr)
    upstream_git("fetch", "--quiet", "--tags", "origin")
    if upstream_git("rev-parse", "--is-shallow-repository").strip() == "true":
        upstream_git("fetch", "--quiet", "--unshallow", "origin")


def schema_pin():
    """upstream_ntscrt recorded in core/params/schema.json."""
    doc = json.loads(SCHEMA.read_text(encoding="utf-8"))
    pin = doc.get("sources", {}).get("upstream_ntscrt", "")
    if not re.fullmatch(r"[0-9a-f]{40}", pin):
        raise Fail("core/params/schema.json sources.upstream_ntscrt is %r, not a commit SHA" % pin)
    return pin


def blob_sha(data):
    """Git blob id of these exact bytes (no eol/filters), so a byte flip is always caught."""
    return hashlib.sha1(b"blob %d\0" % len(data) + data).hexdigest()


def tree_files(commit, *paths, cwd=UPSTREAM):
    """{path: (mode, blob_sha)} for files under `paths` at `commit` (submodule gitlinks: mode 160000)."""
    out = git("ls-tree", "-r", "-z", commit, "--", *paths, cwd=cwd, binary=True).decode("utf-8", "replace")
    files = {}
    for rec in out.split("\0"):
        if not rec:
            continue
        meta, path = rec.split("\t", 1)
        mode, _typ, sha = meta.split()
        files[path] = (mode, sha)
    return files


def upstream_vendor_pin(commit, path):
    """Commit upstream pins its own submodule `path` (e.g. Vendor/ntsc-rs) at, or None."""
    f = tree_files(commit, path)
    v = f.get(path)
    return v[1] if v and v[0] == "160000" else None


# ---------------------------------------------------------------------------
# port map

def glob_regex(pattern):
    """'**' = any depth (also zero dirs: 'a/**' matches 'a/x'), '*' = within a directory, '?' = one char."""
    out, i = [], 0
    while i < len(pattern):
        if pattern.startswith("**/", i):
            out.append("(?:.*/)?")
            i += 3
        elif pattern.startswith("**", i):
            out.append(".*")
            i += 2
        elif pattern[i] == "*":
            out.append("[^/]*")
            i += 1
        elif pattern[i] == "?":
            out.append("[^/]")
            i += 1
        else:
            out.append(re.escape(pattern[i]))
            i += 1
    return re.compile("^" + "".join(out) + "$")


def is_glob(p):
    return any(c in p for c in "*?")


class PortMap:
    def __init__(self, path=PORT_MAP):
        try:
            self.doc = json.loads(Path(path).read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            raise Fail("cannot read %s: %s" % (path, e))
        self.path = Path(path)
        self.roots = self.doc.get("roots", [])
        self.vendor_pins = self.doc.get("vendor_pins", {})
        self.retained_presets = set(self.doc.get("retained_presets", []))
        self.entries = self.doc.get("entries", [])
        self.exact = {}
        self.globs = []
        for e in self.entries:
            e["kind"] = [e["kind"]] if isinstance(e.get("kind"), str) else list(e.get("kind", []))
            e.setdefault("ours", [])
            e.setdefault("notes", "")
            if is_glob(e["upstream"]):
                self.globs.append((glob_regex(e["upstream"]), e))
            else:
                self.exact[e["upstream"]] = e

    def problems(self):
        """Malformed entries (unknown kinds, duplicates)."""
        out, seen = [], set()
        for e in self.entries:
            u = e.get("upstream")
            if not u:
                out.append("entry without 'upstream': %r" % e)
                continue
            if u in seen:
                out.append("duplicate entry: %s" % u)
            seen.add(u)
            if not e["kind"]:
                out.append("%s: no kind" % u)
            for k in e["kind"]:
                if k not in KINDS:
                    out.append("%s: unknown kind %r (one of %s)" % (u, k, ", ".join(KINDS)))
        return out

    def in_roots(self, path):
        return any(path.startswith(r) for r in self.roots)

    def match(self, path):
        e = self.exact.get(path)
        if e is not None:
            return e
        for rx, e in self.globs:
            if rx.match(path):
                return e
        return None
