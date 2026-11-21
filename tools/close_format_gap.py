#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
P0-12: close the engine file-format verification gap.

WHY THIS EXISTS
---------------
The recovery + byte verification covers all 878,373 distinct (bootLBA, id)
nodes, but the *format* evidence was not uniform:

  * batches 4-7 (`recover` path) fed the bytes written by RecoverEngine back
    into SignatureDB::Match -> one verdict per recovered file (762,629 rows).
  * vol 63 (batches 1/2/4) got a full engine probe in P0-6: 9,855 of 9,856
    user nodes have formatReadOk=1 in engine_nodes_vol63.csv.
  * batch 3 (105,901 nodes on vol 209728638) has engine_result.csv with only
    four columns (id,status,targetPath,reason) -> NO format verdict at all.
  * P0-6's scan on vol 209728638 stopped after `--format-sample` rows, so
    861,555 of its rows are placeholders (agreement="noPayload") that were
    never probed.

So the claim "every file was format-matched by the engine" was true for
762,629 + 9,855 nodes and unproven for batch 3's 105,901.

WHAT THIS DOES
--------------
Re-probes exactly the uncovered ids through the engine, read-only:

    phoenix_headless_p07.exe scan --disk 2 --boot 209728638 \
        --ids batch_003_ids.txt --format-sample 0 --out engine_fmt_b3.csv

`scan` uses NtfsParser for discovery and RecoverEngine::ReadFileDataPrefix()
for the prefix decode, then SignatureDB::Match(AC) +
FormatPayloadScoreUtil::Score for the verdict. No writes to the source disk,
no recovery output, no extra disk space beyond the CSV.

Then it re-derives, from the CSV alone, one verdict per requested id and
cross-checks vol 63 against the P0-6 artifact.

Exit codes: 0 = gap closed and proven, 1 = gap still open, 2 = cannot run
(no elevation / disk unavailable), 3 = engine failed.
"""
import argparse
import collections
import csv
import json
import os
import shutil
import subprocess
import sys

ROOT = r"D:\dev\PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
BATCH_DIR = os.path.join(OUT, "batches")
ENGINE = os.path.join(ROOT, "build-gcc14", "tests", "phoenix_headless_p07.exe")
MSYS_BIN = r"C:\msys64\mingw64\bin"
MATCHER = "SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score"

# Batches whose per-row format verdicts are missing, with their volume.
GAP = [(3, 209728638)]
# Batches already covered by the `recover` path (bytes written -> Match).
COVERED_BY_RECOVER = [4, 5, 6, 7]
# Volume whose P0-6 scan was full (not sample-capped).
FULL_PROBE_VOLUME = 63


def ids_of(batch):
    p = os.path.join(BATCH_DIR, "batch_%03d_ids.txt" % batch)
    with open(p) as f:
        return [int(x) for x in f if x.strip()]


def can_open_disk(disk=2):
    """Read 512 bytes from LBA0 with GENERIC_READ only."""
    try:
        with open("\\\\.\\PhysicalDrive%d" % disk, "rb") as f:
            f.seek(0)
            return len(f.read(512)) == 512
    except OSError as e:
        return e


def run_scan(boot, ids_path, out_csv, disk=2, probe_bytes=4096):
    env = dict(os.environ)
    if os.path.isdir(MSYS_BIN):
        env["PATH"] = MSYS_BIN + os.pathsep + env.get("PATH", "")
    argv = [ENGINE, "scan", "--disk", str(disk), "--boot", str(boot),
            "--ids", ids_path, "--format-sample", "0",
            "--format-probe-bytes", str(probe_bytes), "--out", out_csv]
    print("[run]", " ".join(argv), flush=True)
    r = subprocess.run(argv, capture_output=True, text=True,
                       encoding="utf-8", errors="replace", env=env)
    sys.stderr.write(r.stderr[-4000:])
    return r.returncode


def read_verdicts(csv_path, wanted):
    """One verdict per requested id, from the engine's scan CSV."""
    seen = {}
    rows = 0
    with open(csv_path, newline="", encoding="utf-8", errors="replace") as f:
        rd = csv.DictReader(f)
        need = {"id", "size", "isDir", "isSystem", "formatAgreement",
                "formatReadOk", "formatMatcher", "engineExt"}
        missing = need - set(rd.fieldnames or [])
        if missing:
            raise RuntimeError("scan csv lacks columns %s" % sorted(missing))
        for r in rd:
            rid = int(r["id"])
            if rid not in wanted:
                continue
            rows += 1
            if rid in seen:
                continue
            seen[rid] = {
                "size": int(r["size"] or 0),
                "isDir": r["isDir"] == "1",
                "isSystem": r["isSystem"] == "1",
                "agreement": r["formatAgreement"] or "",
                "readOk": r["formatReadOk"] == "1",
                "engineExt": r["engineExt"] or "",
                "matcher": r["formatMatcher"] or "",
            }
    return seen, rows


