#!/usr/bin/env python3
"""Maintain fxplug/param_ids.json: stable FxPlug parameter IDs for schema entries.

FxPlug stores parameter values in Final Cut / Motion projects by numeric ID,
so an ID must never change or be reused once released. This file is
APPEND-ONLY: new schema params/groups get the next free ID in [100, 4999];
removed ones keep their ID (marked in "retired") so it is never reassigned.

  update_param_ids.py [--schema core/params/schema.json] [--map fxplug/param_ids.json] [--check]

--check exits 1 (without writing) when entries are missing, for CI.
"""
import argparse
import json
import os
import sys

FIRST, LAST = 100, 4999
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))


def keys_from_schema(schema):
    keys = ["group:" + g["id"] for g in schema.get("groups", []) if g.get("id")]
    keys += [p["id"] for p in schema.get("params", []) if p.get("id")]
    return keys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--schema", default=os.path.join(ROOT, "core", "params", "schema.json"))
    ap.add_argument("--map", default=os.path.join(ROOT, "fxplug", "param_ids.json"))
    ap.add_argument("--check", action="store_true")
    a = ap.parse_args()

    with open(a.schema, encoding="utf-8") as f:
        schema = json.load(f)
    if os.path.exists(a.map):
        with open(a.map, encoding="utf-8") as f:
            m = json.load(f)
    else:
        m = {"version": 1, "ids": {}, "retired": []}
    ids = m.setdefault("ids", {})
    retired = m.setdefault("retired", [])

    used = set(ids.values())
    if len(used) != len(ids):
        sys.exit("error: duplicate ids in " + a.map)
    bad = {k: v for k, v in ids.items() if not (FIRST <= v <= LAST)}
    if bad:
        sys.exit(f"error: ids out of range [{FIRST},{LAST}]: {bad}")

    keys = keys_from_schema(schema)
    missing = [k for k in keys if k not in ids]
    gone = [k for k in ids if k not in keys and k not in retired]
    if a.check:
        for k in missing:
            print(f"::warning::fxplug/param_ids.json has no id for '{k}' (run fxplug/tools/update_param_ids.py)")
        for k in gone:
            print(f"note: '{k}' is no longer in the schema (its id stays reserved)")
        sys.exit(1 if missing else 0)

    nxt = max(used | {FIRST - 1}) + 1
    for k in missing:
        if nxt > LAST:
            sys.exit("error: FxPlug id range exhausted")
        ids[k] = nxt
        used.add(nxt)
        nxt += 1
    for k in gone:
        retired.append(k)
    m["note"] = ("APPEND-ONLY. FxPlug parameter IDs saved in FCP/Motion projects. Never edit or reuse an id; "
                 "regenerate with fxplug/tools/update_param_ids.py.")
    with open(a.map, "w", encoding="utf-8", newline="\n") as f:
        json.dump(m, f, indent=2)
        f.write("\n")
    print(f"{len(missing)} added, {len(gone)} retired, {len(ids)} total -> {a.map}")


if __name__ == "__main__":
    main()
