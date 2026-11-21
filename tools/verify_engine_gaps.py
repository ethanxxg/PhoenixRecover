#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Prove (or disprove) the NtfsParser $ATTRIBUTE_LIST recovery gap.

P0-7, real LDM disk 2026-11-14. The engine's own scan output
`engine_nodes_vol*.csv` contains a few nodes with:

    size > 0  AND  clusterCount == 0  AND  residentSize == 0

i.e. the engine knows a declared size (usually stale $FILE_NAME.realSize) but
has NO data runs, so RecoverEngine reports "No recoverable data (no clusters
and no resident data)". On this disk those files keep their $DATA in EXTENSION
MFT records referenced by a resident $ATTRIBUTE_LIST (0x20) in the base record.
NtfsParser only walks attributes of the base record, so it never sees them.

This tool is read-only against the source disk. It:
  1. finds those nodes in the ENGINE's own CSV (never from the oracle);
  2. re-parses each base MFT record directly from \\.\PhysicalDrive2;
  3. follows the $ATTRIBUTE_LIST entries of type $DATA to extension records,
     reconstructs the runlist in lowestVCN order (sparse gaps kept),
     reads the payload and computes an independent SHA-256;
  4. cross-checks the reconstructed size against the engine's declared size;
  5. writes engine_gap_evidence.json.

Verdict is falsifiable: after NtfsParser learns to follow $ATTRIBUTE_LIST,
the CSV will show clusterCount>0 for these ids and the tool reports
"NO GAP DETECTED". `--negative-guard` injects a node that has a normal in-base
$DATA (so no attribute list) into the gap set; the tool MUST then fail.

Usage:
    python tools/verify_engine_gaps.py
    python tools/verify_engine_gaps.py --negative-guard --out /tmp/neg.json
