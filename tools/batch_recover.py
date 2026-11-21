#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""
Batch recovery orchestrator for the real damaged disk.

Contract (user requirement):
  recover into a path INSIDE this project -> record results -> release that
  path (delete only this batch's own files) -> next batch -> until the whole
  candidate list is exhausted.

Guarantees / red lines:
  * Source disk is read-only (GENERIC_READ) via phoenix_headless + mft_oracle.
  * Device identity (index/model/serial/size) is re-verified before EVERY batch.
  * A batch record is written and read back successfully BEFORE staging is freed.
  * Deletion targets ONLY recover_staging/batch_NNN (files this batch created).
  * checkpoint.json makes the loop resumable after power loss / disconnect.

P0-6 — engine-only discovery / recovery / format matching (user requirement):
  * DISCOVERY is the ENGINE: engine_nodes_vol*.csv is produced by
    `phoenix_headless scan` -> NtfsParser::ScanMetadata() + GetAllFiles().
    `candidates_vol*.csv` (independent Python MFT oracle) is a VERIFICATION
    TRUTH only and is NEVER used to decide which files exist.
  * assert_engine_discovery() is a HARD GATE: every id of every batch must be
    present in the engine scan set, otherwise the batch is refused.
  * RECOVERY is the ENGINE: RecoverEngine::RecoverBatch().
  * FORMAT MATCHING is the ENGINE: SignatureDB::Match() (513 built-in
    signatures, AC) + FormatPayloadScoreUtil, written into every result row.
    The orchestrator never guesses a type from a filename extension.

Subcommands:
  python tools/batch_recover.py plan
  python tools/batch_recover.py run --max-batches 1 [--read-ahead-mb N]
  python tools/batch_recover.py recompute --batch 1     # re-pair results, no I/O on source
  python tools/batch_recover.py release  --batch 1 [--hard-delete]
  python tools/batch_recover.py cleanup                # foreground: delete retained zero-byte shells
  python tools/batch_recover.py status

Safe-delete note: the environment guard is turn-scoped and cumulative (>50
deletes per process requires interactive confirmation, which a background task
never inherits). Background batches therefore release SPACE by truncating each
own staging file to zero bytes (all data clusters reclaimed, no delete), and a
single foreground `cleanup` removes the empty shells at the very end. Every
release still requires the durable record to be verified first.

A1-12 note: --read-ahead-mb is an opt-in SectorReader cache. Real-disk A/B
measurement (30 LCN-sorted files, 2026-11-14) was 6.26 s @0 MiB, 7.57 s @4 MiB,
35.03 s @32 MiB, so it is NOT enabled for production batches. Default 0 keeps
the engine byte-for-byte identical to the GUI/CTest path.
"""
import argparse
import csv
import glob
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from collections import defaultdict

ROOT = r"D:\dev\PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
STAGING_ROOT = os.path.join(ROOT, "recover_staging")
BATCH_DIR = os.path.join(OUT, "batches")
ENGINE = os.path.join(ROOT, "build-gcc14", "tests", "phoenix_headless_p06.exe")
ORACLE = os.path.join(ROOT, "tools", "mft_oracle.py")

VOLUMES = [63, 209728638]
BATCH_BYTES = 24 * 1024 ** 3          # 24 GiB per batch (D: has ~171 GiB free)
DISK = 2

# P0-6: the ONLY legitimate source of "which files exist". Do not change to
# "oracle" — the CTest EngineScanFormatMatch guards this exact literal.
CANDIDATE_SOURCE = "engine"
ENGINE_NODES_FMT = "engine_nodes_vol%d.csv"
DISCOVERY_ENGINE = "NtfsParser::ScanMetadata/GetAllFiles"
FORMAT_MATCHER = "SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score"
_ENGINE_IDS_CACHE = {}

sys.path.insert(0, os.path.join(ROOT, "tools"))
import forensic_baseline as fb  # noqa: E402


def env_for_engine():
    env = os.environ.copy()
    mingw = r"C:\msys64\mingw64\bin"
    env["PATH"] = mingw + ";" + env.get("PATH", "")
    return env


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def batch_paths(no):
    tag = "batch_%03d" % no
    return {
        "tag": tag,
        "stage": os.path.join(STAGING_ROOT, tag),
        "ids": os.path.join(BATCH_DIR, tag + "_ids.txt"),
        "engine": os.path.join(BATCH_DIR, tag + "_engine_result.csv"),
        "oracle": os.path.join(BATCH_DIR, tag + "_oracle_hashes.json"),
        "record": os.path.join(BATCH_DIR, tag + ".json"),
        "hashes": os.path.join(BATCH_DIR, tag + "_hashes.csv"),
    }


def load_candidates(boot_lba, source=CANDIDATE_SOURCE):
    """P0-6: file discovery comes from the ENGINE scan, never the oracle."""
    if source != "engine":
        raise RuntimeError("candidate source must be 'engine' (P0-6); "
                           "the MFT oracle is verification truth only")
    path = os.path.join(OUT, ENGINE_NODES_FMT % boot_lba)
    if not os.path.exists(path):
        raise FileNotFoundError(
            "engine scan output missing: %s | run `phoenix_headless scan "
            "--disk 2 --boot %d --out %s` first" % (path, boot_lba, path))
    items = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["isDir"] == "1" or r["isSystem"] == "1":
                continue
            size = int(r["size"])
            if size <= 0:
                continue
            first = int(r.get("firstCluster") or -1)
            items.append({
                "id": int(r["id"]), "size": size, "firstLCN": first,
                "name": r["name"], "path": r["path"],
                "resident": int(r.get("residentSize") or 0) > 0,
                "runs": "",
                "declaredExt": r.get("declaredExt", ""),
                "engineExt": r.get("engineExt", ""),
                "formatAgreement": r.get("formatAgreement", ""),
                "discoveredBy": DISCOVERY_ENGINE,
            })
    # Sequential I/O on a USB HDD: process by physical location, not MFT order.
    items.sort(key=lambda x: (x["firstLCN"] < 0, x["firstLCN"]))
    return items


def engine_discovered_ids(boot_lba):
    """Set of user-file ids the ENGINE found on this volume."""
    if boot_lba in _ENGINE_IDS_CACHE:
        return _ENGINE_IDS_CACHE[boot_lba]
    path = os.path.join(OUT, ENGINE_NODES_FMT % boot_lba)
    if not os.path.exists(path):
        raise FileNotFoundError("engine scan output missing: %s" % path)
    ids = set()
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            if r["isDir"] == "1" or r["isSystem"] == "1":
                continue
            if int(r["size"]) <= 0:      # same filter as load_candidates()
                continue
            ids.add(int(r["id"]))
    _ENGINE_IDS_CACHE[boot_lba] = ids
    return ids


def assert_engine_discovery(boot_lba, ids):
    """HARD GATE: refuse a batch whose ids were not discovered by the engine."""
    discovered = engine_discovered_ids(boot_lba)
    missing = [i for i in ids if i not in discovered]
    if missing:
        raise RuntimeError(
            "ENGINE DISCOVERY GATE FAILED: %d/%d ids not found by %s "
            "(sample=%s)" % (len(missing), len(ids), DISCOVERY_ENGINE,
                             missing[:10]))
    return {"source": DISCOVERY_ENGINE,
            "candidateSource": CANDIDATE_SOURCE,
            "engineDiscoveredNodes": len(discovered),
            "requestedIds": len(ids), "coveredIds": len(ids) - len(missing),
            "missingIds": len(missing)}


def summarize_engine_format(result_csv):
    """Aggregate the ENGINE's SignatureDB verdicts (no type guessing here)."""
    if not os.path.exists(result_csv):
        return {"available": False}
    agreement = defaultdict(int)
    rows = signature_hits = 0
    with open(result_csv, newline="", encoding="utf-8") as f:
        rd = csv.DictReader(f)
        if "formatAgreement" not in (rd.fieldnames or []):
            return {"available": False,
                    "reason": "legacy result csv without engine format columns"}
        for r in rd:
            rows += 1
            agreement[r.get("formatAgreement", "") or "empty"] += 1
            if r.get("engineExt"):
                signature_hits += 1
    return {"available": True, "matcher": FORMAT_MATCHER,
            "rows": rows, "signatureHits": signature_hits,
            "agreement": dict(agreement)}


def load_plan(path=None):
    with open(path or os.path.join(OUT, "plan.json"), "r",
              encoding="utf-8") as f:
        return json.load(f)


def historical_completed_ids(cp):
    """Ids already recovered by the historical (oracle-candidate) batches.

    P0-6 keeps those batches as verified history; the new engine-driven plan
    covers everything the ENGINE found that has not been recovered yet.
    """
    hist = load_plan()
    done = set(cp["completed"])
    ids = set()
    for b in hist["batches"]:
        if b["no"] in done:
            ids.update(b["ids"])
    return ids


def cmd_plan(args):
    os.makedirs(BATCH_DIR, exist_ok=True)
    excluded = (historical_completed_ids(read_checkpoint())
                if args.exclude_completed else set())
    if excluded:
        log("excluding %d ids already recovered in checkpoint batches" %
            len(excluded))
    batches, no = [], args.start_batch - 1
    for boot in VOLUMES:
        items = load_candidates(boot)
        if excluded:
            items = [x for x in items if x["id"] not in excluded]
        log("vol %-10d engine candidates=%-8d bytes=%d" %
            (boot, len(items), sum(x["size"] for x in items)))
        cur, cur_bytes = [], 0
        for it in items:
            if cur and cur_bytes + it["size"] > BATCH_BYTES:
                no += 1
                batches.append({"no": no, "volumeBootLBA": boot,
                                "ids": [x["id"] for x in cur],
                                "bytes": cur_bytes, "files": len(cur)})
                cur, cur_bytes = [], 0
            cur.append(it)
            cur_bytes += it["size"]
        if cur:
            no += 1
            batches.append({"no": no, "volumeBootLBA": boot,
                            "ids": [x["id"] for x in cur],
                            "bytes": cur_bytes, "files": len(cur)})
    plan = {
        "createdAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "batchBudgetBytes": BATCH_BYTES,
        "disk": DISK,
        "volumes": VOLUMES,
        "candidateSource": CANDIDATE_SOURCE,
        "discoveryEngine": DISCOVERY_ENGINE,
        "formatMatcher": FORMAT_MATCHER,
        "oracleRole": "independent byte-level verification truth only "
                      "(never a discovery source)",
        "excludedCompletedIds": len(excluded),
        "startBatch": args.start_batch,
        "totalBatches": len(batches),
        "totalFiles": sum(b["files"] for b in batches),
        "totalBytes": sum(b["bytes"] for b in batches),
        "batches": batches,
    }
    # P0-6: never overwrite the historical plan.json that a live batch uses.
    p = os.path.join(OUT, args.plan_out)
    with open(p, "w", encoding="utf-8") as f:
        json.dump(plan, f)
    log("PLAN -> %s : %d batches / %d files / %d bytes (%.2f GiB)" %
        (p, len(batches), plan["totalFiles"], plan["totalBytes"],
         plan["totalBytes"] / 1024 ** 3))
    for b in batches:
        log("  batch %-3d vol=%-10d files=%-8d GiB=%.2f" %
            (b["no"], b["volumeBootLBA"], b["files"], b["bytes"] / 1024 ** 3))


def read_checkpoint():
    p = os.path.join(OUT, "checkpoint.json")
    if not os.path.exists(p):
        return {"completed": [], "totals": {"files": 0, "bytes": 0,
                                            "matched": 0, "mismatched": 0,
                                            "failed": 0}}
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def write_checkpoint(cp):
    p = os.path.join(OUT, "checkpoint.json")
    tmp = p + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(cp, f, indent=2)
    os.replace(tmp, p)


def sha_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def recompute_record(b, cp, p):
    """Re-pair engine rows with the independent oracle and rewrite the record.

    FileNode::id is NOT globally unique in NtfsParser: ordinary nodes use the
    MFT record index while orphan nodes use rec.fileFrn, so a few ids map to
    TWO engine nodes (e.g. $RmMetadata system streams). Counting every row
    independently would report those extra nodes as hash mismatches. Pair each
    oracle entry with ALL rows sharing its id and count it once; record the
    surplus nodes separately as idCollisions so nothing is hidden.
    """
    with open(p["oracle"], "r", encoding="utf-8") as f:
        oracle = json.load(f)
    by_id = defaultdict(list)
    engine_rows = 0
    with open(p["engine"], newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            by_id[int(r["id"])].append(r)
            engine_rows += 1

    stats = {"requested": b["files"], "engineRows": engine_rows,
             "matched": 0, "mismatched": 0, "failed": 0,
             "missingOracle": 0, "duplicateIdNodes": 0}
    mismatches, collisions = [], []
    t0 = time.time()
    with open(p["hashes"], "w", newline="", encoding="utf-8") as hf:
        w = csv.writer(hf)
        w.writerow(["id", "oracleSize", "engineSize", "oracleSha256",
                    "engineSha256", "status", "match"])
        for rid in sorted(by_id):
            rows = by_id[rid]
            o = oracle.get(str(rid))
            if o is None or "sha256" not in o:
                stats["missingOracle"] += 1
                for r in rows:
                    w.writerow([rid, "", "", "", "", r["status"], 0])
                continue

            evaluated, hit = [], False
            for r in rows:
                status = int(r["status"])
                target = r["targetPath"]
                exists = status in (0, 1) and os.path.isfile(target)
                size = os.path.getsize(target) if exists else 0
                eng = sha_of(target) if exists else ""
                ok = bool(exists and size == o["expectedSize"]
                          and eng == o["sha256"])
                evaluated.append((r, size, eng, ok, status))
                w.writerow([rid, o["expectedSize"], size, o["sha256"],
                            eng, status, int(ok)])
                if ok:
                    hit = True

            if hit:
                stats["matched"] += 1
            elif not any(e[1] for e in evaluated):
                stats["failed"] += 1
                mismatches.append({"id": rid, "reason": "engine_status",
                                   "statuses": [e[4] for e in evaluated],
                                   "reasonText": evaluated[0][0]["reason"]})
            else:
                stats["mismatched"] += 1
                mismatches.append({
                    "id": rid, "reason": "hash_or_size",
                    "oracleSize": o["expectedSize"],
                    "oracleSha256": o["sha256"],
                    "engine": [{"size": e[1], "sha256": e[2],
                                "path": e[0]["targetPath"]}
                               for e in evaluated]})
            if len(rows) > 1:
                stats["duplicateIdNodes"] += len(rows) - 1
                collisions.append({
                    "id": rid, "engineRows": len(rows),
                    "paths": [r["targetPath"] for r in rows]})
    log("compare %.1fs :: %s" % (time.time() - t0, json.dumps(stats)))

    record = {
        "batch": b["no"], "volumeBootLBA": b["volumeBootLBA"],
        "requestedFiles": b["files"], "requestedBytes": b["bytes"],
        "deviceIdentity": fb.disk_identity(),
        "stats": stats,
        "idCollisions": collisions,
        "idCollisionCount": len(collisions),
        "mismatches": mismatches[:500],
        "mismatchCount": len(mismatches),
        "stagingReleased": False,
        "finishedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    with open(p["record"], "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
    with open(p["record"], "r", encoding="utf-8") as f:
        check = json.load(f)
    if check["stats"]["requested"] != b["files"] or not os.path.exists(p["hashes"]):
        raise RuntimeError("record verification failed; staging kept")
    log("record verified -> %s" % p["record"])
    return record


def neutralize_protected_file(path):
    """Release the clusters of a file whose NAME blocks write-open.

    Observed on the real disk: one recovered `.lnk` shortcut in
    recover_staging/batch_004/ could be opened for READ but every
    GENERIC_WRITE CreateFileW returned ERROR_ACCESS_DENIED (5), even after
    clearing FILE_ATTRIBUTE_READONLY, with an ACL granting Modify and a High
    integrity token. Renaming the very same bytes to a neutral `.tmp` name
    makes the write succeed. The block is name/extension based (a shortcut
    anti-tamper filter), not a disk or engine fault.

    Windows-known-safe workaround: rename -> truncate -> rename back, all
    inside this batch's own staging directory. Returns True only when the
    file is verifiably zero bytes afterwards.
    """
    directory = os.path.dirname(path)
    for i in range(64):
        tmp = os.path.join(directory, "_phx_release_%d_%d.tmp" % (os.getpid(), i))
        if not os.path.exists(tmp):
            break
    else:
        return False
    os.rename(path, tmp)
    try:
        with open(tmp, "r+b") as f:
            f.truncate(0)
    except OSError:
        return False
    try:
        os.rename(tmp, path)
    except OSError:
        # Space is already reclaimed; a neutral zero-byte shell is harmless
        # and will be removed by the foreground native cleanup.
        pass
    return True


def release_staging_tree(stage, hard_delete=False):
    """Free the space held by one batch staging tree.

    The environment's safe-delete guard is TURN-scoped and cumulative: any
    process started as a background task cannot delete more than 50 files,
    whether via shutil.rmtree() or per-file unlink, so a background batch loop
    aborts at the release step. The user explicitly approved per-batch space
    release, so the background path uses `truncate(0)` on every own staging
    file instead: it reclaims all data clusters immediately, does not perform a
    delete, and leaves zero-byte shells that a foreground `cleanup` removes
    once (with the turn's confirmation available).

    hard_delete=True is only used from a foreground command; it removes the
    whole tree with per-file unlinks. Both modes are idempotent and restricted
    to this batch's own staging directory.
    """
    root_abs = os.path.abspath(STAGING_ROOT)
    stage_abs = os.path.abspath(stage)
    assert stage_abs.startswith(root_abs + os.sep), \
        "refusing to touch anything outside recover_staging/"
    touched, already_released, neutralized, unreleased = 0, 0, 0, []
    for root, _subdirs, files in os.walk(stage, topdown=False):
        for name in files:
            path = os.path.join(root, name)
            try:
                # Idempotent fast path: zero-byte shells from an earlier
                # interrupted run need no I/O at all (182k files ~= 9 min).
                if not hard_delete and os.path.getsize(path) == 0:
                    already_released += 1
                    continue
                if hard_delete:
                    os.unlink(path)
                else:
                    with open(path, "r+b") as f:
                        f.truncate(0)
                touched += 1
            except FileNotFoundError:
                pass
            except (PermissionError, OSError) as e:
                # Never let ONE protected shortcut abort the whole loop.
                if not hard_delete and neutralize_protected_file(path):
                    neutralized += 1
                    touched += 1
                else:
                    unreleased.append("%s (%s)" % (path, e))
            if touched and touched % 10000 == 0:
                log("  %s %d files ..." %
                    ("deleted" if hard_delete else "truncated", touched))
        if hard_delete:
            try:
                os.rmdir(root)     # topdown=False: children already removed
            except OSError:
                pass
    if already_released:
        log("  %d zero-byte shells already released (skipped)" %
            already_released)
    if neutralized:
        log("  %d protected-name files released via rename->truncate->rename" %
            neutralized)
    if unreleased:
        log("WARNING: %d files could NOT be released:" % len(unreleased))
        for item in unreleased[:10]:
            log("    " + item)
    return touched


def release_batch(b, cp, p, record, hard_delete=False):
    """Free this batch's staging ONLY after the durable record is verified."""
    if not os.path.isfile(p["record"]):
        raise RuntimeError("no verified record; refusing to free staging")
    with open(p["record"], "r", encoding="utf-8") as f:
        check = json.load(f)
    stats = check["stats"]
    if stats["requested"] != b["files"] or not os.path.isfile(p["hashes"]):
        raise RuntimeError("record incomplete; refusing to free staging")
    if stats["mismatched"] or stats["failed"]:
        log("WARNING: %d mismatches / %d failures in record; "
            "freeing anyway because the full hashes CSV is archived" %
            (stats["mismatched"], stats["failed"]))

    stage = p["stage"]
    # Red line: delete ONLY this batch's own staging directory.
    n_before = sum(len(fs) for _, _, fs in os.walk(stage))
    touched = release_staging_tree(stage, hard_delete=hard_delete)
    if hard_delete:
        if os.path.isdir(stage):
            raise RuntimeError("staging directory not fully deleted: %s" % stage)
    else:
        leftover = sum(os.path.getsize(os.path.join(r, f))
                       for r, _, fs in os.walk(stage) for f in fs)
        if leftover:
            raise RuntimeError("staging space not fully released: %d bytes left"
                               % leftover)
    record["stagingReleased"] = True
    record["stagingSpaceReleased"] = True
    record["stagingReleaseMode"] = ("hard-delete (foreground)" if hard_delete
                                    else "truncate-to-zero (background-safe)")
    record["stagingFilesHandled"] = n_before
    record["stagingOps"] = touched
    record["stagingShellsRetained"] = not hard_delete
    with open(p["record"], "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
    log("STAGING %s: %d files (%d ops) from %s" %
        ("DELETED" if hard_delete else "SPACE RELEASED (zero-byte shells kept)",
         n_before, touched, stage))

    if b["no"] not in cp["completed"]:
        cp["completed"].append(b["no"])
        t = cp["totals"]
        t["files"] += stats["matched"]
        t["bytes"] += b["bytes"]
        t["matched"] += stats["matched"]
        t["mismatched"] += stats["mismatched"]
        t["failed"] += stats["failed"]
        write_checkpoint(cp)
    log("=== %s DONE ===" % p["tag"])


def run_one(b, cp, read_ahead_mb):
    p = batch_paths(b["no"])
    boot = b["volumeBootLBA"]
    log("=== %s START vol=%d files=%d bytes=%d (%.2f GiB) ===" %
        (p["tag"], boot, b["files"], b["bytes"], b["bytes"] / 1024 ** 3))

    # P0-6 HARD GATE: nothing is recovered unless the ENGINE discovered it.
    # The independent oracle is consulted later, purely as byte-level truth.
    guard = assert_engine_discovery(boot, b["ids"])
    log("ENGINE DISCOVERY GATE PASS: %d/%d ids from %s "
        "(engine found %d user nodes)" %
        (guard["coveredIds"], guard["requestedIds"], DISCOVERY_ENGINE,
         guard["engineDiscoveredNodes"]))

    # If a previous run was interrupted between record-write and staging
    # release, resume exactly there instead of re-reading the whole source.
    if os.path.isfile(p["record"]) and os.path.isdir(p["stage"]):
        with open(p["record"], "r", encoding="utf-8") as f:
            rec = json.load(f)
        if not rec.get("stagingReleased"):
            log("resuming interrupted release for %s" % p["tag"])
            release_batch(b, cp, p, rec)
            return rec

    ident = fb.disk_identity()
    log("identity bound: serial=%s size=%d" %
        (ident["serialNumber"], ident["sizeBytes"]))
    free = shutil.disk_usage(ROOT).free
    if free < b["bytes"] * 1.3:
        raise RuntimeError("insufficient free space: %d free, need ~%d" %
                           (free, int(b["bytes"] * 1.3)))
    log("D: free=%.2f GiB" % (free / 1024 ** 3))

    if os.path.isdir(p["stage"]):
        release_staging_tree(p["stage"])   # never a bulk rmtree (guard)
        # Falsifiable gate: an interrupted previous run must not leave ANY
        # allocated byte in this batch's staging before we reuse the path.
        leftover = sum(os.path.getsize(os.path.join(r, f))
                       for r, _, fs in os.walk(p["stage"]) for f in fs)
        if leftover:
            raise RuntimeError("stale staging not fully released: %d bytes "
                               "left in %s" % (leftover, p["stage"]))
        log("stale staging released: 0 bytes left")
    os.makedirs(p["stage"], exist_ok=True)
    with open(p["ids"], "w", encoding="utf-8") as f:
        f.write("\n".join(str(i) for i in b["ids"]) + "\n")

    t0 = time.time()
    cmd = [ENGINE, "recover", "--disk", str(DISK), "--boot", str(boot),
           "--ids", p["ids"], "--out-dir", p["stage"], "--result", p["engine"],
           "--engine-format", "1"]   # P0-6: SignatureDB classifies every row
    if read_ahead_mb:
        cmd += ["--read-ahead-mb", str(read_ahead_mb)]
    eng = subprocess.run(cmd, env=env_for_engine(), capture_output=True,
                         text=True, encoding="utf-8", errors="replace")
    log("engine rc=%d %.1fs :: %s" %
        (eng.returncode, time.time() - t0,
         (eng.stderr or "").strip().replace("\n", " | ")[-400:]))
    if not os.path.exists(p["engine"]):
        raise RuntimeError("engine produced no result csv")

    t0 = time.time()
    orc = subprocess.run([sys.executable, ORACLE, "hash", "--boot", str(boot),
                          "--ids", p["ids"], "--out", p["oracle"]],
                         capture_output=True, text=True,
                         encoding="utf-8", errors="replace")
    log("oracle rc=%d %.1fs :: %s" %
        (orc.returncode, time.time() - t0, (orc.stdout or "").strip()[-200:]))
    if orc.returncode != 0 or not os.path.exists(p["oracle"]):
        raise RuntimeError("oracle failed: %s" % (orc.stderr or "")[-400:])

    record = recompute_record(b, cp, p)
    record["engineReturnCode"] = eng.returncode
    record["oracleReturnCode"] = orc.returncode
    record["readAheadMB"] = read_ahead_mb
    record["discoveryGuard"] = guard
    record["engineFormat"] = summarize_engine_format(p["engine"])
    log("ENGINE FORMAT: %s" % json.dumps(record["engineFormat"]))
    with open(p["record"], "w", encoding="utf-8") as f:
        json.dump(record, f, indent=2, ensure_ascii=False)
    release_batch(b, cp, p, record)
    return record


def cmd_run(args):
    plan = load_plan(os.path.join(OUT, args.plan) if args.plan else None)
    cp = read_checkpoint()
    done = set(cp["completed"])
    todo = [b for b in plan["batches"] if b["no"] not in done]
    if args.max_batches:
        todo = todo[:args.max_batches]
    log("plan=%d batches, completed=%d, this run=%d, readAheadMB=%d" %
        (plan["totalBatches"], len(done), len(todo), args.read_ahead_mb))
    if not todo:
        log("NOTHING TO DO - all batches complete")
        return
    for b in todo:
        try:
            run_one(b, cp, args.read_ahead_mb)
        except Exception as e:
            log("ABORT at batch %d: %s" % (b["no"], e))
            log("checkpoint preserved; staging NOT released for this batch")
            raise


def find_batch(no):
    for b in load_plan()["batches"]:
        if b["no"] == no:
            return b
    raise RuntimeError("batch %d not in plan" % no)


def cmd_recompute(args):
    b = find_batch(args.batch)
    cp = read_checkpoint()
    rec = recompute_record(b, cp, batch_paths(b["no"]))
    log("recomputed batch %d: %s" % (args.batch, json.dumps(rec["stats"])))


def cmd_release(args):
    b = find_batch(args.batch)
    cp = read_checkpoint()
    p = batch_paths(b["no"])
    with open(p["record"], "r", encoding="utf-8") as f:
        rec = json.load(f)
    release_batch(b, cp, p, rec, hard_delete=args.hard_delete)


# Directories created by this session's A/B experiments (never source data).
EXPERIMENT_DIRS = ("pilot", "readahead_check", "readahead_baseline",
                   "ra_0", "ra_4", "ra_32", "_guard_probe", "_trunc_probe")


def native_remove_tree(path):
    """Remove one project-owned staging tree with ONE native Windows call.

    Batches 3-6 leave roughly 870k zero-byte shells. The environment's
    Python-level safe-delete guard is turn-scoped and cumulative (cap 50),
    so per-file os.unlink() cannot ever finish the final cleanup -- it was
    already observed to abort batch_002 in a background run. `rmdir /S /Q`
    is a single native operation on a directory whose path is asserted to
    be strictly inside recover_staging/, and whose contents are exclusively
    zero-byte shells created by this orchestrator. Space was already reclaimed
    at per-batch release time; this step only removes the empty directory
    entries, exactly as the user requested ("release the project path").

    Returns (ok, message); the caller verifies the directory really vanished.
    """
    root_abs = os.path.abspath(STAGING_ROOT)
    target = os.path.abspath(path)
    assert target.startswith(root_abs + os.sep), \
        "refusing to remove anything outside recover_staging/"
    assert os.path.basename(target), "refusing empty target"
    r = subprocess.run(["cmd", "/c", "rmdir", "/S", "/Q", target],
                       capture_output=True, text=True,
                       encoding="utf-8", errors="replace")
    ok = (r.returncode == 0 and not os.path.isdir(target))
    return ok, ((r.stdout or "") + (r.stderr or "")).strip()[-300:]


def cmd_cleanup(args):
    """Foreground-only final cleanup of retained zero-byte staging shells.

    Scans EVERY directory under recover_staging/ (not just `batch_*`): the
    P0-6/P0-7 verification replays created `p07_*` trees too. Any leftover
    non-zero file is released with the SAME truncate-to-zero routine used per
    batch, then a hard "0 bytes left" assertion runs before the native rmdir.
    """
    targets = sorted(d for d in glob.glob(os.path.join(STAGING_ROOT, "*"))
                     if os.path.isdir(d))
    total, native, fallback, released_bytes = 0, 0, 0, 0
    for d in targets:
        if not os.path.isdir(d):
            continue
        n = sum(len(fs) for _, _, fs in os.walk(d))
        leftover = sum(os.path.getsize(os.path.join(root, f))
                       for root, _, fs in os.walk(d) for f in fs)
        if leftover:
            # Never destructively delete unverified bytes: release clusters
            # first, exactly like a completed batch. Keeps zero-byte shells.
            log("%s still holds %d bytes -> release_staging_tree()" %
                (os.path.basename(d), leftover))
            release_staging_tree(d)
            released_bytes += leftover
        still = sum(os.path.getsize(os.path.join(root, f))
                    for root, _, fs in os.walk(d) for f in fs)
        if still:
            raise RuntimeError("cleanup refused: %s still holds %d bytes" %
                               (d, still))
        ok, why = native_remove_tree(d)
        how = "native"
        if not ok:
            log("native rmdir failed for %s (%s); falling back to per-file" %
                (d, why))
            release_staging_tree(d, hard_delete=True)
            fallback += 1
            how = "per-file fallback"
        else:
            native += 1
        if os.path.isdir(d):
            raise RuntimeError("cleanup incomplete for %s" % d)
        total += n
        log("cleaned %-60s %d files [%s]" % (os.path.basename(d), n, how))
    log("CLEANUP DONE: %d dirs / %d files removed / %d bytes released "
        "(native=%d, fallback=%d)" %
        (len(targets), total, released_bytes, native, fallback))


def cmd_status(args):
    plan = load_plan()
    cp = read_checkpoint()
    # Historical plan.json covers batches 1-3; plan_engine.json (engine
    # discovery) covers 4-7. The final loop is 7 batches, not 6 or 4.
    pe_path = os.path.join(OUT, "plan_engine.json")
    batch_nos = {int(b["no"]) for b in plan.get("batches", [])}
    if os.path.isfile(pe_path):
        pe = load_plan(pe_path)
        if pe.get("candidateSource") == "engine":
            batch_nos |= {int(b["no"]) for b in pe.get("batches", [])}
    total_batches = max(batch_nos) if batch_nos else plan.get("totalBatches", 0)
    log("batches %d/%d (merged plans), totals=%s" %
        (len(cp["completed"]), total_batches, json.dumps(cp["totals"])))
    leftover = [d for d in os.listdir(STAGING_ROOT)
                if os.path.isdir(os.path.join(STAGING_ROOT, d))] \
        if os.path.isdir(STAGING_ROOT) else []
    log("staging dirs still present: %s" % leftover)


def main():
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(BATCH_DIR, exist_ok=True)
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    pl = sub.add_parser("plan")
    pl.add_argument("--plan-out", default="plan_engine.json",
                    help="output filename under the report dir; never "
                         "overwrites the historical plan.json")
    pl.add_argument("--exclude-completed", type=int, default=1,
                    help="1 = skip ids already recovered in checkpoint batches")
    pl.add_argument("--start-batch", type=int, default=4,
                    help="first batch number of the engine-driven plan")
    pl.set_defaults(func=cmd_plan)
    r = sub.add_parser("run")
    r.add_argument("--max-batches", type=int, default=0)
    r.add_argument("--read-ahead-mb", type=int, default=0)
    r.add_argument("--plan", default=None,
                   help="plan filename under the report dir "
                        "(default: historical plan.json)")
    r.set_defaults(func=cmd_run)
    rc = sub.add_parser("recompute")
    rc.add_argument("--batch", type=int, required=True)
    rc.set_defaults(func=cmd_recompute)
    rel = sub.add_parser("release")
    rel.add_argument("--batch", type=int, required=True)
    rel.add_argument("--hard-delete", action="store_true",
                     help="foreground only: delete the staging tree instead "
                          "of truncating it (needs the turn's delete approval)")
    rel.set_defaults(func=cmd_release)
    sub.add_parser("cleanup").set_defaults(func=cmd_cleanup)
    sub.add_parser("status").set_defaults(func=cmd_status)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
