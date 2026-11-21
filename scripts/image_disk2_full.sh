#!/usr/bin/env bash
# Full-volume imaging of the LDM disk (PhysicalDrive 2) into C:\PhoenixImage.
#
# Runs the two NTFS volumes sequentially with resume support: a killed run can be
# restarted and continues from the last recorded LBA (see *.map.json nextLba).
#
# Why the `env -i` wrapper: launching image_region.exe from a Git-Bash tool shell
# inherits an environment whose injected DLLs make the exe crash at startup
# (segfault before any log line). A minimal PATH fixes it; do not "simplify" this.
set -u

EXE=./build-gcc14/tests/image_region.exe
OUT='C:\PhoenixImage'
LOG=/c/PhoenixImage/fullimage.log

run_volume() {
    local label=$1 start=$2 sectors=$3 name=$4
    local img="$OUT\\$name.img"
    local map="/c/PhoenixImage/$name.img.map.json"
    local extra=()
    [ -f "$map" ] && extra+=(--resume)

    echo "=== $label start LBA $start sectors $sectors -> $name.img $(date -Is) ===" | tee -a "$LOG"
    env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows \
        MSYS2_ARG_CONV_EXCL='*' \
        "$EXE" --disk 2 --start "$start" --sectors "$sectors" --out "$img" \
        --chunk-kib 1024 --retries 1 --timeout-ms 5000 \
        --max-streak 6 --skip-mib 8 --skip-max-mib 256 \
        --bad-budget 2000 --time-budget 0 "${extra[@]}" 2>&1 | tee -a "$LOG"
    echo "=== $label done rc=${PIPESTATUS[0]} $(date -Is) ===" | tee -a "$LOG"
}

cd "$(dirname "$0")/.." || exit 1
rm -f /c/PhoenixImage/v0.img.map.json.keep

run_volume "vol0" 63        209728511  v0
run_volume "vol1" 209728638 436228943  v1

echo "ALL DONE $(date -Is)" | tee -a "$LOG"