def classify(v):
    """Was this node genuinely probed by SignatureDB?

    A node counts as probed when the engine reports readOk, OR when it is a
    node with nothing to probe (directory / system / zero length). Anything
    else - in particular agreement="noPayload" on a node that DOES have a
    size - means the probe never happened and must not be counted as a
    verdict. That distinction is the whole point: P0-6's 861,555 "noPayload"
    rows were unprobed placeholders, not engine conclusions.
    """
    if v["isDir"] or v["isSystem"] or v["size"] == 0:
        return "noPayloadLegitimate"
    if v["readOk"]:
        return "probed"
    return "UNPROBED"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk", type=int, default=2)
    ap.add_argument("--probe-bytes", type=int, default=4096)
    ap.add_argument("--reuse", action="store_true",
                    help="reuse an existing scan CSV instead of re-running")
    ap.add_argument("--out", default=os.path.join(OUT, "format_gap_verification.json"))
    args = ap.parse_args()

    report = {"generatedBy": "tools/close_format_gap.py",
              "matcher": MATCHER, "gapBatches": [b for b, _ in GAP],
              "volumes": {}, "errors": [], "sourceDiskWriteOperations": 0}

    # ---- 1. can we even touch the disk? --------------------------------
    probe = can_open_disk(args.disk)
    opened = probe is True
    report["diskOpen"] = {"disk": args.disk, "ok": opened,
                          "error": None if opened else str(probe)}
    if not opened:
        report["conclusion"] = ("BLOCKED: cannot open \\\\.\\PhysicalDrive%d "
                                "read-only (%s) - the remaining format probe "
                                "needs an elevated session" %
                                (args.disk, probe))
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, indent=2, ensure_ascii=False)
        print("BLOCKED:", probe)
        print("report ->", args.out)
        return 2

    # ---- 2. vol 63 is already fully probed by P0-6 ---------------------
    vol63 = os.path.join(OUT, "engine_nodes_vol%d.csv" % FULL_PROBE_VOLUME)
    if os.path.isfile(vol63):
        want = set()
        for b in (1, 2, 4):
            want |= set(ids_of(b))
        seen, _ = read_verdicts(vol63, want)
        cnt = collections.Counter(classify(v) for v in seen.values())
        missing = sorted(want - set(seen))
        report["volumes"][str(FULL_PROBE_VOLUME)] = {
            "source": os.path.basename(vol63),
            "requestedIds": len(want), "verdicts": len(seen),
            "missingIds": len(missing), "missingSample": missing[:10],
            "classes": dict(cnt),
            "probed": cnt["probed"], "unprobed": cnt["UNPROBED"],
        }
        if cnt["UNPROBED"] or missing:
            report["errors"].append(
                "vol63: %d unprobed + %d missing after P0-6 full probe"
                % (cnt["UNPROBED"], len(missing)))

    # ---- 3. batch 3: probe the uncovered ids through the engine --------
    for batch, boot in GAP:
        ids = ids_of(batch)
        want = set(ids)
        ids_path = os.path.join(BATCH_DIR, "batch_%03d_ids.txt" % batch)
        out_csv = os.path.join(OUT, "engine_fmt_b%03d_vol%d.csv" % (batch, boot))
        if not (args.reuse and os.path.isfile(out_csv)):
            if os.path.isfile(out_csv):
                os.remove(out_csv)
            rc = run_scan(boot, ids_path, out_csv, disk=args.disk,
                          probe_bytes=args.probe_bytes)
            if rc != 0 or not os.path.isfile(out_csv):
                report["errors"].append("engine scan rc=%d for batch %d"
                                        % (rc, batch))
                report["conclusion"] = "ENGINE SCAN FAILED"
                with open(args.out, "w", encoding="utf-8") as f:
                    json.dump(report, f, indent=2, ensure_ascii=False)
                print("ENGINE SCAN FAILED rc=%d" % rc)
                return 3

        seen, rows = read_verdicts(out_csv, want)
        cnt = collections.Counter(classify(v) for v in seen.values())
        agr = collections.Counter(v["agreement"] for v in seen.values()
                                  if v["readOk"])
        sig = sum(1 for v in seen.values() if v["readOk"] and v["engineExt"])
        missing = sorted(want - set(seen))
        report["volumes"]["batch_%03d" % batch] = {
            "volumeBootLBA": boot, "source": os.path.basename(out_csv),
            "csvRows": rows, "requestedIds": len(want),
            "verdicts": len(seen), "missingIds": len(missing),
            "missingSample": missing[:10], "classes": dict(cnt),
            "probed": cnt["probed"], "unprobed": cnt["UNPROBED"],
            "signatureHits": sig,
            "agreement": dict(agr),
            "allRowsSignatureClassified": all(
                MATCHER in v["matcher"] for v in seen.values()),
        }
        if missing:
            report["errors"].append("batch %d: %d requested ids absent from "
                                    "engine scan" % (batch, len(missing)))
        if cnt["UNPROBED"]:
            report["errors"].append("batch %d: %d ids still unprobed"
                                    % (batch, cnt["UNPROBED"]))
        if not report["volumes"]["batch_%03d" % batch][
                "allRowsSignatureClassified"]:
            report["errors"].append("batch %d: rows not labelled with %s"
                                    % (batch, MATCHER))

    # ---- 4. batches 4-7 stay covered by the recover path ---------------
    for b in COVERED_BY_RECOVER:
        p = os.path.join(BATCH_DIR, "batch_%03d_engine_result.csv" % b)
        rows = signed = 0
        with open(p, newline="", encoding="utf-8-sig", errors="replace") as f:
            rd = csv.DictReader(f)
            for r in rd:
                rows += 1
                if "SignatureDB" in (r.get("formatMatcher") or ""):
                    signed += 1
        report["volumes"]["batch_%03d" % b] = {
            "source": os.path.basename(p), "csvRows": rows,
            "signatureClassifiedRows": signed,
            "path": "recover: written bytes -> SignatureDB::Match"}
        if rows != signed:
            report["errors"].append("batch %d: %d/%d rows not signature-classified"
                                    % (b, signed, rows))

    # ---- 5. totals ----------------------------------------------------
    total_ids = 0
    total_probed = 0
    for key, v in report["volumes"].items():
        if key.startswith("vol"):
            total_ids += v["requestedIds"]
            total_probed += v["probed"]
    report["summary"] = {
        "requestedIdsVol63VolBatch3": total_ids,
        "probedByEngine": total_probed,
        "coveredByRecoverPath": sum(
            report["volumes"]["batch_%03d" % b]["csvRows"]
            for b in COVERED_BY_RECOVER),
    }
    report["conclusion"] = ("FORMAT EVIDENCE COMPLETE: every requested id has an "
                            "engine SignatureDB verdict"
                            if not report["errors"] else
                            "FORMAT EVIDENCE INCOMPLETE")
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)

    print("=== FORMAT GAP VERIFICATION ===")
    for key, v in report["volumes"].items():
        if "classes" in v:
            print("  %-16s requested=%s verdicts=%s probed=%s unprobed=%s "
                  "missing=%s" % (key, v["requestedIds"], v["verdicts"],
                                  v["probed"], v["unprobed"], v["missingIds"]))
        else:
            print("  %-16s rows=%s signed=%s" % (key, v["csvRows"],
                                                 v["signatureClassifiedRows"]))
    print("  summary:", json.dumps(report["summary"], ensure_ascii=False))
    for e in report["errors"]:
        print("  ERROR:", e)
    print("  conclusion:", report["conclusion"])
    print("report ->", args.out)
    return 0 if not report["errors"] else 1


if __name__ == "__main__":
    sys.exit(main())
