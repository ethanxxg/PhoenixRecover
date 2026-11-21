#!/usr/bin/env bash
# Full recovery of volume 1 from the completed raw image, sharded over 24
# processes with flat isolated output.  A per-id directory for each of 872k
# files costs ~80 ms/file; flat `f<seq>_<id>_<name>` leaves plus id shards cut
# that by ~4x. Each worker has its own output subtree and result CSV, so no
# two workers can collide.
#
# The `env -i` wrapper is mandatory: launching these exes from the Git-Bash
# tool environment crashes them at startup (see P0-13 notes).
set -u

cd "$(dirname "$0")/.." || exit 1
EXE='C:/PhoenixImage/phoenix_headless_flat.exe'
N=24
PIDS=()
FAIL=0

for i in $(seq -w 0 $((N - 1))); do
    env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows \
        MSYS2_ARG_CONV_EXCL='*' \
        "$EXE" recover \
        --image 'C:\PhoenixImage\v1.img' --boot 0 \
        --ids "C:\\PhoenixImage\\ids_v1_full_p${i}.txt" \
        --out-dir "D:\\PhoenixOut\\v1_full\\p${i}" \
        --result "C:\\PhoenixImage\\recover_v1_full_p${i}.csv" \
        --read-ahead-mb 0 --flat-output 1 --engine-format 0 \
        > "/c/PhoenixImage/recover_v1_full_p${i}.log" 2>&1 &
    PIDS+=("$!")
    echo "started shard $i pid ${PIDS[${#PIDS[@]}-1]}"
done

for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        FAIL=1
        echo "shard pid $pid FAILED"
    fi
done

OUT=/c/PhoenixImage/recover_v1_full_result.csv
: > "$OUT"
first=1
for i in $(seq -w 0 $((N - 1))); do
    f="/c/PhoenixImage/recover_v1_full_p${i}.csv"
    [ -f "$f" ] || { echo "missing result $f"; FAIL=1; continue; }
    if [ "$first" = 1 ]; then
        cat "$f" >> "$OUT"
        first=0
    else
        tail -n +2 "$f" >> "$OUT"
    fi
done

echo "ALL SHARDS DONE fail=$FAIL rows=$(wc -l < "$OUT") $(date -Is)"
exit "$FAIL"
