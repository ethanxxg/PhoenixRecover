#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""One compact watch window over the real-disk recovery (default 9 minutes).

Prints checkpoint / finalizer / current batch stage every `interval` seconds
and exits early when the finalizer reaches a terminal state. Keeps the
unattended loop observable without polling the background tasks themselves.

    python tools/watch_recovery.py                  # 9 min
    python tools/watch_recovery.py --seconds 540 --interval 180
"""
import argparse
import json
import os
import time

ROOT = r"D:/dev/PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
BATCH_DIR = os.path.join(OUT, "batches")


def jload(path, default=None):
    if not os.path.isfile(path):
        return default
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=int, default=540)
    ap.add_argument("--interval", type=int, default=180)
    args = ap.parse_args()
    end = time.time() + args.seconds
    log_path = os.path.join(OUT, "watch_progress.log")
    while True:
        cp = jload(os.path.join(OUT, "checkpoint.json"), {})
        fin = jload(os.path.join(OUT, "finalize_state.json"), {})
        latest = max(
            ((os.path.getmtime(os.path.join(BATCH_DIR, f)), f)
             for f in os.listdir(BATCH_DIR)), default=(0, ""))
        stage = ""
        for no in range(6, 8):
            tag = "batch_%03d" % no
            if os.path.isdir(os.path.join(ROOT, "recover_staging", tag)):
                stage = "engine-recovering"
            if os.path.isfile(os.path.join(BATCH_DIR, tag + ".json")):
                stage = "record-written"
        line = ("%s cp=%s fin=%s latest=%s@%s %s"
                % (time.strftime("%H:%M:%S"),
                   cp.get("completed", []), fin.get("status", "no-state"),
                   latest[1], time.strftime("%H:%M:%S",
                                            time.localtime(latest[0])), stage))
        print(line, flush=True)
        with open(log_path, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if fin.get("status") in ("OK", "FAILED", "TIMEOUT") \
                or time.time() + args.interval > end:
            break
        time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