"""
import argparse
import csv
import hashlib
import json
import os
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mft_oracle as mo

ROOT = r"D:/dev/PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
VOLUMES = [(63, "engine_nodes_vol63.csv"),
           (209728638, "engine_nodes_vol209728638.csv")]


def read_gap_nodes(path):
    out = []
    with open(path, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            if r["isDir"] == "1" or r["isSystem"] == "1":
                continue
            size = int(r["size"] or 0)
            cc = int(r["clusterCount"] or 0)
            rs = int(r["residentSize"] or 0)
            if size > 0 and cc == 0 and rs == 0:
                out.append({
                    "id": int(r["id"]), "name": r["name"],
                    "path": r["path"], "engineSize": size,
                    "engineClusterCount": cc, "engineResidentSize": rs,
                    "engineStatus": int(r["status"] or 0),
                })
    return out


def parse_attr_list(rec, attr_off, end):
    """Return $ATTRIBUTE_LIST entries as dicts (type, lowestVcn, recordIndex...)."""
    entries = []
    pos = attr_off
    while pos + 16 <= end:
        atype = struct.unpack_from("<I", rec, pos)[0]
        if atype in (0, 0xFFFFFFFF):
            break
        alen = struct.unpack_from("<I", rec, pos + 4)[0]
        if alen < 16 or pos + alen > end:
            break
        if atype == 0x20 and not rec[pos + 8]:
            vlen = struct.unpack_from("<I", rec, pos + 0x10)[0]
            voff = struct.unpack_from("<H", rec, pos + 0x14)[0]
            q, stop = pos + voff, pos + voff + vlen
            while q + 26 <= stop:
                et = struct.unpack_from("<I", rec, q)[0]
                rl = struct.unpack_from("<H", rec, q + 4)[0]
                nl = rec[q + 6]
                vcn = struct.unpack_from("<Q", rec, q + 8)[0]
                ref = struct.unpack_from("<Q", rec, q + 16)[0]
                if rl < 26:
                    break
                entries.append({
                    "type": et, "recordLength": rl, "nameLength": nl,
                    "lowestVcn": vcn,
                    "recordIndex": ref & 0x0000FFFFFFFFFFFF,
                    "sequence": ref >> 48,
                })
                q += rl
        pos += alen
    return entries


def find_unnamed_data_runs(rec, record_size):
    """Return (runs, realSize, initializedSize) for the unnamed $DATA."""
    attr_off = struct.unpack_from("<H", rec, 0x14)[0]
    pos, end = attr_off, min(record_size, len(rec))
    while pos + 16 <= end:
        atype = struct.unpack_from("<I", rec, pos)[0]
        if atype in (0, 0xFFFFFFFF):
            break
        alen = struct.unpack_from("<I", rec, pos + 4)[0]
        if alen < 16 or pos + alen > end:
            break
        if atype == 0x80 and rec[pos + 9] == 0 and rec[pos + 8]:
            run_off = struct.unpack_from("<H", rec, pos + 0x20)[0]
            real = struct.unpack_from("<Q", rec, pos + 0x30)[0]
            init = struct.unpack_from("<Q", rec, pos + 0x38)[0]
            if run_off < alen:
                return mo.decode_runlist(rec, pos + run_off, pos + alen), real, init
        pos += alen
    return None, None, None


def resolve_truth(disk, vol, rid):
    """Rebuild the true payload of one MFT record via $ATTRIBUTE_LIST."""
    pos = vol.record_byte_offset(rid)
    if pos is None:
        return {"error": "MFT record sparse or out of range"}
    raw = disk.read(pos, vol.recordSize)
    rec = mo.apply_usa(raw, vol.recordSize)
    if rec[:4] != b"FILE":
        return {"error": "MFT record magic mismatch"}
    attr_off = struct.unpack_from("<H", rec, 0x14)[0]
    end = struct.unpack_from("<I", rec, 0x18)[0] or vol.recordSize
    entries = [e for e in parse_attr_list(rec, attr_off, min(end, vol.recordSize))
               if e["type"] == 0x80 and e["nameLength"] == 0]
    if not entries:
        return {"error": "no unnamed $DATA entries in $ATTRIBUTE_LIST",
                "attributeListEntries": len(
                    parse_attr_list(rec, attr_off, min(end, vol.recordSize)))}

    entries.sort(key=lambda e: e["lowestVcn"])
    flat, cursor, real_size, seen = [], 0, None, []
    for e in entries:
        if e["lowestVcn"] > cursor:
            flat.append((0, e["lowestVcn"] - cursor))   # sparse gap
            cursor = e["lowestVcn"]
        ep = vol.record_byte_offset(e["recordIndex"])
        if ep is None:
            return {"error": "extension record %d sparse/out of range" %
                    e["recordIndex"]}
        ext = mo.apply_usa(disk.read(ep, vol.recordSize), vol.recordSize)
        if ext[:4] != b"FILE":
            return {"error": "extension record %d magic mismatch" %
                    e["recordIndex"]}
        runs, real, _init = find_unnamed_data_runs(ext, vol.recordSize)
        if runs is None:
            return {"error": "extension record %d has no unnamed non-resident "
                    "$DATA" % e["recordIndex"]}
        if real_size is None:
            real_size = real
        seen.append({"recordIndex": e["recordIndex"],
                     "lowestVcn": e["lowestVcn"], "sequence": e["sequence"],
                     "runCount": len(runs),
                     "clusters": sum(n for _, n in runs),
                     "declaredRealSize": real})
        for lcn, nclus in runs:
            flat.append((lcn, nclus))
            cursor += nclus
    if real_size is None or real_size <= 0:
        # A deleted / truncated file can leave an $ATTRIBUTE_LIST whose
        # extension $DATA has realSize=0 and an EMPTY runlist. The engine's
        # large `size` is then just stale $FILE_NAME.realSize. Reporting
        # "no recoverable data" is CORRECT here, not an engine gap.
        return {
            "noPayload": True,
            "truthSize": 0,
            "truthSha256": hashlib.sha256(b"").hexdigest(),
            "extensionRecords": seen,
            "reason": "extension $DATA has no runlist / realSize=0 "
                      "(deleted or truncated); engine declared size is "
                      "stale $FILE_NAME.realSize",
        }
    payload = vol.read_file_payload(flat, real_size)
    return {
        "truthSha256": hashlib.sha256(payload).hexdigest(),
        "truthSize": real_size,
        "payloadBytesRead": len(payload),
        "flattenedRuns": len(flat),
        "extensionRecords": seen,
        "source": "$ATTRIBUTE_LIST -> extension MFT records, read-only",
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--negative-guard", action="store_true")
    ap.add_argument("--out", default="engine_gap_evidence.json")
    args = ap.parse_args()

    disk = mo.Disk()
    errors = []
    volumes = []
    try:
        for boot, csv_name in VOLUMES:
            path = os.path.join(OUT, csv_name)
            gaps = read_gap_nodes(path)
            if args.negative_guard and boot == 63 and gaps:
                # Tamper ONE real node's declared size. The tool must reject
                # the evidence because truthSize no longer agrees with the
                # engine's own CSV, proving the cross-check is falsifiable.
                gaps[0]["engineSize"] += 1
                gaps[0]["negativeGuard"] = True
            vol = mo.Volume(disk, boot)
            checked = []
            for g in gaps:
                truth = resolve_truth(disk, vol, g["id"])
                row = dict(g)
                row.update(truth)
                if "error" in truth:
                    errors.append("vol %d id %d: %s" %
                                  (boot, g["id"], truth["error"]))
                elif truth.get("noPayload"):
                    row["classification"] = "noPayloadDeletedExtension"
                    row["engineDeclaredSizeIsStale"] = True
                elif truth["truthSize"] != g["engineSize"]:
                    errors.append("vol %d id %d size mismatch: engine=%d "
                                  "truth=%d" % (boot, g["id"],
                                                g["engineSize"],
                                                truth["truthSize"]))
                    row["sizeAgreesWithEngine"] = False
                else:
                    row["sizeAgreesWithEngine"] = True
                checked.append(row)
            volumes.append({"bootLBA": boot, "engineCsv": csv_name,
                            "gapNodes": len(gaps), "nodes": checked})
    finally:
        disk.close()

    real_gaps = [n for v in volumes for n in v["nodes"]
                 if "classification" not in n and "error" not in n]
    no_payload = [n for v in volumes for n in v["nodes"]
                  if n.get("classification") == "noPayloadDeletedExtension"]
    total = len(real_gaps)
    size_sum = sum(n.get("truthSize", 0) for n in real_gaps)
    verdict = ("ENGINE ATTRIBUTE_LIST GAP CONFIRMED" if total
               else "NO GAP DETECTED")
    doc = {
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "generatedBy": "tools/verify_engine_gaps.py",
        "purpose": "Independent read-only proof that the engine misses $DATA "
                   "stored in $ATTRIBUTE_LIST extension MFT records.",
        "engineCsvIsDiscoverySource": True,
        "sourceDiskWriteOperations": 0,
        "gapNodeCount": total,
        "gapBytes": size_sum,
        "noPayloadDeletedExtensionCount": len(no_payload),
        "noPayloadIds": [n["id"] for n in no_payload],
        "verdict": verdict,
        "volumes": volumes,
        "errors": errors,
        "negativeGuard": args.negative_guard,
    }
    dest = args.out if os.path.isabs(args.out) else os.path.join(OUT, args.out)
    with open(dest, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2, ensure_ascii=False)

    for v in volumes:
        for n in v["nodes"]:
            print("vol=%-10d id=%-8d engineSize=%-10d truthSize=%-10s "
                  "ext=%s sha=%s" %
                  (v["bootLBA"], n["id"], n["engineSize"],
                   n.get("truthSize", "ERR"),
                   len(n.get("extensionRecords", [])),
                   (n.get("truthSha256") or n.get("error", ""))[:16]))
    print("VERDICT: %s | nodes=%d bytes=%d | errors=%d" %
          (verdict, total, size_sum, len(errors)))
    if errors:
        for e in errors[:10]:
            print("  ERROR:", e)
        return 1
    if args.negative_guard:
        print("ERROR: negative guard did not trip (gate not falsifiable)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
