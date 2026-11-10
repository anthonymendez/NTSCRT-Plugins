#!/usr/bin/env python3
"""Zip every bundle with a given extension found under a build tree.

  package_bundles.py --build DIR --ext .ofx.bundle --out dist/NTSCRT-ofx-<os>.zip

Bundle *directories* are zipped with their relative layout preserved
(symlinks kept as links, which macOS framework bundles need). Exits 1 if none
were found unless --allow-empty, so a renamed OFX target can't silently ship
an empty artifact.
"""
import argparse
import os
import stat
import sys
import zipfile


def add_tree(zf, root, arc_prefix):
    for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
        rel_dir = os.path.relpath(dirpath, os.path.dirname(root))
        for name in sorted(dirnames):
            full = os.path.join(dirpath, name)
            if os.path.islink(full):  # symlinked dir (Versions/Current): store as link
                add_link(zf, full, os.path.join(arc_prefix, rel_dir, name))
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            arc = os.path.join(arc_prefix, rel_dir, name)
            if os.path.islink(full):
                add_link(zf, full, arc)
            else:
                zf.write(full, arc)


def add_link(zf, full, arc):
    info = zipfile.ZipInfo(arc.replace(os.sep, "/"))
    info.create_system = 3
    info.external_attr = (stat.S_IFLNK | 0o755) << 16
    zf.writestr(info, os.readlink(full))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", required=True)
    ap.add_argument("--ext", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--allow-empty", action="store_true")
    ap.add_argument("--extra", nargs="*", default=[],
                    help="files added at the zip root (install scripts, INSTALL.md); .sh/.command keep +x")
    a = ap.parse_args()

    found = []
    for dirpath, dirnames, _ in os.walk(a.build):
        for d in list(dirnames):
            if d.endswith(a.ext):
                found.append(os.path.join(dirpath, d))
                dirnames.remove(d)  # don't descend into the bundle
        # skip CMake/cargo scratch trees
        dirnames[:] = [d for d in dirnames if d not in ("CMakeFiles", "_deps", "target", ".git")]
    # The same bundle can be staged twice (e.g. per-config dirs): keep unique names, prefer shortest path.
    by_name = {}
    for p in sorted(found, key=len):
        by_name.setdefault(os.path.basename(p), p)
    if not by_name:
        print(f"::warning::no '*{a.ext}' bundle found under {a.build}")
        sys.exit(0 if a.allow_empty else 1)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as zf:
        for name, p in sorted(by_name.items()):
            print(f"adding {p}")
            add_tree(zf, p, "")
        for f in a.extra:
            info = zipfile.ZipInfo(os.path.basename(f))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            mode = 0o755 if f.endswith((".sh", ".command")) else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            with open(f, "rb") as fh:
                zf.writestr(info, fh.read())
            print(f"adding {f}")
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
