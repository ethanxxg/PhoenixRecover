#!/usr/bin/env bash
# ENGINE-SIDE byte verification of every recovered volume-1 file.
#
# Same production path as `recover`: SectorReader::OpenImage + NtfsParser
# node discovery + FileNode runlist.  The engine rebuilds each file's bytes
# from the image and compares them byte-for-byte with the recovered file.
# The Python MFT oracle is NOT used here; it is only a separate cross-check.
set -u

EXE='C:/PhoenixImage/phoenix_headless_verify.exe'
IMAGE='C:\PhoenixImage\v1.img'
RESULT='C:\PhoenixImage\recover_v1_full_result.csv'
OUTDIR=/c/PhoenixImage
N=24
PIDS=()
FAIL=0

for i in $(seq -w 0 $((N - 1))); do
    env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows \
        MSYS2_ARG_CONV_EXCL='*' \
        "$EXE" verify \
        --image "$IMAGE" --boot 0 --result "$RESULT" \
        --ids "C:\\PhoenixImage\\ids_v1_full_p${i}.txt" \
        --out "C:\\PhoenixImage\\engine_verify_v1_full_p${i}.csv" \
        --read-ahead-mb 0 \
        > "$OUTDIR/engine_verify_v1_full_p${i}.log" 2>&1 &
    PIDS+=("$!")
    echo "verify shard $i pid ${PIDS[${#PIDS[@]}-1]}"
done

for pid in "${PIDS[@]}"; do
    if ! wait "$pid"; then
        FAIL=1
        echo "verify shard pid $pid FAILED"
    fi
done

OUT="$OUTDIR/engine_verify_v1_full.csv"
: > "$OUT"
first=1
for i in $(seq -w 0 $((N - 1))); do
    f="$OUTDIR/engine_verify_v1_full_p${i}.csv"
    [ -f "$f" ] || { echo "missing verify result $f"; FAIL=1; continue; }
    if [ "$first" = 1 ]; then
        cat "$f" >> "$OUT"
        first=0
    else
        tail -n +2 "$f" >> "$OUT"
    fi
done

echo "ENGINE VERIFY DONE fail=$FAIL rows=$(wc -l < "$OUT") $(date -Is)"
exit "$FAIL"
