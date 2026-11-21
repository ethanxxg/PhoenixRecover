#!/usr/bin/env bash
# Hash every requested id from a raw volume image, in small batches.
#
# Why batches: tools/mft_oracle.py::hash accumulates per-file buffers and dies
# with MemoryError on long runs (12 GiB pagefile, then heap fragmentation over
# thousands of files). One process per batch resets the heap, and a batch that
# still blows up is isolated instead of killing the whole verification.
#
# usage: scripts/oracle_hash_batched.sh <image> <ids.txt> <out.json> [batch]
set -u
IMG=${1:?image}
IDS=${2:?ids}
OUT=${3:?out}
BATCH=${4:-500}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
split -l "$BATCH" -d "$IDS" "$WORK/id_"

n=0
fail=0
for f in "$WORK"/id_*; do
    n=$((n + 1))
    if PHOENIX_ORACLE_IMAGE="$IMG" py -3 tools/mft_oracle.py hash \
        --boot 0 --ids "$f" --out "$WORK/out_$n.json" >/dev/null 2>"$WORK/err_$n.txt"; then
        :
    else
        fail=$((fail + 1))
        echo "batch $n FAILED: $(tail -2 "$WORK/err_$n.txt" | tr '\n' ' ')" >&2
    fi
done

py -3 - "$WORK" "$OUT" "$n" "$fail" <<'EOF'
import glob, json, sys
work, out, batches, failed = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
merged = {}
for p in sorted(glob.glob(work + "/out_*.json")):
    with open(p, encoding="utf-8") as f:
        merged.update(json.load(f))
with open(out, "w", encoding="utf-8") as f:
    json.dump(merged, f, ensure_ascii=False, indent=1)
print("batches=%d failed=%d hashed=%d -> %s" % (batches, failed, len(merged), out))
EOF
