#!/usr/bin/env python3
"""Summarize the ENGINE-side verify CSVs for the full volume-1 recovery.

This is a PURE STATISTICS reader. It never discovers files, never decides
match/mismatch, and never touches the image. All verdicts come from
phoenix_headless `verify` (production SectorReader + NtfsParser runlist and
byte-for-byte comparison). This script only counts the verdicts that tool
already wrote, so the final report can quote exact numbers.

Usage:
    py -3 tools/summarize_v1_verify.py \
        --glob 'C:/PhoenixImage/engine_verify_v1_full_p*.csv' \
        --out-json C:/PhoenixImage/engine_verify_v1_summary.json
"""
import argparse
import csv
import glob
import json
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--glob", required=True, help="glob of engine verify CSV shards")
    ap.add_argument("--out-json", required=True)
    ap.add_argument("--samples", type=int, default=20, help="max non-match samples to keep")
    args = ap.parse_args()

    paths = sorted(glob.glob(args.glob))
    header_expected = ["id", "status", "targetPath", "reason", "bytes", "firstDiffOffset"]
    counts = {}
    matched_bytes = 0
    samples = []
    rows_total = 0
    seen_header = 0

    for p in paths:
        with open(p, "r", encoding="utf-8", newline="") as f:
            rd = csv.reader(f)
            try:
                head = next(rd)
            except StopIteration:
                print(f"WARN empty shard: {p}", file=sys.stderr)
                continue
            if head != header_expected:
                print(f"WARN unexpected header in {p}: {head}", file=sys.stderr)
            seen_header += 1
            for row in rd:
                if len(row) < 6:
                    continue
                rows_total += 1
                status = row[1]
                counts[status] = counts.get(status, 0) + 1
                try:
                    nbytes = int(row[4])
                    first_diff = int(row[5])
                except ValueError:
                    nbytes = first_diff = 0
                if status == "match":
                    matched_bytes += nbytes
                else:
                    if len(samples) < args.samples:
                        samples.append({
                            "id": row[0], "status": status, "reason": row[3],
                            "bytes": nbytes, "firstDiffOffset": first_diff,
                            "path": row[2], "shard": os.path.basename(p),
                        })

    summary = {
        "source": "phoenix_headless verify (engine: SectorReader + NtfsParser runlist)",
        "shard_files": len(paths),
        "shard_headers": seen_header,
        "rows": rows_total,
        "counts": counts,
        "matched_bytes": matched_bytes,
        "matched_gib": round(matched_bytes / (1024 ** 3), 3),
        "all_match": rows_total > 0 and counts.get("match", 0) == rows_total,
        "non_match_samples": samples,
    }

    out_dir = os.path.dirname(os.path.abspath(args.out_json))
    if out_dir and not os.path.isdir(out_dir):
        os.makedirs(out_dir, exist_ok=True)
    with open(args.out_json, "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=2)

    print(f"shards={len(paths)} rows={rows_total} all_match={summary['all_match']}")
    for k in sorted(counts):
        print(f"  {k:10s} {counts[k]}")
    print(f"matched_bytes={matched_bytes} ({summary['matched_gib']} GiB)")
    if samples:
        print(f"non-match samples (up to {len(samples)}):")
        for s in samples[:10]:
            print(f"  id={s['id']} {s['status']} reason={s['reason']} "
                  f"firstDiff={s['firstDiffOffset']} path={s['path']}")

    # Non-zero exit if any verdict is not a clean match, so this can gate a run.
    return 0 if summary["all_match"] else 1


if __name__ == "__main__":
    sys.exit(main())
