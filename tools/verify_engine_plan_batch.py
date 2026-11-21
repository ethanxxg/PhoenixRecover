#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Explain archived anomalies of ENGINE-DRIVEN batches (P0-6/P0-7).

Batches 4-7 take their ids from `engine_nodes_vol*.csv` (NtfsParser), while the
independent `candidates_vol*.csv` truth is historical and incomplete. Three
classes of `match=0` rows therefore appear that the old `verify_engine_batch.py`
(ADT/ADS-false-mismatch schema) cannot describe:

  1. engine-only ADS nodes  -> id = (recordIndex << 4) | (adsIdx+1); the
     oracle candidate CSV has no row, so `recompute_record` archived an EMPTY
     engineSha256. Truth is the named $DATA payload re-parsed from the base MFT
     record; the engine bytes are obtained by a fresh ENGINE replay.
  2. zero-length / deleted files -> no payload exists; status!=0 is correct.
  3. $ATTRIBUTE_LIST gap (id 78) -> P0-6 engine had clusterCount=0; P0-7 fixed
     NtfsParser and `engine_fix_verification.json` proves byte-exact recovery.

This tool NEVER rewrites an archived record. For every `match=0` row it:
  * re-parses truth DIRECTLY from the read-only source disk (normal / ADS /
    attribute-list aware);
  * if the archived row has no engine SHA-256, replays those ids through the
    PRODUCTION engine (`phoenix_headless_p07.exe recover`) into a fresh staging
    directory and hashes the recovered bytes;
  * classifies each row and requires `unresolved == 0`;
  * writes `batch_NNN_engine_plan_verification.json` (same schema family as
    the P0-5 records, but re-derived field-by-field by verify_archive.py).

P0-9: `plan_engine.json::files` counts candidate ROWS, not distinct ids. A
duplicated numeric node id (an ADS slot id colliding with an MFT record index)
is the SAME recovery request twice, so `request_scope()` uses the distinct ids
in ids.txt as the byte-verification denominator and refuses any
declared-minus-unique deficit that is not fully explained by literal duplicate
lines. `verify_archive.py::ep_valid()` re-derives the same scope independently.

Usage:
    python tools/verify_engine_plan_batch.py --batch 4
    python tools/verify_engine_plan_batch.py --all-completed
    python tools/verify_engine_plan_batch.py --all-completed --dry-run
    python tools/verify_engine_plan_batch.py --all-completed --negative-guard-cross-volume
    python tools/verify_engine_plan_batch.py --batch 5 --negative-guard-duplicate-ids
