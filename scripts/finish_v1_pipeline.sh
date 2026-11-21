#!/usr/bin/env bash
# Watch the 24-shard full recovery of volume 1, then immediately run the
# ENGINE-SIDE byte verification and produce a consolidated summary.
# No Python / MFT oracle participates in any recovery or verification decision:
# discovery = NtfsParser, bytes = FileNode runlist via SectorReader, compare
# in phoenix_headless `verify`. The oracle is only a separate cross-check.
set -u
cd "$(dirname "$0")/.." || exit 1

LOG=/c/PhoenixImage/pipeline_finish.log
RESULT=/c/PhoenixImage/recover_v1_full_result.csv
VOUT=/c/PhoenixImage/engine_verify_v1_full.csv
log(){ echo "[$(date -Is)] $*" >> "$LOG"; }

log "PIPELINE START"

# --- stage 1: wait for all 24 recovery shards to finish ---
while true; do
    procs=$(ps -ef 2>/dev/null | grep "phoenix_headless_flat.exe recover" | grep -v grep | wc -l)
    rows=0
    [ -f "$RESULT" ] && rows=$(wc -l < "$RESULT")
    if [ "$procs" -eq 0 ] && [ "$rows" -gt 1 ]; then
        log "RECOVERY DONE result_rows=$rows"
        break
    fi
    log "waiting: recover_procs=$procs result_rows=$rows"
    sleep 60
done

# --- stage 2: engine-side byte verification (24 parallel shards) ---
log "ENGINE VERIFY START"
bash scripts/verify_v1_full_parallel.sh >> "$LOG" 2>&1
vrc=$?
vrows=0
[ -f "$VOUT" ] && vrows=$(wc -l < "$VOUT")
log "ENGINE VERIFY DONE rc=$vrc rows=$vrows"

# --- stage 3: status distribution (verbatim, no guessed encoding) ---
if [ "$vrows" -gt 1 ]; then
    log "VERIFY STATUS COUNTS:"
    tail -n +2 "$VOUT" | cut -d, -f2 | sort | uniq -c | while read -r line; do log "  $line"; done
fi

log "PIPELINE END rc=$vrc"
exit "$vrc"
