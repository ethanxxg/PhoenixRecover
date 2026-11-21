#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P0-6 verification: prove that ENGINE DISCOVERY (not the independent oracle)
covers every id the real-disk recovery plan asks the engine to restore, and
that file-format verdicts came from the engine's SignatureDB.

Everything is recomputed from the raw CSV/JSON artifacts. No self-declared
"verified" flag is trusted.

Usage:
  python tools/verify_engine_scan.py
  python tools/verify_engine_scan.py --negative-guard   # MUST exit 1
"""
import argparse
import csv
import glob
import json
import os
import re
import sys

ROOT = r"D:\dev\PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
VOLUMES = [63, 209728638]
DISCOVERY_ENGINE = "NtfsParser::ScanMetadata/GetAllFiles"
FORMAT_MATCHER = "SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score"


def engine_csv(boot):
    return os.path.join(OUT, "engine_nodes_vol%d.csv" % boot)


def oracle_csv(boot):
    return os.path.join(OUT, "candidates_vol%d.csv" % boot)


def read_engine(boot):
    path = engine_csv(boot)
    if not os.path.exists(path):
        raise FileNotFoundError("engine scan artifact missing: %s" % path)
    ids, fmt = set(), {
        "userNodes": 0, "signatureHits": 0, "prefixReadOk": 0,
        "agreement": {}, "probedRows": 0,
    }
    with open(path, newline="", encoding="utf-8") as f:
        rd = csv.DictReader(f)
        required = {"id", "isDir", "isSystem", "size", "engineExt",
                    "formatAgreement", "formatReadOk"}
        missing = required - set(rd.fieldnames or [])
        if missing:
            raise RuntimeError("engine csv lacks columns %s — stale artifact?"
                               % sorted(missing))
        for r in rd:
            if r["isDir"] == "1" or r["isSystem"] == "1":
                continue
            if int(r["size"]) <= 0:
                continue
            ids.add(int(r["id"]))
            fmt["userNodes"] += 1
            if r["engineExt"]:
                fmt["signatureHits"] += 1
            if r["formatReadOk"] == "1":
                fmt["prefixReadOk"] += 1
            if r["formatAgreement"]:
                key = r["formatAgreement"]
                fmt["agreement"][key] = fmt["agreement"].get(key, 0) + 1
                fmt["probedRows"] += 1
    return ids, fmt


def read_oracle(boot):
    path = oracle_csv(boot)
    if not os.path.exists(path):
        return set()
    ids = set()
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["isSystem"] == "1" or r["isDir"] == "1":
                continue
            if int(r["size"]) <= 0:
                continue
            ids.add(int(r["id"]))
    return ids


def read_signature_count():
    """Extract the built-in signature count the ENGINE actually loaded."""
    counts = {}
    for p in sorted(glob.glob(os.path.join(OUT, "engine_scan_vol*.log"))):
        with open(p, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
        m = re.search(r"SignatureDB built-in signatures=(\d+)", text)
        if m:
            counts[os.path.basename(p)] = int(m.group(1))
        n = re.search(r"NtfsParser::ScanMetadata/GetAllFiles nodes=(\d+)", text)
        if n:
            counts[os.path.basename(p) + ":nodes"] = int(n.group(1))
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--negative-guard", action="store_true",
                    help="simulate one undiscovered id; the gate MUST fail")
    ap.add_argument("--out", default="engine_scan_coverage.json")
    args = ap.parse_args()

    with open(os.path.join(OUT, "plan.json"), "r", encoding="utf-8") as f:
        plan = json.load(f)

    volumes, batch_rows = {}, []
    all_ok = True
    for boot in VOLUMES:
        eng, fmt = read_engine(boot)
        orc = read_oracle(boot)
        volumes[str(boot)] = {
            "engineDiscoveredUserNodes": len(eng),
            "oracleUserCandidates": len(orc),
            "oracleRole": "independent byte-level verification truth only",
            "intersection": len(eng & orc),
            "engineOnly": len(eng - orc),
            "oracleOnly": len(orc - eng),
            "oracleOnlySample": sorted(orc - eng)[:10],
            "engineFormat": fmt,
            "formatMatcher": FORMAT_MATCHER,
        }

    for b in plan["batches"]:
        boot = b["volumeBootLBA"]
        eng, _ = read_engine(boot)
        ids = list(b["ids"])
        if args.negative_guard and b["no"] == plan["batches"][0]["no"]:
            ids = ids + [999999999999]     # never discovered by the engine
        missing = [i for i in ids if i not in eng]
        ok = not missing
        all_ok = all_ok and ok
        batch_rows.append({
            "batch": b["no"], "volumeBootLBA": boot,
            "requestedIds": len(ids), "coveredByEngine": len(ids) - len(missing),
            "missingIds": len(missing), "missingSample": missing[:10],
            "discoveryGate": "PASS" if ok else "FAIL",
        })

    sig_counts = read_signature_count()
    conclusion = ("ENGINE DISCOVERY COVERS ALL %d PLAN IDS"
                  % sum(r["requestedIds"] for r in batch_rows)
                  if all_ok else "ENGINE DISCOVERY GATE FAILED")
    result = {
        "generatedBy": "tools/verify_engine_scan.py (recomputed from raw CSV/JSON)",
        "discoveryEngine": DISCOVERY_ENGINE,
        "formatMatcher": FORMAT_MATCHER,
        "candidateSource": "engine",
        "oracleRole": "independent byte-level verification truth only",
        "engineSignatureCountsFromLogs": sig_counts,
        "volumes": volumes,
        "batches": batch_rows,
        "allBatchesCoveredByEngine": all_ok,
        "conclusion": conclusion,
    }
    path = os.path.join(OUT, args.out)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)

    print("conclusion:", conclusion)
    for r in batch_rows:
        print("  batch %-2d vol=%-10d requested=%-8d covered=%-8d missing=%d %s"
              % (r["batch"], r["volumeBootLBA"], r["requestedIds"],
                 r["coveredByEngine"], r["missingIds"], r["discoveryGate"]))
    for boot, v in volumes.items():
        print("  vol %-10s engineNodes=%-8d oracleCandidates=%-8d "
              "engineOnly=%-6d oracleOnly=%-6d signatureHits=%d"
              % (boot, v["engineDiscoveredUserNodes"], v["oracleUserCandidates"],
                 v["engineOnly"], v["oracleOnly"],
                 v["engineFormat"]["signatureHits"]))
    print("artifact:", path)
    print("engineSignatureCountsFromLogs:", json.dumps(sig_counts))
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
