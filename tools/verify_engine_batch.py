#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Generate an engine-byte-verification record for a completed batch.

The independent oracle's candidate CSV was produced before the NTFS ADS fix
(P0-5), so batches 4-6 will keep reporting the same FALSE mismatches on files
with a Zone.Identifier stream. The batch plan / candidate CSV / truth files
are historical evidence and must NOT be rewritten. Instead, for every batch
with anomalies this tool:

  1. isolates ids whose engine rows are match=0, status=0 and have NO other
     matching row for the same id (real false-mismatch candidates);
  2. re-parses those MFT records DIRECTLY from the source disk with the
     ADS-safe oracle (mft_oracle.py verify-ids, read-only);
  3. compares the corrected truth against the ARCHIVED engine SHA-256/size in
     batch_NNN_hashes.csv (works even after staged files were released);
  4. requires a full retry record for every failed id, when any;
  5. writes batch_NNN_engine_verification.json with the same schema that
     tools/verify_archive.py re-derives (it never trusts this file's verdict).

Usage:
    python tools/verify_engine_batch.py --batch 3 --dry-run   # audit only
    python tools/verify_engine_batch.py --batch 4
    python tools/verify_engine_batch.py --all-completed

Requires the elevated read-only physical-disk access used by mft_oracle.py.
"""
import argparse
import csv
import json
import os
import subprocess
import sys
import tempfile
import time

ROOT = r"D:/dev/PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
BATCH_DIR = os.path.join(OUT, "batches")
ORACLE = os.path.join(ROOT, "tools", "mft_oracle.py")


def jload(path, default=None):
    if not os.path.isfile(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def hash_rows(no):
    p = os.path.join(BATCH_DIR, "batch_%03d_hashes.csv" % no)
    rows = []
    with open(p, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            rows.append(r)
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch", type=int)
    ap.add_argument("--all-completed", action="store_true")
    ap.add_argument("--dry-run", action="store_true",
                    help="audit and print, but do not write the record")
    ap.add_argument("--out", help="override output path (single batch only)")
    args = ap.parse_args()

    plan = jload(os.path.join(OUT, "plan.json"), {})
    cp = jload(os.path.join(OUT, "checkpoint.json"), {})
    boot_by_batch = {b["no"]: b["volumeBootLBA"] for b in plan.get("batches", [])}
    if args.all_completed:
        # Historical oracle-candidate batches ONLY. Batches >= 4 are engine-
        # driven and must use verify_engine_plan_batch.py; an old-schema
        # record for them could let verify_archive.py's ev_valid() accept a
        # misleading "verified" file instead of the volume-scoped P0-7 gate.
        targets = sorted(n for n in cp.get("completed", []) if n <= 3)
    elif args.batch:
        if args.batch >= 4:
            ap.error("batch %d is engine-driven; use "
                     "tools/verify_engine_plan_batch.py instead" % args.batch)
        targets = [args.batch]
    else:
        ap.error("specify --batch N or --all-completed")
    if len(targets) > 1 and args.out:
        ap.error("--out is only valid with a single --batch")

    overall = 0
    for no in targets:
        tag = "batch_%03d" % no
        rec = jload(os.path.join(BATCH_DIR, tag + ".json"))
        if not rec:
            print("%s: record missing, SKIP" % tag)
            overall |= 1
            continue
        s = rec.get("stats", {})
        rows = hash_rows(no)
        matched_ids = {r["id"] for r in rows if r["match"] == "1"}
        mismatch_rows = [r for r in rows
                         if r["match"] == "0" and r["status"] == "0"
                         and r["id"] not in matched_ids]
        by_id = {}
        for r in mismatch_rows:
            by_id.setdefault(r["id"], []).append(r)

        anomalies = bool(s.get("mismatched") or s.get("failed")
                         or s.get("matched") != s.get("requested"))
        if not anomalies:
            print("%s: no anomalies, verification record not needed" % tag)
            continue

        boot = boot_by_batch.get(no, rec.get("volumeBootLBA"))
        if not boot:
            print("%s: ERROR no volumeBootLBA" % tag)
            overall |= 1
            continue

        checks = []
        if by_id:
            with tempfile.TemporaryDirectory() as td:
                ids_file = os.path.join(td, "ids.txt")
                truth_file = os.path.join(td, "truth.json")
                with open(ids_file, "w", encoding="utf-8") as f:
                    f.write("\n".join(sorted(by_id, key=int)) + "\n")
                cmd = [sys.executable, ORACLE, "verify-ids", "--boot", str(boot),
                       "--ids", ids_file, "--out", truth_file]
                r = subprocess.run(cmd, capture_output=True, text=True)
                if r.returncode != 0:
                    print("%s: ERROR verify-ids failed: %s" %
                          (tag, (r.stderr or r.stdout)[-300:]))
                    overall |= 1
                    continue
                truth = jload(truth_file, {})
            for rid, t in sorted(truth.items(), key=lambda x: int(x[0])):
                erows = by_id.get(rid, [])
                e = erows[0] if erows else {}
                ok = (len(erows) == 1
                      and e.get("engineSha256") == t.get("sha256")
                      and int(e.get("engineSize", "-1")) == int(t.get("expectedSize", -2))
                      and bool(t.get("adsSkipped")))
                checks.append({
                    "id": int(rid), "expectedSize": t.get("expectedSize"),
                    "engineSize": int(e.get("engineSize", "-1")),
                    "oracleSha256": t.get("sha256", ""),
                    "engineSha256": e.get("engineSha256", ""),
                    "adsSkipped": t.get("adsSkipped", []),
                    "match": bool(ok),
                })
        good = sum(1 for c in checks if c["match"])

        retry = jload(os.path.join(BATCH_DIR, tag + "_retry.json"), {})
        rs = retry.get("stats", {"matched": 0, "mismatched": 0,
                                 "failed": 0, "missingOracle": 0})
        if s.get("failed") and (rs.get("matched") != s.get("failed")
                                or rs.get("mismatched") or rs.get("failed")):
            print("%s: ERROR %d failures not fully covered by a retry record" %
                  (tag, s.get("failed")))
            overall |= 1

        effective = int(s.get("matched", 0)) + good + int(rs.get("matched", 0))
        eff = {
            "requested": s.get("requested"),
            "directMatched": s.get("matched"),
            "correctedOracleMatched": good,
            "retryMatched": rs.get("matched", 0),
            "effectiveVerified": effective,
            "unresolved": int(s.get("requested", 0)) - effective,
            "idCollisionNoiseRows": s.get("duplicateIdNodes", 0),
        }
        collisions = rec.get("idCollisions", [])
        record = {
            "batch": no, "kind": "engine-byte-verification",
            "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "generatedBy": "tools/verify_engine_batch.py",
            "purpose": "Explain archived anomalies without modifying any record or the engine.",
            "originalStats": s,
            "originalRecordsUnmodified": True,
            "findings": {
                "engineOutputCollision": {
                    "failed": s.get("failed", 0),
                    "resolution": "headless output isolation + retry" if s.get("failed") else "not needed",
                    "retryRecord": tag + "_retry.json" if s.get("failed") else None,
                    "retryStats": rs,
                },
                "oracleAdsFalseMismatch": {
                    "count": len(checks),
                    "cause": "mft_oracle.py candidate CSV ignored $DATA nameLength (Zone.Identifier ADS)",
                    "resolution": "ADS-safe verify-ids re-parsed each MFT record directly from disk",
                    "exactSha256Match": "%d/%d" % (good, len(checks)),
                },
                "adsNodeIdCollision": {
                    "count": s.get("duplicateIdNodes", 0),
                    "cause": "adsNode.id = (recordIndex << 4) | (adsIdx+1) can equal another recordIndex",
                    "ids": [c.get("id") for c in collisions],
                },
            },
            "correctedOracleChecks": checks,
            "effectiveStats": eff,
            "conclusion": ("ENGINE BYTE RECOVERY VERIFIED FOR ALL %d REQUESTED IDS"
                           % int(s.get("requested", 0))) if eff["unresolved"] == 0
                          else "UNRESOLVED ANOMALIES REMAIN",
            "sourceDiskWriteOperations": 0,
        }
        ok = (eff["unresolved"] == 0 and good == len(checks)
              and len(checks) == int(s.get("mismatched", 0)))
        print("%s: mismatches=%d corrected=%d retry=%d effective=%d/%d %s" %
              (tag, s.get("mismatched", 0), good, rs.get("matched", 0),
               effective, s.get("requested", 0), "OK" if ok else "INCOMPLETE"))
        if not ok:
            overall |= 1
            continue
        if args.dry_run:
            print("   dry-run: record not written")
            continue
        dest = args.out or os.path.join(BATCH_DIR, tag + "_engine_verification.json")
        with open(dest, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=2, ensure_ascii=False)
        print("   -> %s" % dest)
    return overall


if __name__ == "__main__":
    sys.exit(main())
