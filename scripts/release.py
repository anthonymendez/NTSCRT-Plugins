#!/usr/bin/env python3
"""Cut a release: date the CHANGELOG's Unreleased section and set VERSION.

  python scripts/release.py              # release the version in VERSION (first release: 0.1.0)
  python scripts/release.py patch        # bump 0.1.0 -> 0.1.1, then release it
  python scripts/release.py minor|major  # 0.1.1 -> 0.2.0 / 1.0.0
  python scripts/release.py 0.3.0        # explicit version
  python scripts/release.py rc           # release candidate: 0.4.0 -> 0.4.1-rc.1, 0.4.1-rc.1 -> 0.4.1-rc.2
  python scripts/release.py 0.5.0-rc.1   # explicit release candidate
  python scripts/release.py --notes v0.1.0   # print that version's CHANGELOG section (CI release notes)

With VERSION at X.Y.Z-rc.N, no argument releases the final X.Y.Z, and patch / minor / major count
from X.Y.Z. A release candidate only sets VERSION: the CHANGELOG's Unreleased section stays open
(its text is the candidate's notes) until the final release dates it. CI publishes -rc tags as
GitHub *prereleases*, which `releases/latest` (what the installers download) skips.

Refuses to run on a dirty tree, on a branch other than main, or behind origin/main.
Edits VERSION and CHANGELOG.md only, then prints the git commands to commit,
tag and push. Pushing the tag makes CI build and publish the GitHub Release.
See docs/RELEASING.md.
"""
import argparse
import datetime
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VERSION = ROOT / "VERSION"
CHANGELOG = ROOT / "CHANGELOG.md"
REPO = "https://github.com/anthonymendez/NTSCRT-Plugins"
SEMVER = re.compile(r"^(\d+)\.(\d+)\.(\d+)(?:-rc\.(\d+))?$")   # X.Y.Z or X.Y.Z-rc.N


def die(msg):
    print(f"release: {msg}", file=sys.stderr)
    sys.exit(1)


def git(*args):
    return subprocess.run(["git", "-C", str(ROOT), *args], capture_output=True, text=True)


def check_git():
    """Release only from a clean, up-to-date main: what gets tagged is what CI built on main."""
    st = git("status", "--porcelain")
    if st.returncode != 0:
        die("not a git checkout (git status failed)")
    if st.stdout.strip():
        die("the working tree has uncommitted changes: commit or stash them first\n" + st.stdout.rstrip())
    branch = git("rev-parse", "--abbrev-ref", "HEAD").stdout.strip()
    if branch != "main":
        die(f"on branch '{branch}': releases are cut from main")
    if git("fetch", "--quiet", "origin", "main").returncode != 0:
        die("git fetch origin main failed: can't check that main is up to date")
    behind = git("rev-list", "--count", "HEAD..origin/main").stdout.strip()
    if behind != "0":
        die(f"main is {behind} commit(s) behind origin/main: git pull first")


def git_tags():
    try:
        out = subprocess.run(["git", "-C", str(ROOT), "tag", "--list", "v*"], capture_output=True, text=True, check=True)
        return set(out.stdout.split())
    except (OSError, subprocess.CalledProcessError):
        return set()


def section(text, heading_re):
    """(start, end) of the body of the first '## ' section whose heading matches."""
    m = re.search(heading_re, text, re.M)
    if not m:
        return None
    nxt = re.search(r"^## |^\[[^\]]+\]: ", text[m.end():], re.M)
    return m.end(), (m.end() + nxt.start()) if nxt else len(text)


def notes(tag):
    v = tag.lstrip("v")
    text = CHANGELOG.read_text(encoding="utf-8")
    span = section(text, rf"^## \[{re.escape(v)}\][^\n]*\n")
    m = SEMVER.match(v)
    if not span and m and m.group(4):
        # Release candidates have no section of their own: their notes are the Unreleased section.
        span = section(text, r"^## \[Unreleased\][^\n]*\n")
        if span:
            print(f"Release candidate for {v.split('-')[0]}: please report problems before the final release.\n")
    if not span:
        die(f"no '## [{v}]' section in CHANGELOG.md")
    print(text[span[0]:span[1]].strip())


