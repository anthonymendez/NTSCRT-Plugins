#!/usr/bin/env python3
"""Build-time workaround for upstream NTSCRT's crt-smoke on CI (golden workflow).

Swift 6.1-6.2 (Xcode 16.4-26.3 on GitHub's macOS runners) give up on
DigitalVideoRenderer.capture(): "the compiler is unable to type-check this
expression in reasonable time" at the `bgra.withUnsafeBufferPointer { ... }`
nest, because the per-pixel closure inside it (several untyped float literals,
`min(255, ...)`) is solved together with the whole nest. This edits the
WORKING TREE of the checked-out submodule on the runner only (never committed):

  1. the per-row body of the first `DispatchQueue.concurrentPerform(iterations: h) { y in`
     in capture() becomes a local `func`, which Swift type-checks on its own;
  2. `let shade = 1.3 - 0.6 * min(255, ...)` gets explicit Float types.

Same arithmetic, same evaluation order: the rendered pixels are unchanged.
Idempotent; fails loudly if the source no longer looks as expected.

Usage: patch_upstream_build.py <upstream checkout>
"""
import os
import sys

MARK = "// ntscrt-plugins CI: type-checker workaround"


def main():
    path = os.path.join(sys.argv[1], "Sources", "CrtCodec", "DigitalVideoRenderer.swift")
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    if any(MARK in l for l in lines):
        print("already patched")
        return
    start = next((i for i, l in enumerate(lines) if "static func capture(" in l), None)
    if start is None:
        sys.exit("error: capture() not found")
    head = "DispatchQueue.concurrentPerform(iterations: h) { y in"
    i = next((k for k in range(start, len(lines)) if lines[k].strip() == head), None)
    if i is None:
        sys.exit("error: concurrentPerform closure not found in capture()")
    indent = lines[i][: len(lines[i]) - len(lines[i].lstrip())]
    # closing brace: first later line that is exactly `indent + "}"`
    j = next((k for k in range(i + 1, len(lines)) if lines[k] == indent + "}"), None)
    if j is None:
        sys.exit("error: end of the concurrentPerform closure not found")
    lines[i] = f"{indent}{MARK}\n{indent}func captureRow(_ y: Int) {{"
    lines[j] = f"{indent}}}\n{indent}DispatchQueue.concurrentPerform(iterations: h, execute: captureRow)"
    n = 0
    for k in range(i, j + 1):
        old = "let shade = 1.3 - 0.6 * min(255, 0.299 * r + 0.587 * gr + 0.114 * b) / 255"
        if old in lines[k]:
            lines[k] = lines[k].replace(
                old,
                "let shade: Float = (1.3 as Float) - (0.6 as Float) * Swift.min(255 as Float, "
                "(0.299 as Float) * r + (0.587 as Float) * gr + (0.114 as Float) * b) / (255 as Float)")
            n += 1
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    print(f"patched {path}: lines {i + 1}-{j + 1} as a local func, {n} shade expression(s) typed")


if __name__ == "__main__":
    main()
