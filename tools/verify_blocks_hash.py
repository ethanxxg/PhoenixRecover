#!/usr/bin/env python3
"""Streaming byte verification: image clusters vs recovered file, no full-file buffers.

tools/mft_oracle.py `hash` reads a whole file into memory, so files near 512 MiB
(or a long run of files) die with MemoryError. This helper hashes the same bytes
incrementally: the raw image is read cluster-run by cluster-run (runlist taken
from the oracle's own parsed CSV) and compared against the SHA-256 of the
recovered file on disk.

usage:
  py -3 tools/verify_blocks_hash.py --image C:\\PhoenixImage\\v0.img \
      --csv <oracle candidates.csv> --result <recover result.csv> \
      --ids ids.txt --out out.json
"""
import argparse
import csv
import hashlib
import json
import os

CHUNK = 8 << 20


def load_runs(spec, cluster):
    out = []
    for seg in spec.split(";"):
        seg = seg.strip()
        if not seg:
            continue
        lcn, cnt = seg.split(":")
        out.append((int(lcn) * cluster, int(cnt) * cluster))
    return out


def sha_stream(f, total):
    h = hashlib.sha256()
    left = total
    while left > 0:
        blk = f.read(min(CHUNK, left))
        if not blk:
            break
        h.update(blk)
        left -= len(blk)
    return h.hexdigest(), total - left


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--image", required=True)
    ap.add_argument("--csv", required=True)
    ap.add_argument("--result", required=True)
    ap.add_argument("--ids", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    rows = {}
    with open(args.csv, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            rows.setdefault(r["id"], r)

    paths = {}
    with open(args.result, encoding="utf-8", newline="") as f:
        for r in csv.DictReader(f):
            paths.setdefault(r["id"], []).append(r["targetPath"].replace("/", os.sep))

    want = [x.strip() for x in open(args.ids, encoding="utf-8") if x.strip()]
    out = {}
    with open(args.image, "rb") as img:
        for rid in want:
            row = rows.get(rid)
            if not row or row["resident"] == "1" or not row["runs"].strip():
                out[rid] = {"error": "not a plain non-resident file"}
                continue
            cand = [p for p in paths.get(rid, []) if os.path.exists(p)]
            if not cand:
                out[rid] = {"error": "recovered file missing"}
                continue
            fp = cand[0]
            size = int(row["size"])
            h_img = hashlib.sha256()
            read = 0
            for off, length in load_runs(row["runs"], 4096):
                img.seek(off)
                left = length
                while left > 0 and read < size:
                    blk = img.read(min(CHUNK, left, size - read))
                    if not blk:
                        break
                    h_img.update(blk)
                    left -= len(blk)
                    read += len(blk)
                if read >= size:
                    break
            with open(fp, "rb") as f:
                h_file, fbytes = sha_stream(f, size)
            out[rid] = {
                "imageSha256": h_img.hexdigest(),
                "fileSha256": h_file,
                "bytes": fbytes,
                "size": size,
                "match": h_img.hexdigest() == h_file and fbytes == size,
                "file": os.path.basename(fp),
            }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, ensure_ascii=False, indent=1)
    ok = sum(1 for v in out.values() if v.get("match"))
    print("checked=%d match=%d bytes=%.2f GiB -> %s" %
          (len(out), ok, sum(v.get("bytes", 0) for v in out.values()) / 2 ** 30, args.out))


if __name__ == "__main__":
    main()
