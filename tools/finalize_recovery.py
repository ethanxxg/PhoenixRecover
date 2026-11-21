#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Unattended close-out of the real-disk recovery (P0-9).

The 7-batch engine-driven recovery runs in a separate background process.
This watcher waits until checkpoint.json reports ALL 7 batches, then executes
the ordered sequence from `P0-3_repair_playbook.md §5.1`:

    status -> engine-plan verification -> historical verification
    -> cleanup -> zero-write re-sample -> STRICT archive gate -> summary

Hard rules:
  * Byte verification MUST run BEFORE cleanup: it replays archived ids
    through the production engine into recover_staging/ when an engine SHA-256
    is missing. Cleanup would destroy that evidence.
  * Any CRITICAL step failing stops the sequence immediately and leaves the
    staging tree intact for triage. `finalize_state.json` records exactly
    where it stopped.
  * cleanup is WARNING-level: its contents are exclusively zero-byte shells
    (all clusters were already reclaimed per batch). If the native rmdir is
    interrupted, the strict archive gate can still run and the leftovers can
    be removed manually later.
  * Never writes to the source disk. Idempotent: an existing successful
    state file makes this watcher exit without replaying anything.

Usage:
    python tools/finalize_recovery.py                 # wait + finalise
    python tools/finalize_recovery.py --timeout 21600
    python tools/finalize_recovery.py --status-only  # just print state