"""
import argparse
import csv
import hashlib
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

ROOT = r"D:/dev/PhoenixRecover"
sys.path.insert(0, os.path.join(ROOT, "tools"))
import mft_oracle as mo
import verify_engine_gaps as gaps

OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
BATCH_DIR = os.path.join(OUT, "batches")
STAGING = os.path.join(ROOT, "recover_staging")
ENGINE = os.path.join(ROOT, "build-gcc14", "tests", "phoenix_headless_p07.exe")
GAP_EVIDENCE = os.path.join(OUT, "engine_gap_evidence.json")
FIX_EVIDENCE = os.path.join(OUT, "engine_fix_verification.json")


def jload(path, default=None):
    if not os.path.isfile(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def hash_rows(no):
    with open(os.path.join(BATCH_DIR, "batch_%03d_hashes.csv" % no),
              newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def engine_rows(no):
    p = os.path.join(BATCH_DIR, "batch_%03d_engine.csv" % no)
    if not os.path.isfile(p):
        return {}
    out = {}
    with open(p, newline="", encoding="utf-8", errors="replace") as f:
        for r in csv.DictReader(f):
            out[int(r["id"])] = r
    return out


def sha_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1024 * 1024), b""):
            h.update(b)
    return h.hexdigest()


def named_data_streams(rec, record_size, vol):
    """Return [(streamName, payloadBytes)] for every NAMED $DATA attribute."""
    out = []
    attr_off = struct.unpack_from("<H", rec, 0x14)[0]
    pos, end = attr_off, min(record_size, len(rec))
    while pos + 16 <= end:
        atype = struct.unpack_from("<I", rec, pos)[0]
        if atype in (0, 0xFFFFFFFF):
            break
        alen = struct.unpack_from("<I", rec, pos + 4)[0]
        if alen < 16 or pos + alen > end:
            break
        nl = rec[pos + 9]
        if atype == 0x80 and nl:
            no = struct.unpack_from("<H", rec, pos + 0x0A)[0]
            name = rec[pos + no:pos + no + nl * 2].decode("utf-16-le",
                                                          errors="replace")
            if not rec[pos + 8]:
                vlen = struct.unpack_from("<I", rec, pos + 0x10)[0]
                voff = struct.unpack_from("<H", rec, pos + 0x14)[0]
                if voff + vlen <= alen:
                    out.append((name, bytes(rec[pos + voff:pos + voff + vlen])))
            else:
                ro = struct.unpack_from("<H", rec, pos + 0x20)[0]
                real = struct.unpack_from("<Q", rec, pos + 0x30)[0]
                if ro < alen and real > 0:
                    runs = mo.decode_runlist(rec, pos + ro, pos + alen)
                    out.append((name, vol.read_file_payload(runs, real)))
        pos += alen
    return out


def load_ads_ids():
    """Engine CSV names contain ':' for ADS nodes. Pin the id encoding.

    Node ids are only unique WITHIN a volume (MFT record index). A set that
    merges both volumes would reinterpret vol 209728638's record N as vol
    63's ADS node N, so this map is volume-scoped.
    """
    out = {}
    for boot in (63, 209728638):
        p = os.path.join(OUT, "engine_nodes_vol%d.csv" % boot)
        ids = set()
        if not os.path.isfile(p):
            out[boot] = ids
            continue
        with open(p, newline="", encoding="utf-8", errors="replace") as f:
            for r in csv.DictReader(f):
                if ":" in (r.get("name") or ""):
                    ids.add(int(r["id"]))
        out[boot] = ids
    return out


def resolve_truth(disk, vol, rid, is_ads=False):
    """Independent truth for a normal id, an ADS id, or an attribute-list id."""
    pos = None if is_ads else vol.record_byte_offset(rid)
    if pos is None:
        # ADS id encoding: (recordIndex << 4) | (adsIdx + 1)
        base, slot = rid >> 4, (rid & 0xF) - 1
        if base <= 0 or slot < 0 or (rid & 0xF) == 0:
            return {"error": "id is neither a record index nor an ADS id"}
        bpos = vol.record_byte_offset(base)
        if bpos is None:
            return {"error": "ADS base record %d sparse/out of range" % base}
        brec = mo.apply_usa(disk.read(bpos, vol.recordSize), vol.recordSize)
        streams = named_data_streams(brec, vol.recordSize, vol)
        if slot >= len(streams):
            return {"error": "ADS slot %d missing (record %d has %d)" %
                    (slot, base, len(streams))}
        payload = streams[slot][1]
        return {"sha256": hashlib.sha256(payload).hexdigest(),
                "size": len(payload), "kind": "adsNamedData",
                "baseRecord": base, "streamName": streams[slot][0]}

    raw = disk.read(pos, vol.recordSize)
    rec = mo.apply_usa(raw, vol.recordSize)
    if rec[:4] != b"FILE":
        return {"error": "MFT record magic mismatch"}

    # $ATTRIBUTE_LIST with no in-base $DATA -> extension records.
    gap = gaps.resolve_truth(disk, vol, rid)
    if gap.get("noPayload"):
        return {"sha256": hashlib.sha256(b"").hexdigest(), "size": 0,
                "kind": "noPayloadDeletedExtension"}
    if "truthSha256" in gap:
        return {"sha256": gap["truthSha256"], "size": gap["truthSize"],
                "kind": "attributeListExtension",
                "extensionRecords": gap.get("extensionRecords", [])}

    info = mo.parse_record(rec, vol.recordSize)
    if info is None:
        return {"error": "unparsable MFT record"}
    if info["resident"] is not None:
        payload = info["resident"]
    else:
        payload = vol.read_file_payload(info["runs"], info["size"])
    return {"sha256": hashlib.sha256(payload).hexdigest(),
            "size": info["size"], "kind": "baseRecordData",
            "adsNames": info["adsNames"]}


def replay_ids(ids_by_boot, dry_run=False):
    """Recover unresolved ids through the production engine.

    Returns {(bootLBA, id): {sha256, size, ...}}: the key MUST include the
    volume, because the same numeric MFT record index exists in both volumes
    and means a completely different file.
    """
    result = {}
    run_tag = time.strftime("%Y%m%d_%H%M%S")
    for boot in sorted(ids_by_boot):
        ids = sorted(set(ids_by_boot[boot]))
        if not ids:
            continue
        # A fresh directory per run: previous replays leave zero-byte shells,
        # and the engine's output isolation restarts its sequence at 0.
        tag = "p07_replay_vol%d_%s" % (boot, run_tag)
        stage = os.path.join(STAGING, tag)
        os.makedirs(stage, exist_ok=True)
        with tempfile.TemporaryDirectory() as td:
            ids_file = os.path.join(td, "ids.txt")
            res_file = os.path.join(td, "result.csv")
            with open(ids_file, "w", encoding="utf-8") as f:
                f.write("\n".join(str(i) for i in ids) + "\n")
            if dry_run:
                print("dry-run: would replay %d ids for vol %d" %
                      (len(ids), boot))
                continue
            cmd = [ENGINE, "recover", "--disk", "2", "--boot", str(boot),
                   "--ids", ids_file, "--out-dir", stage,
                   "--result", res_file, "--engine-format", "1"]
            env = dict(os.environ)
            env["PATH"] = r"C:\msys64\mingw64\bin" + os.pathsep + env.get("PATH", "")
            r = subprocess.run(cmd, cwd=ROOT, env=env, capture_output=True,
                               text=True, encoding="utf-8", errors="replace")
            print("replay vol=%d ids=%d rc=%d :: %s" %
                  (boot, len(ids), r.returncode,
                   (r.stderr or "").strip().replace("\n", " | ")[-220:]))
            if not os.path.isfile(res_file):
                continue
            with open(res_file, newline="", encoding="utf-8",
                      errors="replace") as f:
                for row in csv.DictReader(f):
                    rid = int(row["id"])
                    if int(row["status"]) != 0:
                        result[(boot, rid)] = {"sha256": "", "size": 0,
                                               "status": int(row["status"]),
                                               "reason": row.get("reason", "")}
                        continue
                    p = os.path.join(ROOT, row["targetPath"].replace("\\", os.sep))
                    if os.path.isfile(p):
                        result[(boot, rid)] = {"sha256": sha_of(p),
                                               "size": os.path.getsize(p),
                                               "status": 0,
                                               "engineExt": row.get("engineExt", ""),
                                               "formatAgreement": row.get("formatAgreement", "")}
                    else:
                        result[(boot, rid)] = {"sha256": "", "size": 0,
                                               "status": 0, "reason": "replay file missing"}
        # Release replay staging data (keep zero-byte shells for final cleanup).
        if not dry_run:
            for d, _, fs in os.walk(stage):
                for n in fs:
                    p = os.path.join(d, n)
                    try:
                        with open(p, "r+b") as f:
                            f.truncate(0)
                    except OSError:
                        pass
    return result


def classify(row, truth, replay, gap_keys, fix_ok, boot):
    """Classify one anomaly row. gap_keys/replay keys are (bootLBA, id) pairs."""
    rid = int(row["id"])
    key = (boot, rid)
    eng_sha = row.get("engineSha256") or ""
    eng_size = int(row.get("engineSize") or 0)
    status = int(row.get("status") or 0)
    if not eng_sha and key in replay and replay[key].get("sha256"):
        eng_sha = replay[key]["sha256"]
        eng_size = replay[key]["size"]
    t_sha, t_size = truth.get("sha256", ""), truth.get("size", 0)

    if eng_sha and eng_sha == t_sha and eng_size == t_size:
        return "exactSha256Match", eng_sha, eng_size
    # Specific explanations BEFORE the generic zero-length one, otherwise a
    # deleted extension record is mislabeled as an ordinary empty file.
    if truth.get("kind") == "noPayloadDeletedExtension":
        return "noPayloadDeletedExtension", eng_sha, eng_size
    if t_size == 0 and status != 0:
        return "zeroLengthNoPayload", eng_sha, eng_size
    # P0-7 only fixed vol 63 / id 78. Never let that explanation leak into
    # another volume that happens to have the same MFT record number.
    if key in gap_keys and fix_ok:
        return "recoveredByP07AttributeListFix", eng_sha, eng_size
    return "UNRESOLVED", eng_sha, eng_size


def request_scope(declared, id_lines, hash_ids):
    """Reconcile the PLAN row count with the RECOVERY request count.

    `plan_engine.json::files` counts candidate ROWS, not distinct ids. An
    engine node id is an MFT record index (or `(rec<<4)|slot` for an ADS
    node), so two different nodes can share one numeric id when an ADS id
    collides with an ordinary MFT record. Asking the engine for that id twice
    recovers the SAME node set, so the byte-verification denominator must be
    the number of DISTINCT ids actually requested.

    The declared-vs-unique deficit is accepted ONLY when it is fully explained
    by literal duplicate lines in ids.txt AND the archived hashes.csv covers
    exactly that distinct id set. A genuinely missing id can never be excused
    as a "duplicate".
    """
    counts = {}
    for x in id_lines:
        counts[x] = counts.get(x, 0) + 1
    dups = {str(k): v - 1 for k, v in counts.items() if v > 1}
    n_unique = len(counts)
    errors = []
    if len(id_lines) != declared:
        errors.append("ids.txt has %d lines != declared requested=%d"
                      % (len(id_lines), declared))
    deficit = declared - n_unique
    if deficit != sum(dups.values()):
        errors.append("declared-unique deficit %d not fully explained by "
                      "%d duplicate id rows" % (deficit, sum(dups.values())))
    missing = set(counts) - set(hash_ids)
    extra = set(hash_ids) - set(counts)
    if missing:
        errors.append("hashes.csv is missing %d requested distinct ids (e.g. %s)"
                      % (len(missing), sorted(missing)[:5]))
    if extra:
        errors.append("hashes.csv has %d ids absent from ids.txt (e.g. %s)"
                      % (len(extra), sorted(extra)[:5]))
    return {
        "requestedDeclared": declared,
        "idsLines": len(id_lines),
        "requestedUniqueIds": n_unique,
        "duplicateRequestedIds": dups,
        "duplicateExtraRows": sum(dups.values()),
        "hashDistinctIds": len(hash_ids),
        "hashIdsEqualRequest": not missing and not extra,
    }, errors


def duplicate_scope_self_test():
    """Falsifiable gate: a real missing id must not pass as a duplicate."""
    legal, e1 = request_scope(4, [1, 2, 2, 3], {1, 2, 3})
    _, e2 = request_scope(4, [1, 2, 3], {1, 2, 3})          # one row missing
    _, e3 = request_scope(4, [1, 2, 2, 3], {1, 2, 3, 9})    # foreign hash id
    _, e4 = request_scope(4, [1, 2, 2, 3], {1, 3})          # hash id missing
    _, e5 = request_scope(5, [1, 2, 2, 3], {1, 2, 3})       # over-declared
    return (not e1 and legal["requestedUniqueIds"] == 3
            and all([e2, e3, e4, e5]))


def volume_scoping_ok(gap_keys, fix_ok):
    """Falsifiable self-test: a cross-volume gap explanation must be rejected.

    vol 209728638 also has an MFT record 78 ('...appinfo', 220 bytes) that is
    completely unrelated to vol 63's P0-7 attribute-list fix. With correct
    volume scoping that row stays UNRESOLVED; with the old global-id bug it is
    silently 'explained' by the wrong volume's fix. Both directions are tested.
    """
    fake_row = {"id": "78", "match": "0", "engineSha256": "",
                "engineSize": "0", "status": "1"}
    fake_truth = {"sha256": "deadbeef" * 8, "size": 220,
                  "kind": "baseRecordData"}
    if (63, 78) not in gap_keys or not fix_ok:
        return False                      # evidence missing -> cannot prove
    kind_ok, _, _ = classify(fake_row, fake_truth, {}, gap_keys,
                             fix_ok, 209728638)
    if kind_ok != "UNRESOLVED":
        return False                      # correct scoping must NOT explain it
    leaked = set(gap_keys) | {(209728638, 78)}   # simulate the old bug
    kind_bug, _, _ = classify(fake_row, fake_truth, {}, leaked,
                              fix_ok, 209728638)
    if kind_bug == "UNRESOLVED":
        return False                      # leak undetectable -> gate is useless
    return True


def build_record(no, replay, gap_keys, fix_ok, disk, vols, ads_by_boot,
                 declared_override=None):
    tag = "batch_%03d" % no
    rec = jload(os.path.join(BATCH_DIR, tag + ".json"), {})
    if not rec:
        return None, ["%s: record missing" % tag]
    st = rec.get("stats", {})
    boot = rec.get("volumeBootLBA")
    vol = vols.get(boot)
    if vol is None:
        return None, ["%s: no volume for bootLBA=%s" % (tag, boot)]
    rows = hash_rows(no)
    matched_ids = {int(r["id"]) for r in rows if r["match"] == "1"}
    checks, errors = [], []
    idp = os.path.join(BATCH_DIR, tag + "_ids.txt")
    if not os.path.isfile(idp):
        return None, ["%s: ids.txt missing" % tag]
    with open(idp, "r", encoding="utf-8") as f:
        id_lines = [int(x) for x in f if x.strip()]
    declared = int(st.get("requested", 0))
    if declared_override is not None:
        declared = declared_override
    scope, scope_errors = request_scope(
        declared, id_lines, {int(r["id"]) for r in rows})
    errors.extend("%s: %s" % (tag, e) for e in scope_errors)
    for row in rows:
        if row["match"] == "1" or int(row["id"]) in matched_ids:
            continue
        rid = int(row["id"])
        truth = resolve_truth(disk, vol, rid,
                              rid in ads_by_boot.get(boot, set()))
        if "error" in truth:
            errors.append("%s id %d truth: %s" % (tag, rid, truth["error"]))
            checks.append({"id": rid, "classification": "UNRESOLVED",
                           "error": truth["error"]})
            continue
        kind, e_sha, e_size = classify(row, truth, replay, gap_keys,
                                       fix_ok, boot)
        c = {"id": rid, "classification": kind,
             "truthKind": truth.get("kind"),
             "truthSize": truth.get("size"),
             "truthSha256": truth.get("sha256"),
             "engineSizeArchived": int(row.get("engineSize") or 0),
             "engineSha256Archived": row.get("engineSha256") or "",
             "engineSizeUsed": e_size, "engineSha256Used": e_sha,
             "oracleSizeArchived": int(row.get("oracleSize") or 0),
             "oracleSha256Archived": row.get("oracleSha256") or "",
             "status": int(row.get("status") or 0)}
        if truth.get("streamName"):
            c["adsStreamName"] = truth["streamName"]
        checks.append(c)
        if kind == "UNRESOLVED":
            errors.append("%s id %d: no byte-level explanation" % (tag, rid))

    counts = {}
    for c in checks:
        counts[c["classification"]] = counts.get(c["classification"], 0) + 1
    direct = int(st.get("matched", 0))
    covered = matched_ids | {int(c["id"]) for c in checks}
    if covered != set(id_lines):
        errors.append("%s: verified id set (%d) != requested distinct ids (%d)"
                      % (tag, len(covered), len(set(id_lines))))
    eff = {
        # Denominator = DISTINCT ids actually sent to the engine. The plan's
        # `files` is a ROW count and may contain duplicate numeric ids.
        "requested": scope["requestedUniqueIds"],
        "requestedDeclared": declared,
        "directMatched": direct,
        "anomalyRows": len(checks),
        "explained": len(checks) - counts.get("UNRESOLVED", 0),
        "effectiveVerified": direct + len(checks) - counts.get("UNRESOLVED", 0),
        "unresolved": counts.get("UNRESOLVED", 0),
        "classifications": counts,
    }
    if eff["effectiveVerified"] != eff["requested"]:
        errors.append("%s effective %d != requested distinct %d (declared %d)"
                      % (tag, eff["effectiveVerified"], eff["requested"],
                         declared))
    doc = {
        "batch": no, "kind": "engine-plan-byte-verification",
        "generatedAt": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "generatedBy": "tools/verify_engine_plan_batch.py",
        "purpose": "Explain engine-driven batch anomalies WITHOUT rewriting "
                   "any archived record; truth re-parsed read-only from the "
                   "source disk, missing engine hashes obtained by a fresh "
                   "production-engine replay.",
        "originalStats": st,
        "originalRecordsUnmodified": True,
        "requestScope": scope,
        "volumeBootLBA": boot,
        "replayEngine": os.path.basename(ENGINE),
        "checks": checks,
        "effectiveStats": eff,
        "conclusion": ("ENGINE BYTE RECOVERY VERIFIED FOR ALL %d REQUESTED IDS"
                       % eff["requested"]) if not errors
                      else "UNRESOLVED ANOMALIES REMAIN",
        "sourceDiskWriteOperations": 0,
        "errors": errors,
    }
    return doc, errors


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--batch", type=int)
    ap.add_argument("--all-completed", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--negative-guard", action="store_true",
                    help="remove the P0-7 explanation; MUST fail")
    ap.add_argument("--negative-guard-cross-volume", action="store_true",
                    help="simulate the old global-id leak; MUST fail")
    ap.add_argument("--negative-guard-duplicate-ids", action="store_true",
                    help="over-declare one requested id; MUST fail (a real "
                         "missing id may not be excused as a duplicate)")
    args = ap.parse_args()

    cp = jload(os.path.join(OUT, "checkpoint.json"), {})
    done = sorted(cp.get("completed", []))
    if args.all_completed:
        targets = [n for n in done if n >= 4]
    elif args.batch:
        targets = [args.batch]
    else:
        ap.error("specify --batch N or --all-completed")

    gap = jload(GAP_EVIDENCE, {})
    fix = jload(FIX_EVIDENCE, {})
    # Only REAL, volume-scoped gaps qualify. `noPayloadDeletedExtension` nodes
    # are excluded (they have an explicit classification) and so are errors.
    gap_keys = set()
    for v in gap.get("volumes", []):
        boot = int(v.get("bootLBA") or 0)
        for n in v.get("nodes", []):
            if ("classification" in n or "error" in n or n.get("noPayload")
                    or int(n.get("truthSize") or 0) <= 0
                    or not n.get("truthSha256")):
                continue
            gap_keys.add((boot, int(n["id"])))
    # Do not trust the verdict STRING (it uses ATTRIBUTE_LIST with an
    # underscore). Re-derive the fix from its byte-level fields instead.
    fix_ok = (not fix.get("errors")
              and not fix.get("negativeGuard")
              and fix.get("gapId") == 78
              and bool(fix.get("engineRecoveredSha256"))
              and fix.get("engineRecoveredSha256")
              == fix.get("independentTruthSha256")
              == fix.get("expectedSha256Used")
              and fix.get("engineRecoveredSize")
              == fix.get("independentTruthSize")
              and int(fix.get("beforeFixClusterCount", -1)) == 0
              and int(fix.get("afterFixClusterCount", 0)) > 0)
    ads_by_boot = load_ads_ids()

    if args.negative_guard_cross_volume:
        # Re-create the pre-fix bug: the vol-63 gap is visible from every
        # volume. volume_scoping_ok() must then detect the leak and reject.
        for boot, rid in list(gap_keys):
            for other in (63, 209728638):
                gap_keys.add((other, rid))
        print("NEGATIVE GUARD cross-volume: leaked gap keys -> %s" %
              sorted(gap_keys))
        ok = volume_scoping_ok(gap_keys, fix_ok)
        print("VOLUME SCOPING SELF-TEST -> %s" %
              ("PASSED (leak NOT detected)" if ok else "FAILED (leak detected)"))
        return 0 if ok else 1

    if not volume_scoping_ok(gap_keys, fix_ok):
        print("ERROR: volume scoping self-test failed; refusing to verify")
        return 1
    print("volume scoping self-test: PASS (%d volume-scoped gap keys)" %
          len(gap_keys))
    if not duplicate_scope_self_test():
        print("ERROR: duplicate-id scope self-test failed; refusing to verify")
        return 1
    print("duplicate-id scope self-test: PASS")

    # Collect ids whose archived row has no engine SHA-256 -> need replay.
    ids_by_boot = {}
    for no in targets:
        rec = jload(os.path.join(BATCH_DIR, "batch_%03d.json" % no), {})
        boot = rec.get("volumeBootLBA")
        ids = []
        for r in hash_rows(no):
            if r["match"] == "0" and not r.get("engineSha256"):
                ids.append(int(r["id"]))
        if ids:
            ids_by_boot.setdefault(boot, []).extend(ids)
    if args.negative_guard:
        # Remove the P0-7 explanation. Batch 4's id 78 must then become an
        # UNRESOLVED anomaly, proving the gate really re-derives its verdict.
        gap_keys, fix_ok = set(), False
    replay = replay_ids(ids_by_boot, args.dry_run)
    if args.dry_run:
        return 0
    if args.negative_guard and (63, 78) in replay and replay[(63, 78)].get("sha256"):
        s = replay[(63, 78)]["sha256"]
        replay[(63, 78)]["sha256"] = ("0" if s[:1] != "0" else "1") + s[1:]

    disk = mo.Disk()
    overall = 0
    try:
        vols = {}
        for no in targets:
            rec = jload(os.path.join(BATCH_DIR, "batch_%03d.json" % no), {})
            boot = rec.get("volumeBootLBA")
            if boot not in vols:
                vols[boot] = mo.Volume(disk, boot)
            declared = int(rec.get("stats", {}).get("requested", 0))
            override = (declared + 1 if args.negative_guard_duplicate_ids
                        else None)
            doc, errors = build_record(no, replay, gap_keys, fix_ok, disk,
                                       vols, ads_by_boot,
                                       declared_override=override)
            if doc is None:
                print("batch_%03d: %s" % (no, errors))
                overall |= 1
                continue
            dest = os.path.join(BATCH_DIR,
                                "batch_%03d_engine_plan_verification.json" % no)
            if not args.negative_guard and not args.negative_guard_duplicate_ids:
                with open(dest, "w", encoding="utf-8") as f:
                    json.dump(doc, f, indent=2, ensure_ascii=False)
            eff = doc["effectiveStats"]
            print("batch_%03d: requested=%d direct=%d explained=%d "
                  "effective=%d/%d unresolved=%d %s" %
                  (no, eff["requested"], eff["directMatched"],
                   eff["explained"], eff["effectiveVerified"],
                   eff["requested"], eff["unresolved"],
                   "OK" if not errors else "INCOMPLETE"))
            for c in doc["checks"][:8]:
                print("   id=%-10d %-34s truth=%s engine=%s" %
                      (c["id"], c["classification"],
                       str(c.get("truthSha256", ""))[:12],
                       str(c.get("engineSha256Used", ""))[:12]))
            if errors:
                overall |= 1
                for e in errors[:6]:
                    print("   ERROR:", e)
            else:
                print("   -> " + os.path.basename(dest))
    finally:
        disk.close()
    if args.negative_guard or args.negative_guard_duplicate_ids:
        which = ("P0-7 explanation removed" if args.negative_guard
                 else "one requested id over-declared")
        print("NEGATIVE GUARD: %s -> %s" %
              (which, "FAIL as expected" if overall else "DID NOT TRIP"))
        return 1 if overall else 0
    return overall


if __name__ == "__main__":
    sys.exit(main())
