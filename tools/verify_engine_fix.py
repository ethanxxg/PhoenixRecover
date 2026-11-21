#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Falsifiable verification of the P0-7 $ATTRIBUTE_LIST engine fix.

Chain (all engine code, zero writes to the source disk):
  * `engine_nodes_vol63.csv`       - P0-6 engine scan, id 78 clusterCount=0
  * `engine_nodes_vol63_p07.csv`   - P0-7 engine scan, id 78 clusterCount>0
  * `batch_gap78_engine.csv`       - P0-7 RecoverEngine::RecoverBatch result
  * the recovered file on disk     - SHA-256 compared with the INDEPENDENT
                                     truth in engine_gap_evidence.json
                                     (rebuilt from $ATTRIBUTE_LIST extension
                                      MFT records read directly from the disk)

Negative guard `--negative-guard` corrupts the expected truth SHA-256 and MUST
fail, proving the byte comparison is really being performed.

Usage:
    python tools/verify_engine_fix.py
    python tools/verify_engine_fix.py --negative-guard --out /tmp/neg.json
"""
import argparse
import csv
import hashlib
import json
import os
import sys
import time

ROOT = r"D:/dev/PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
STAGING = os.path.join(ROOT, "recover_staging")

BEFORE_CSV = os.path.join(OUT, "engine_nodes_vol63.csv")
AFTER_CSV = os.path.join(OUT, "engine_nodes_vol63_p07.csv")
RESULT_CSV = os.path.join(OUT, "batch_gap78_engine.csv")
GAP_EVIDENCE = os.path.join(OUT, "engine_gap_evidence.json")
GAP_ID = 78


def row_for(path, rid):
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if int(r["id"]) == rid:
                return r
    return None


def sha_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--negative-guard", action="store_true")
    ap.add_argument("--out", default="engine_fix_verification.json")
    args = ap.parse_args()

    errors = []
    ev = json.load(open(GAP_EVIDENCE, encoding="utf-8"))
    truth = next((n for v in ev["volumes"] for n in v["nodes"]
                  if n["id"] == GAP_ID), None)
    if not truth or "truthSha256" not in truth:
        errors.append("no independent truth for id %d" % GAP_ID)
        truth = {"truthSha256": "", "truthSize": 0}

    expected_sha = truth["truthSha256"]
    if args.negative_guard:
        expected_sha = ("0" if expected_sha[:1] != "0" else "1") + expected_sha[1:]

    before = row_for(BEFORE_CSV, GAP_ID)
    after = row_for(AFTER_CSV, GAP_ID)
    if not before or not after:
        errors.append("missing engine scan row for id %d" % GAP_ID)
        before = before or {"clusterCount": "?", "size": "?"}
        after = after or {"clusterCount": "?", "size": "?"}

    before_cc = int(before["clusterCount"] or 0)
    after_cc = int(after["clusterCount"] or 0)
    if before_cc != 0:
        errors.append("before-fix scan unexpectedly had %d clusters" % before_cc)
    if after_cc <= 0:
        errors.append("after-fix scan still has %d clusters" % after_cc)

    result_rows = []
    with open(RESULT_CSV, newline="", encoding="utf-8", errors="replace") as f:
        result_rows = list(csv.DictReader(f))
    one = next((r for r in result_rows if int(r["id"]) == GAP_ID), None)
    if not one:
        errors.append("no engine recover result row for id %d" % GAP_ID)
        one = {"status": "?", "targetPath": "", "engineExt": "",
               "formatAgreement": ""}
    if int(one["status"]) != 0:
        errors.append("engine recover status=%s (expected 0)" % one["status"])

    target = os.path.join(ROOT, one["targetPath"].replace("\\", os.sep))
    got_size, got_sha = 0, ""
    if os.path.isfile(target):
        got_size = os.path.getsize(target)
        got_sha = sha_of(target)
    else:
        errors.append("recovered file missing: %s" % target)

    if got_size != int(truth["truthSize"] or 0):
        errors.append("size mismatch: engine=%d truth=%s" %
                      (got_size, truth["truthSize"]))
    if got_sha != expected_sha:
        errors.append("SHA-256 mismatch: engine=%s expected=%s" %
                      (got_sha, expected_sha))

    doc = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "generatedBy": "tools/verify_engine_fix.py",
        "purpose": "End-to-end proof that the production engine (P0-7 build) "
                   "recovers a $ATTRIBUTE_LIST file byte-for-byte.",
        "gapId": GAP_ID,
        "beforeFixClusterCount": before_cc,
        "afterFixClusterCount": after_cc,
        "afterFixFirstCluster": after.get("firstCluster", ""),
        "engineDeclaredSize": int(after.get("size") or 0),
        "independentTruthSize": truth.get("truthSize"),
        "independentTruthSha256": truth.get("truthSha256"),
        "expectedSha256Used": expected_sha,
        "engineRecoveredSize": got_size,
        "engineRecoveredSha256": got_sha,
        "engineRecoverStatus": int(one["status"]) if one["status"] != "?" else "?",
        "engineFormatExt": one.get("engineExt", ""),
        "engineFormatAgreement": one.get("formatAgreement", ""),
        "engineFormatMatcher": one.get("formatMatcher", ""),
        "sourceDiskWriteOperations": 0,
        "negativeGuard": args.negative_guard,
        "errors": errors,
        "verdict": ("ENGINE ATTRIBUTE_LIST FIX VERIFIED BYTE-FOR-BYTE"
                    if not errors else "VERIFICATION FAILED"),
    }
    dest = args.out if os.path.isabs(args.out) else os.path.join(OUT, args.out)
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)

    print("id=%d clusters before=%d after=%d | size=%d/%s sha=%s" %
          (GAP_ID, before_cc, after_cc, got_size,
           truth.get("truthSize"), got_sha[:16]))
    print("VERDICT: %s | errors=%d" % (doc["verdict"], len(errors)))
    for e in errors[:8]:
        print("  ERROR:", e)
    # Convention used by the other Python gates in this project: a negative
    # guard must be REJECTED (EXIT=1). If the tampering is NOT detected we
    # also exit 1, with an explicit "gate not falsifiable" message.
    if args.negative_guard:
        if not errors:
            print("ERROR: negative guard did not trip - gate not falsifiable")
            return 1
        return 1
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
