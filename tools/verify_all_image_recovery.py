#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Full-image recovery verification, streaming and restartable.

Compares EVERY recovered file against an independent NTFS MFT oracle view of
the raw volume image.  Both sides are hashed incrementally: nothing longer than
CHUNK bytes is ever held in memory, which is what makes 100k+ files / 100+ GiB
practical.  Results are appended to a JSONL file as they are produced, so a
long run can be monitored (or interrupted and resumed) without losing work.

Truth source: the oracle's candidates CSV (id/volumeBootLBA/size/resident/
runs/residentHex).  For resident files the oracle's own residentHex is used;
for non-resident files the byte stream is rebuilt from the runlist directly
against the image.  Compressed files and files missing from the oracle are
reported as SKIP, never silently counted as matches.

usage:
  py -3 tools/verify_all_image_recovery.py \
      --image C:/PhoenixImage/v1.img \
      --csv docs/reports/real_disk_recovery_2026-11-14/candidates_v1_full.csv \
      --result C:/PhoenixImage/recover_v1_full_result.csv \
      --out C:/PhoenixImage/verify_v1_full
"""
import argparse
import base64
import csv
import hashlib
import json
import os
import sys
import time

CHUNK = 8 << 20


def load_candidates(path, wanted):
    rows = {}
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r["id"] not in wanted:
                continue
            runs = []
            for part in r.get("runs", "").split(";"):
                part = part.strip()
                if not part:
                    continue
                lcn, cnt = part.split(":")
                runs.append((int(lcn), int(cnt)))
            first = runs[0][0] if runs else -1
            rows[r["id"]] = {
                "size": int(r["size"] or 0),
                "resident": r["resident"] == "1",
                "compressed": r["compressed"] == "1",
                "residentHex": r.get("residentHex", ""),
                "runs": runs,
                "firstLCN": first,
                "name": decode_name(r.get("name", "")),
            }
    return rows


def decode_name(value):
    try:
        return base64.b64decode(value).decode("utf-8", "replace")
    except Exception:
        return value


def load_targets(path):
    targets = {}
    with open(path, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            if r.get("status", "0") != "0":
                continue
            p = r.get("targetPath", "").replace("/", os.sep)
            targets.setdefault(r["id"], []).append((p, r.get("engineExt", "")))
    return targets


def pick_target(cands, name):
    for p, _ in cands:
        if os.path.isfile(p) and os.path.basename(p).lower() == name.lower():
            return p
    for p, _ in cands:
        if os.path.isfile(p):
            return p
    return None


def truth_hash(img, row):
    """SHA-256 and byte count of the oracle's view of this file."""
    size = row["size"]
    h = hashlib.sha256()
    if row["resident"]:
        payload = bytes.fromhex(row["residentHex"])[:size]
        h.update(payload)
        return h.hexdigest(), len(payload)

    left = size
    for lcn, count in row["runs"]:
        if left <= 0:
            break
        take_clusters = min(count, (left + 4095) // 4096)
        if lcn < 0:  # sparse run
            zeros = b"\x00" * min(left, take_clusters * 4096)
            while zeros and left > 0:
                chunk = zeros[:min(CHUNK, left)]
                h.update(chunk)
                left -= len(chunk)
                zeros = zeros[len(chunk):]
            continue
        img.seek(lcn * 4096)
        remaining = min(left, take_clusters * 4096)
        while remaining > 0:
            blk = img.read(min(CHUNK, remaining))
            if not blk:
                return None, size - left
            h.update(blk)
            remaining -= len(blk)
            left -= len(blk)
    return h.hexdigest(), size - left


def file_hash(path, limit):
    h = hashlib.sha256()
    left = limit
    with open(path, "rb") as f:
        while left > 0:
            blk = f.read(min(CHUNK, left))
            if not blk:
                break
            h.update(blk)
            left -= len(blk)
    return h.hexdigest(), limit - left


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--result", required=True)
    ap.add_argument("--out", required=True, help="output prefix; writes .jsonl + .json")
    ap.add_argument("--ids", default="", help="optional id list (default: all result ids)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--report-every", type=int, default=1000)
    a = ap.parse_args()

    if a.ids:
        wanted = {x.strip() for x in open(a.ids, encoding="utf-8") if x.strip()}
    else:
        wanted = set(load_targets(a.result).keys())

    print("loading oracle rows for %d ids ..." % len(wanted), flush=True)
    cand = load_candidates(a.csv, wanted)
    targets = load_targets(a.result)
    print("oracle rows=%d  result ids=%d" % (len(cand), len(targets)), flush=True)

    order = sorted(
        cand.keys(),
        key=lambda k: (cand[k]["firstLCN"] < 0, cand[k]["firstLCN"]),
    )
    if a.limit:
        order = order[:a.limit]

    jsonl = a.out + ".jsonl"
    done = set()
    if os.path.exists(jsonl):
        for line in open(jsonl, encoding="utf-8"):
            try:
                done.add(json.loads(line)["id"])
            except Exception:
                pass
        print("resume: %d results already present" % len(done), flush=True)

    counts = {"match": 0, "mismatch": 0, "skip": 0, "error": 0}
    bytes_ok = 0
    skipped = {}
    mismatches = []
    t0 = time.time()
    fh = open(jsonl, "a", encoding="utf-8")
    with open(a.image, "rb", buffering=0) as img:
        for n, rid in enumerate(order, 1):
            if rid in done:
                continue
            row = cand[rid]
            rec = {"id": rid, "name": row["name"], "size": row["size"]}
            if row["compressed"]:
                rec.update(kind="skip", reason="ntfs-compressed")
                counts["skip"] += 1
                skipped["compressed"] = skipped.get("compressed", 0) + 1
            elif not row["resident"] and not row["runs"] and row["size"] > 0:
                rec.update(kind="skip", reason="no-runs")
                counts["skip"] += 1
                skipped["no-runs"] = skipped.get("no-runs", 0) + 1
            else:
                target = pick_target(targets.get(rid, []), row["name"])
                if target is None:
                    rec.update(kind="skip", reason="no-recovered-file")
                    counts["skip"] += 1
                    skipped["no-recovered-file"] = skipped.get("no-recovered-file", 0) + 1
                else:
                    try:
                        h_img, n_img = truth_hash(img, row)
                        h_file, n_file = file_hash(target, row["size"])
                        if h_img is None:
                            rec.update(kind="error", reason="short-image-read",
                                       imageBytes=n_img, file=os.path.basename(target))
                            counts["error"] += 1
                        elif h_img == h_file and n_img == row["size"] and n_file == row["size"]:
                            rec.update(kind="match", file=os.path.basename(target))
                            counts["match"] += 1
                            bytes_ok += n_file
                        else:
                            rec.update(kind="mismatch", file=os.path.basename(target),
                                       imageSha256=h_img, fileSha256=h_file,
                                       imageBytes=n_img, fileBytes=n_file)
                            counts["mismatch"] += 1
                            if len(mismatches) < 200:
                                mismatches.append(rec)
                    except Exception as e:
                        rec.update(kind="error", reason="%s: %s" %
                                   (type(e).__name__, str(e)[:160]))
                        counts["error"] += 1
            fh.write(json.dumps(rec, ensure_ascii=False) + "\n")
            if n % a.report_every == 0:
                fh.flush()
                sec = time.time() - t0
                print("%d/%d  match=%d mismatch=%d skip=%d err=%d  %.2f GiB  %.1f MiB/s" %
                      (n, len(order), counts["match"], counts["mismatch"],
                       counts["skip"], counts["error"], bytes_ok / 2 ** 30,
                       bytes_ok / 2 ** 20 / max(sec, 1e-6)), flush=True)
    fh.close()
    summary = {
        "image": a.image,
        "oracleCsv": a.csv,
        "result": a.result,
        "requested": len(order),
        "counts": counts,
        "verifiedBytes": bytes_ok,
        "skippedReasons": skipped,
        "mismatchSamples": mismatches,
        "elapsedSec": round(time.time() - t0, 1),
    }
    with open(a.out + ".json", "w", encoding="utf-8") as f:
        json.dump(summary, f, ensure_ascii=False, indent=1)
    print("DONE %s" % a.out, json.dumps(counts),
          "bytes=%.2f GiB" % (bytes_ok / 2 ** 30), flush=True)


if __name__ == "__main__":
    main()
