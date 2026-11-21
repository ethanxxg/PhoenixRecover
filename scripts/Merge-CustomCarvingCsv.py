#!/usr/bin/env python3
"""Merge per-image custom carving CSV shards into one report."""

from __future__ import annotations

import argparse
import csv
import glob
import os


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo-root", default=os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))
    parser.add_argument("--temp-dir", default="")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    repo = args.repo_root
    temp = args.temp_dir or os.path.join(os.environ.get("TEMP", "."), "phoenix_custom_carve")
    out = args.out or os.path.join(repo, "docs", "reports", "custom_carving_results.csv")

    shards = sorted(glob.glob(os.path.join(temp, "carving-*.dd.csv")))
    if not shards:
        print(f"No shards in {temp}")
        return 1

    rows: list[dict] = []
    for path in shards:
        with open(path, encoding="utf-8") as f:
            rows.extend(list(csv.DictReader(f)))

    rows.sort(key=lambda r: (r.get("image", ""), int(r.get("offset", "0") or 0)))
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    if rows:
        with open(out, "w", newline="", encoding="utf-8") as f:
            w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
            w.writeheader()
            w.writerows(rows)
    print(f"Merged {len(shards)} shards -> {out} ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