"""
import argparse
import json
import os
import subprocess
import sys
import time

ROOT = r"D:/dev/PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
CP = os.path.join(OUT, "checkpoint.json")
LOG = os.path.join(OUT, "finalize.log")
STATE = os.path.join(OUT, "finalize_state.json")
PY = sys.executable
EXPECTED = {1, 2, 3, 4, 5, 6, 7}

# (name, argv, severity). `critical` = abort on non-zero; `warning` = log it
# but keep going. Order is load-bearing; do not reorder.
STEPS = [
    ("status", [PY, "tools/batch_recover.py", "status"], "critical"),
    ("verify_engine_plan_batch",
     [PY, "tools/verify_engine_plan_batch.py", "--all-completed"], "critical"),
    ("verify_engine_batch",
     [PY, "tools/verify_engine_batch.py", "--all-completed"], "critical"),
    ("cleanup", [PY, "tools/batch_recover.py", "cleanup"], "warning"),
    ("forensic_verify",
     [PY, "tools/forensic_baseline.py", "--verify"], "critical"),
    ("verify_archive", [PY, "tools/verify_archive.py"], "critical"),
    ("make_summary", [PY, "tools/make_summary.py"], "critical"),
]


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def completed():
    if not os.path.isfile(CP):
        return set()
    try:
        with open(CP, "r", encoding="utf-8") as f:
            return set(int(x) for x in json.load(f).get("completed", []))
    except (OSError, ValueError):
        return set()


def save_state(state):
    tmp = STATE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(state, f, indent=2, ensure_ascii=False)
    os.replace(tmp, STATE)


def staging_shell_stats():
    """Measure what is left under recover_staging/, split by what it is.

    Two different things live there and must never be added together:
      * `batch_NNN` trees - recovered payloads already released per batch, so
        only zero-byte directory shells remain. This is what `cleanup` sweeps.
      * auxiliary probe trees (`readahead_*`, `ra_*`, `p07_*`) - benchmark and
        evidence artefacts that still hold bytes on purpose.
    A deferred sweep is only justified when `batchShellBytes == 0`.
    """
    root = os.path.join(ROOT, "recover_staging")
    out = {"batchShellFiles": 0, "batchShellBytes": 0,
           "auxBytes": 0, "auxDirs": {}}
    if not os.path.isdir(root):
        return out
    for name in sorted(os.listdir(root)):
        path = os.path.join(root, name)
        if not os.path.isdir(path):
            continue
        files = size = 0
        for cur, _subdirs, names in os.walk(path):
            for nm in names:
                files += 1
                try:
                    size += os.path.getsize(os.path.join(cur, nm))
                except OSError:
                    pass
        if name.startswith("batch_"):
            out["batchShellFiles"] += files
            out["batchShellBytes"] += size
        else:
            out["auxBytes"] += size
            if files or size:
                out["auxDirs"][name] = {"files": files, "bytes": size}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--timeout", type=int, default=21600,
                    help="max seconds to wait for all 7 batches (default 6h)")
    ap.add_argument("--poll", type=int, default=60)
    ap.add_argument("--settle", type=int, default=90,
                    help="extra seconds after checkpoint to let the batch "
                         "loop exit before touching staging")
    ap.add_argument("--status-only", action="store_true")
    ap.add_argument("--resume", action="store_true",
                    help="continue an interrupted run: keep recorded steps and "
                         "skip the ones that already returned rc=0")
    ap.add_argument("--skip-cleanup", action="store_true",
                    help="record the zero-byte shell sweep as DEFERRED instead of "
                         "blocking on it; the verification chain does not depend "
                         "on it because every batch staging file is 0 bytes")
    ap.add_argument("--rerun", default="",
                    help="comma separated step names to execute again even if an "
                         "earlier run returned rc=0 (use after the staging tree "
                         "changed, e.g. verify_archive,make_summary)")
    args = ap.parse_args()
    rerun = {s.strip() for s in args.rerun.split(",") if s.strip()}

    if args.status_only:
        st = json.load(open(STATE, "r", encoding="utf-8")) \
            if os.path.isfile(STATE) else {"status": "no state file"}
        print(json.dumps(st, indent=2, ensure_ascii=False))
        return 0

    if os.path.isfile(STATE):
        with open(STATE, "r", encoding="utf-8") as f:
            old = json.load(f)
        if old.get("status") == "OK" and not (
                args.resume and (args.skip_cleanup or rerun)):
            log("already finalised at %s; nothing to do" %
                old.get("finishedAt"))
            return 0

    resumed = []
    if args.resume and os.path.isfile(STATE):
        with open(STATE, "r", encoding="utf-8") as f:
            state = json.load(f)
        resumed = [s.get("name") for s in state.get("steps", [])]
        log("resume: keeping %d recorded step(s) %s" % (len(resumed), resumed))
    else:
        state = {
            "status": "waiting",
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "expectedBatches": sorted(EXPECTED),
            "steps": [],
            "sourceDiskWriteOperations": 0,
        }
    state["status"] = "waiting"
    state.pop("failedStep", None)
    save_state(state)
    log("watcher started; waiting for checkpoint %s (timeout=%ds)" %
        (sorted(EXPECTED), args.timeout))

    t0 = time.time()
    last_beat = 0
    while True:
        cur = completed()
        if EXPECTED <= cur:
            break
        if time.time() - t0 > args.timeout:
            state["status"] = "TIMEOUT"
            state["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
            state["lastCompleted"] = sorted(cur)
            save_state(state)
            log("TIMEOUT: completed=%s" % sorted(cur))
            return 2
        if time.time() - last_beat >= 600:
            last_beat = time.time()
            log("waiting ... completed=%s elapsed=%.0fs" %
                (sorted(cur), time.time() - t0))
        time.sleep(args.poll)

    log("checkpoint complete: %s; settling %ds" %
        (sorted(completed()), args.settle))
    time.sleep(args.settle)
    state["status"] = "running"
    state["checkpointAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    state["checkpointCompleted"] = sorted(completed())
    save_state(state)

    done_ok = {s.get("name") for s in state.get("steps", [])
               if s.get("returnCode") == 0}

    for name, argv, severity in STEPS:
        if name in done_ok and name not in rerun:
            log("STEP %s: already rc=0 earlier; skipping" % name)
            continue
        if name == "cleanup" and args.skip_cleanup:
            stats = staging_shell_stats()
            # Idempotent: a resumed run replaces the earlier deferred record
            # instead of appending a second one.
            state["steps"] = [s for s in state.get("steps", [])
                              if s.get("name") != name]
            entry = {
                "name": name, "severity": severity, "deferred": True,
                "returnCode": None,
                "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
                "reason": ("zero-byte shell sweep deferred: %d batch staging "
                           "shells hold %d bytes (payloads were released per "
                           "batch, so nothing is left to reclaim); %d auxiliary "
                           "probe dirs hold %d bytes on purpose and are not "
                           "batch data" %
                           (stats["batchShellFiles"],
                            stats["batchShellBytes"],
                            len(stats["auxDirs"]), stats["auxBytes"])),
                "batchShellFiles": stats["batchShellFiles"],
                "batchShellBytes": stats["batchShellBytes"],
                "auxProbeBytes": stats["auxBytes"],
                "auxProbeDirs": stats["auxDirs"],
            }
            state["steps"].append(entry)
            state.setdefault("deferred", [])
            if "cleanup" not in state["deferred"]:
                state["deferred"].append("cleanup")
            save_state(state)
            log("STEP %s: DEFERRED (%d batch shells / %d bytes; aux probes "
                "%d dirs / %d bytes)" %
                (name, stats["batchShellFiles"], stats["batchShellBytes"],
                 len(stats["auxDirs"]), stats["auxBytes"]))
            continue
        log("STEP %s: %s" % (name, " ".join(argv[1:])))
        t = time.time()
        r = subprocess.run(argv, cwd=ROOT, capture_output=True, text=True,
                           encoding="utf-8", errors="replace")
        out = ((r.stdout or "") + "\n" + (r.stderr or "")).strip()
        entry = {
            "name": name, "severity": severity, "returnCode": r.returncode,
            "seconds": round(time.time() - t, 1),
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "outputTail": out[-2000:],
        }
        # Idempotent: re-running a step replaces its earlier record, including
        # a DEFERRED one, so the step list always has exactly one entry per step.
        state["steps"] = [s for s in state.get("steps", [])
                          if s.get("name") != name]
        state["steps"].append(entry)
        log("  rc=%d %.1fs :: %s" %
            (r.returncode, entry["seconds"], out.replace("\n", " | ")[-400:]))
        if r.returncode == 0 and name in (state.get("deferred") or []):
            state["deferred"] = [d for d in state["deferred"] if d != name]
            log("  step %s now executed for real; DEFERRED cleared" % name)
        if r.returncode != 0:
            if severity == "critical":
                state["status"] = "FAILED"
                state["failedStep"] = name
                state["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
                state["note"] = ("stopped before/after %s; staging preserved "
                                 "for triage" % name)
                save_state(state)
                log("ABORT at critical step %s; staging preserved" % name)
                return 1
            state.setdefault("warnings", []).append(name)
            log("  WARNING step %s failed; continuing (zero-byte shells only)"
                % name)
        save_state(state)

    # Keep the recorded steps in execution order even after a resumed run
    # re-inserted one of them (e.g. cleanup executed for real after being
    # recorded as DEFERRED), so the state file reads like the playbook.
    order = {name: i for i, (name, _argv, _sev) in enumerate(STEPS)}
    state["steps"].sort(key=lambda s: order.get(s.get("name"), len(order)))

    state["status"] = "OK"
    state["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    state["conclusion"] = ("ALL 7 BATCHES RECOVERED AND BYTE-VERIFIED; "
                           "STRICT ARCHIVE GATE PASSED")
    save_state(state)
    log("FINALISE OK: %s" % state["conclusion"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