def print_git_steps(new):
    print(f'  git commit -am "Release v{new}"')
    print(f"  git tag -a v{new} -m \"NTSCRT v{new}\"")
    print(f"  git push origin main v{new}")


def main():
    sys.stdout.reconfigure(encoding="utf-8")  # CHANGELOG text on Windows consoles
    ap =argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bump", nargs="?", default="", help="patch | minor | major | rc | X.Y.Z[-rc.N] (default: VERSION as is)")
    ap.add_argument("--notes", metavar="TAG", help="print the CHANGELOG section of TAG and exit")
    a = ap.parse_args()
    if a.notes:
        return notes(a.notes)

    check_git()
    cur = VERSION.read_text(encoding="utf-8").strip()
    m = SEMVER.match(cur)
    if not m:
        die(f"VERSION is '{cur}', expected X.Y.Z or X.Y.Z-rc.N")
    major, minor, patch = map(int, m.groups()[:3])
    cur_rc = int(m.group(4)) if m.group(4) else None
    base = f"{major}.{minor}.{patch}"
    tags = git_tags()
    if a.bump == "":
        new = base   # VERSION as is; an -rc.N VERSION releases its final X.Y.Z
    elif a.bump == "rc":
        if cur_rc is not None:
            new = f"{base}-rc.{cur_rc + 1}"
        elif f"v{base}" in tags:
            new = f"{major}.{minor}.{patch + 1}-rc.1"
        else:
            new = f"{base}-rc.1"
    elif a.bump == "patch":
        new = f"{major}.{minor}.{patch + 1}"
    elif a.bump == "minor":
        new = f"{major}.{minor + 1}.0"
    elif a.bump == "major":
        new = f"{major + 1}.0.0"
    elif SEMVER.match(a.bump):
        new = a.bump
    else:
        die(f"unknown bump '{a.bump}'")
    if f"v{new}" in tags:
        die(f"v{new} is already tagged; pass patch / minor / major / rc")
    is_rc = SEMVER.match(new).group(4) is not None

    if is_rc:
        # The Unreleased section stays open (CI uses it as the candidate's notes); only VERSION moves.
        text = CHANGELOG.read_text(encoding="utf-8")
        span = section(text, r"^## \[Unreleased\][^\n]*\n")
        if not span or not re.search(r"^- ", text[span[0]:span[1]], re.M):
            die("the Unreleased section is empty: add what changed first")
        VERSION.write_text(new + "\n", encoding="utf-8", newline="\n")
        print(f"release: {cur} -> {new} (release candidate: published as a GitHub prerelease). Review, then:\n")
        print_git_steps(new)
        return

    text = CHANGELOG.read_text(encoding="utf-8")
    span = section(text, r"^## \[Unreleased\][^\n]*\n")
    if not span:
        die("CHANGELOG.md has no '## [Unreleased]' section")
    body = text[span[0]:span[1]].strip()
    body = re.sub(r"^First release \(will be [^)]*\)\.\s*", "First release.\n\n", body)
    if not re.search(r"^- ", body, re.M):
        die("the Unreleased section is empty: add what changed first")
    date = datetime.date.today().isoformat()
    text = text[:span[0]] + f"\n## [{new}] - {date}\n\n{body}\n\n" + text[span[1]:]

    # Link references at the bottom: Unreleased compares from the new tag.
    text = re.sub(r"^\[Unreleased\]: .*$", f"[Unreleased]: {REPO}/compare/v{new}...HEAD", text, flags=re.M)
    finals = [SEMVER.match(t[1:]) for t in tags]
    prev = sorted((tuple(map(int, f.groups()[:3])) for f in finals if f and f.group(4) is None), reverse=True)
    link = f"{REPO}/compare/v{'.'.join(map(str, prev[0]))}...v{new}" if prev else f"{REPO}/releases/tag/v{new}"
    text = re.sub(r"^(\[Unreleased\]: .*)$", rf"\1\n[{new}]: {link}", text, count=1, flags=re.M)

    CHANGELOG.write_text(text, encoding="utf-8", newline="\n")
    VERSION.write_text(new + "\n", encoding="utf-8", newline="\n")
    print(f"release: {cur} -> {new}; CHANGELOG.md section dated {date}. Review, then:\n")
    print_git_steps(new)


if __name__ == "__main__":
    main()
