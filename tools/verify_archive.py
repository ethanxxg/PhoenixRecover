#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Verify completeness and internal consistency of the real-disk archive.

Why this exists: the user requires per-batch recovery into a project path,
durable recording of results, then release of that path before the next batch.
Once the file bodies are gone, the ONLY remaining evidence is the archived
records. This tool is the final gate that proves those records are complete and
self-consistent, so the final report cannot over-claim.

Pure post-processing: reads ONLY files under docs/reports/ and inspects the
project-owned recover_staging/ tree. It never touches the source disk and never
modifies any batch record.

Usage:
    python tools/verify_archive.py --allow-partial    # while batches are running
    python tools/verify_archive.py                   # final strict gate
"""
import argparse
import csv
import json
import os
import re

ROOT = r"D:\dev\PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
BATCH_DIR = os.path.join(OUT, "batches")
STAGING_ROOT = os.path.join(ROOT, "recover_staging")
SHA_RE = re.compile(r"[0-9a-f]{64}\Z")


def jload(path, default=None):
    if not os.path.isfile(path):
        return default
    with open(path, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--allow-partial", action="store_true",
                    help="treat an incomplete checkpoint as INFO, not an error")
    ap.add_argument("--out", default=os.path.join(OUT, "archive_verification.json"))
    args = ap.parse_args()

    errors, warnings, infos = [], [], []

    def err(m):
        errors.append(m)

    def warn(m):
        warnings.append(m)

    def info(m):
        infos.append(m)

    # ---- 1. plan integrity -------------------------------------------------
    # P0-6: batches 1-3 come from the historical oracle-driven plan.json;
    # batches 4-7 come from the ENGINE-driven plan_engine.json. Merge by batch
    # number so checkpoint [1..7] is still verifiable. Do not rewrite either
    # plan: both are immutable historical evidence.
    plan = jload(os.path.join(OUT, "plan.json"), {})
    ep = jload(os.path.join(OUT, "plan_engine.json"), {})
    if not plan:
        err("plan.json missing")
        batches, total = [], 0
    else:
        batches = plan.get("batches", [])
        total = plan.get("totalBatches", 0)
        nos = [b["no"] for b in batches]
        if total != len(batches):
            err("plan.json: totalBatches=%d but %d batch entries" %
                (total, len(batches)))
        if sum(b["files"] for b in batches) != plan.get("totalFiles"):
            err("plan.json: sum(batch.files) != totalFiles")
        if sum(b["bytes"] for b in batches) != plan.get("totalBytes"):
            err("plan.json: sum(batch.bytes) != totalBytes")
        info("plan.json: %d batches / %d files / %d bytes (historical)" %
             (total, plan.get("totalFiles", 0), plan.get("totalBytes", 0)))

        if ep.get("candidateSource") == "engine":
            eb = ep.get("batches", [])
            idx = {int(b["no"]): b for b in batches}
            for b in eb:
                idx[int(b["no"])] = b
            batches = [idx[k] for k in sorted(idx)]
            total = max(total, max(idx) if idx else 0)
            if sorted(idx) != list(range(1, total + 1)):
                err("merged batch numbers are not contiguous 1..%d" % total)
            if sum(b["files"] for b in eb) != ep.get("totalFiles"):
                err("plan_engine.json: sum(batch.files) != totalFiles")
            if sum(b["bytes"] for b in eb) != ep.get("totalBytes"):
                err("plan_engine.json: sum(batch.bytes) != totalBytes")
            info("plan_engine.json: %d remaining batches / %d files / %d bytes "
                 "(candidateSource=engine); merged total=%d batches" %
                 (len(eb), ep.get("totalFiles", 0), ep.get("totalBytes", 0),
                  total))
        elif nos != list(range(1, total + 1)):
            err("batch numbers are not a contiguous 1..%d sequence" % total)
        info("merged plan: %d batches" % total)

    # ---- 1b. engine byte-verification records (P0-5) ----------------------
    # A batch may legitimately carry mismatches/failures in its ORIGINAL,
    # immutable record when both causes were later audited:
    #   * output-path collisions -> a full retry with 100% SHA-256 match
    #   * oracle ADS bug         -> truth recomputed directly from MFT on disk
    # Never trust the audit file's own conclusion flag; re-derive every number.
    def ev_valid(no, st):
        tag = "batch_%03d" % no
        ev = jload(os.path.join(BATCH_DIR, tag + "_engine_verification.json"))
        if not ev:
            return False, "%s: anomalies have no engine_verification.json" % tag
        if ev.get("originalStats") != st:
            return False, "%s: verification originalStats != archived stats" % tag
        eff = ev.get("effectiveStats", {})
        if (eff.get("effectiveVerified") != st.get("requested")
                or eff.get("unresolved") != 0):
            return False, "%s: verification does not cover every requested id" % tag
        checks = ev.get("correctedOracleChecks", [])
        if not checks or any(
                not c.get("match")
                or c.get("oracleSha256") != c.get("engineSha256")
                or c.get("expectedSize") != c.get("engineSize")
                or not c.get("adsSkipped") for c in checks):
            return False, "%s: corrected-oracle checks are not exact SHA-256 matches" % tag
        f = ev.get("findings", {})
        rs = f.get("engineOutputCollision", {}).get("retryStats", {})
        if (rs.get("matched") != st.get("failed") or rs.get("mismatched")
                or rs.get("failed") or rs.get("missingOracle")):
            return False, "%s: retry record does not fully explain the failures" % tag
        if f.get("adsNodeIdCollision", {}).get("count") != st.get("duplicateIdNodes"):
            return False, "%s: ADS id-collision count mismatch" % tag
        return True, ("%s: anomalies explained -> %d corrected oracle + %d retry "
                      "+ %d ADS-id rows, effective %d/%d verified" %
                      (tag, eff.get("correctedOracleMatched", 0),
                       rs.get("matched", 0), st.get("duplicateIdNodes", 0),
                       eff.get("effectiveVerified", 0), st.get("requested", 0)))

    # ---- 1c. engine-plan byte-verification records (P0-6/P0-7) ------------
    # Batches 4-7 take ids from the ENGINE. Their archived rows can be
    # match=0 because the historical oracle has no truth for an ADS node, a
    # zero-length file, or (batch 4) the P0-6 $ATTRIBUTE_LIST gap. The new
    # record is again re-derived from the raw hashes CSV; its own conclusion
    # string is never trusted.
    def ep_valid(no, st):
        tag = "batch_%03d" % no
        ep = jload(os.path.join(BATCH_DIR,
                                tag + "_engine_plan_verification.json"), {})
        if not ep:
            return False, "%s: no engine-plan verification record" % tag
        if ep.get("originalStats") != st:
            return False, "%s: engine-plan originalStats != archived stats" % tag
        hp = os.path.join(BATCH_DIR, tag + "_hashes.csv")
        with open(hp, newline="", encoding="utf-8") as f:
            rows = list(csv.DictReader(f))
        matched = {r["id"] for r in rows if r["match"] == "1"}
        want = {r["id"] for r in rows
                if r["match"] == "0" and r["id"] not in matched}
        checks = ep.get("checks", [])
        # P0-9: re-derive the request denominator from ids.txt. `plan.files`
        # counts candidate ROWS; a duplicated numeric id (ADS id colliding
        # with an MFT record index) is the SAME recovery request twice. The
        # declared-unique deficit is legal ONLY if ids.txt really repeats it.
        idp = os.path.join(BATCH_DIR, tag + "_ids.txt")
        if not os.path.isfile(idp):
            return False, "%s: ids.txt missing" % tag
        with open(idp, "r", encoding="utf-8") as f:
            id_lines = [int(x) for x in f if x.strip()]
        counts = {}
        for x in id_lines:
            counts[x] = counts.get(x, 0) + 1
        dups = {str(k): v - 1 for k, v in counts.items() if v > 1}
        n_unique = len(counts)
        declared = int(st.get("requested", 0))
        if (len(id_lines) != declared
                or declared - n_unique != sum(dups.values())):
            return False, ("%s: declared/unique id scope inconsistent "
                           "(lines=%d declared=%d unique=%d dups=%d)"
                           % (tag, len(id_lines), declared, n_unique,
                              sum(dups.values())))
        if {int(r["id"]) for r in rows} != set(counts):
            return False, "%s: hashes.csv id set != requested distinct ids" % tag
        rs = ep.get("requestScope", {})
        if (rs.get("requestedDeclared") != declared
                or rs.get("idsLines") != len(id_lines)
                or rs.get("requestedUniqueIds") != n_unique
                or rs.get("duplicateRequestedIds") != dups
                or rs.get("duplicateExtraRows") != sum(dups.values())
                or rs.get("hashDistinctIds") != n_unique
                or rs.get("hashIdsEqualRequest") is not True):
            return False, "%s: engine-plan requestScope not reproducible" % tag
        # A P0-7 explanation is valid ONLY for vol 63 / id 78. Both volumes
        # have an MFT record 78; without this check a cross-volume leak would
        # silently "explain" an unrelated file.
        rec = jload(os.path.join(BATCH_DIR, tag + ".json"), {})
        if rec and ep.get("volumeBootLBA") != rec.get("volumeBootLBA"):
            return False, ("%s: engine-plan volumeBootLBA=%s != archived %s"
                           % (tag, ep.get("volumeBootLBA"),
                              rec.get("volumeBootLBA")))
        got = {str(c["id"]) for c in checks}
        if got != want:
            return False, ("%s: engine-plan checks %d rows != %d match=0 ids"
                           % (tag, len(got), len(want)))
        allowed = {"exactSha256Match", "zeroLengthNoPayload",
                   "noPayloadDeletedExtension",
                   "recoveredByP07AttributeListFix"}
        for c in checks:
            if c.get("classification") not in allowed:
                return False, "%s id %s: unexplained %s" % (
                    tag, c.get("id"), c.get("classification"))
            if c.get("classification") == "exactSha256Match":
                if (c.get("truthSha256") != c.get("engineSha256Used")
                        or c.get("truthSize") != c.get("engineSizeUsed")
                        or not c.get("engineSha256Used")):
                    return False, ("%s id %s: claimed SHA match is not exact"
                                   % (tag, c.get("id")))
            if c.get("classification") == "recoveredByP07AttributeListFix":
                if (ep.get("volumeBootLBA") != 63
                        or int(c.get("id", -1)) != 78):
                    return False, ("%s id %s: P0-7 explanation used outside "
                                   "vol 63 / id 78" % (tag, c.get("id")))
        eff = ep.get("effectiveStats", {})
        if (eff.get("unresolved") != 0
                or eff.get("effectiveVerified") != n_unique
                or eff.get("requested") != n_unique
                or eff.get("requestedDeclared") != declared
                or eff.get("explained") != len(checks)):
            return False, "%s: engine-plan effective stats do not cover all ids" % tag
        return True, ("%s: engine-plan anomalies explained -> %d/%d distinct ids "
                      "(declared %d incl. %d duplicate rows), classifications=%s" %
                      (tag, eff.get("effectiveVerified"), n_unique, declared,
                       sum(dups.values()),
                       json.dumps(eff.get("classifications", {}),
                                  ensure_ascii=False, sort_keys=True)))

    def explain_anomalies(no, st):
        ok, msg = ev_valid(no, st)
        if ok:
            return ok, msg
        ok2, msg2 = ep_valid(no, st)
        return (ok2, msg2) if ok2 else (False, msg + " | " + msg2)

    # ---- 2. checkpoint integrity -------------------------------------------
    cp = jload(os.path.join(OUT, "checkpoint.json"), {})
    done = sorted(cp.get("completed", [])) if cp else []
    if not cp:
        err("checkpoint.json missing")
    if len(set(done)) != len(done):
        err("duplicate batch numbers in checkpoint.completed")
    if any(n not in [b["no"] for b in batches] for n in done):
        err("checkpoint references a batch not present in plan")
    if len(done) < total:
        msg = "checkpoint incomplete: %d/%d batches" % (len(done), total)
        (info if args.allow_partial else err)(msg)
    t = cp.get("totals", {}) if cp else {}
    # batch_recover.py:369 accumulates stats.matched (directly verified files),
    # NOT plan.requested. A batch with explained anomalies contributes fewer.
    expect_bytes = sum(b["bytes"] for b in batches if b["no"] in done)
    expect_matched = 0
    for no in done:
        r = jload(os.path.join(BATCH_DIR, "batch_%03d.json" % no), {})
        expect_matched += int(r.get("stats", {}).get("matched", 0))
    if t.get("matched") != t.get("files"):
        err("checkpoint totals.matched != totals.files")
    if t.get("files") != expect_matched:
        err("checkpoint totals.files=%s != sum(stats.matched)=%d" %
            (t.get("files"), expect_matched))
    if t.get("bytes") != expect_bytes:
        err("checkpoint totals.bytes=%s != plan sum of completed=%d" %
            (t.get("bytes"), expect_bytes))

    explained_m = explained_f = 0
    for no in done:
        st = jload(os.path.join(BATCH_DIR, "batch_%03d.json" % no), {}).get("stats", {})
        if st.get("mismatched") or st.get("failed"):
            ok, msg = explain_anomalies(no, st)
            (info if ok else err)(msg)
            if ok:
                explained_m += int(st.get("mismatched", 0))
                explained_f += int(st.get("failed", 0))
    if t.get("mismatched") or t.get("failed"):
        if (t.get("mismatched") == explained_m
                and t.get("failed") == explained_f
                and explained_m + explained_f > 0):
            info("checkpoint anomalies %d mismatched / %d failed: fully explained "
                 "by engine verification records (original records untouched)" %
                 (explained_m, explained_f))
        else:
            err("checkpoint records unexplained mismatches=%s / failures=%s" %
                (t.get("mismatched"), t.get("failed")))
    info("checkpoint: completed=%s totals=%s" % (done, json.dumps(t)))

    # ---- 3. per-batch record / hash / staging checks ------------------------
    per_batch = []
    for b in batches:
        no = b["no"]
        tag = "batch_%03d" % no
        item = {"batch": no, "inCheckpoint": no in done,
                "volumeBootLBA": b.get("volumeBootLBA"),
                "plannedFiles": b["files"], "plannedBytes": b["bytes"]}
        if no not in done:
            item["status"] = "pending"
            per_batch.append(item)
            continue

        rec = jload(os.path.join(BATCH_DIR, tag + ".json"))
        if not rec:
            err("%s: record JSON missing" % tag)
            item["status"] = "missing-record"
            per_batch.append(item)
            continue
        s = rec.get("stats", {})
        item["stats"] = s
        item["stagingReleased"] = bool(rec.get("stagingReleased"))
        item["stagingReleaseMode"] = rec.get("stagingReleaseMode", "legacy")

        has_anomaly = bool(s.get("mismatched") or s.get("failed")
                           or s.get("missingOracle")
                           or s.get("matched") != s.get("requested"))
        is_engine_plan = (no >= 4 and ep.get("candidateSource") == "engine")
        ev_good, ev_msg = (True, "")
        if has_anomaly:
            ev_good, ev_msg = explain_anomalies(no, s)
            (info if ev_good else err)(ev_msg)
        item["engineVerification"] = ("verified" if ev_good else "FAILED") if has_anomaly else "not-needed"
        item["candidateSource"] = "engine" if is_engine_plan else "oracle"

        if s.get("requested") != b["files"]:
            err("%s: stats.requested=%s != plan files=%d" %
                (tag, s.get("requested"), b["files"]))
        if s.get("matched") != s.get("requested"):
            if not ev_good:
                err("%s: matched=%s != requested=%s (unexplained)" %
                    (tag, s.get("matched"), s.get("requested")))
        if s.get("missingOracle"):
            if is_engine_plan and ev_good:
                info("%s: missingOracle=%s explained by engine-plan "
                     "byte verification (engine-only ADS nodes have no row "
                     "in the historical oracle candidate CSV)" %
                     (tag, s.get("missingOracle")))
            else:
                err("%s: missingOracle=%s is never excusable" %
                    (tag, s.get("missingOracle")))
        if s.get("mismatched") or s.get("failed"):
            if not ev_good:
                err("%s: mismatched=%s failed=%s unexplained" %
                    (tag, s.get("mismatched"), s.get("failed")))
        if not rec.get("stagingReleased"):
            err("%s: staging not marked released" % tag)

        # requested id list must match the plan size
        idp = os.path.join(BATCH_DIR, tag + "_ids.txt")
        if not os.path.isfile(idp):
            err("%s: ids.txt missing" % tag)
        else:
            with open(idp, "r", encoding="utf-8") as f:
                n_ids = sum(1 for line in f if line.strip())
            item["idsLines"] = n_ids
            if n_ids != b["files"]:
                err("%s: ids.txt has %d lines != %d planned" %
                    (tag, n_ids, b["files"]))

        # engine result and oracle truth files must exist
        for suffix, label in (("_engine_result.csv", "engine result"),
                              ("_oracle_hashes.json", "oracle hashes")):
            if not os.path.isfile(os.path.join(BATCH_DIR, tag + suffix)):
                err("%s: %s missing" % (tag, label))

        # per-file hash CSV: fully streamed (batch 6 has ~466k rows)
        hp = os.path.join(BATCH_DIR, tag + "_hashes.csv")
        if not os.path.isfile(hp):
            err("%s: hashes.csv missing" % tag)
        else:
            rows = hits = bad = 0
            with open(hp, newline="", encoding="utf-8") as f:
                for r in csv.DictReader(f):
                    rows += 1
                    if r.get("match") == "1":
                        hits += 1
                        osz, esz = r.get("oracleSize"), r.get("engineSize")
                        osh, esh = r.get("oracleSha256"), r.get("engineSha256")
                        if osz != esz or osh != esh or not SHA_RE.match(esh or ""):
                            bad += 1
            item["hashRows"] = rows
            item["hashMatches"] = hits
            if rows != s.get("engineRows"):
                err("%s: hashes.csv rows=%d != stats.engineRows=%s" %
                    (tag, rows, s.get("engineRows")))
            if hits != s.get("matched"):
                err("%s: hashes.csv match=1 rows=%d != stats.matched=%s" %
                    (tag, hits, s.get("matched")))
            if bad:
                err("%s: %d rows have inconsistent size/hash for a claimed match" %
                    (tag, bad))
            # Rows with match=0 are the surplus FileNode::id-collision nodes;
            # they must be accounted for by duplicateIdNodes, never hidden.
            surplus = rows - hits
            declared = int(s.get("duplicateIdNodes", 0))
            # Each failed row, each false-mismatch row, and each surplus ADS-id
            # collision node contributes exactly one match=0 row.
            expected_surplus = (int(s.get("failed", 0))
                                + int(s.get("mismatched", 0)) + declared
                                + (int(s.get("missingOracle", 0))
                                   if is_engine_plan else 0))
            if surplus > expected_surplus:
                warn("%s: %d unmatched hash rows > %d expected "
                     "(failed+mismatched+ADS-id)" % (tag, surplus, expected_surplus))
            else:
                info("%s: %d match=0 rows = %d failed + %d false mismatch + %d id-collision" %
                     (tag, surplus, s.get("failed", 0), s.get("mismatched", 0), declared))

        # staging space must actually be released
        stage = os.path.join(STAGING_ROOT, tag)
        if os.path.isdir(stage):
            nfiles = nonzero = 0
            for root, _subdirs, files in os.walk(stage):
                for name in files:
                    nfiles += 1
                    try:
                        if os.path.getsize(os.path.join(root, name)):
                            nonzero += 1
                    except OSError:
                        pass
            item["stagingFiles"] = nfiles
            item["stagingNonZeroBytesFiles"] = nonzero
            if nonzero:
                err("%s: %d staging files still hold data (space NOT released)" %
                    (tag, nonzero))
            else:
                info("%s: %d zero-byte shells retained until final cleanup" %
                     (tag, nfiles))
        else:
            item["stagingFiles"] = 0
            item["stagingNonZeroBytesFiles"] = 0
            info("%s: staging tree fully deleted" % tag)

        item["status"] = "verified"
        per_batch.append(item)

    # ---- 4. zero-write forensic proof --------------------------------------
    baseline = jload(os.path.join(OUT, "baseline.json"), {})
    verify = jload(os.path.join(OUT, "baseline_verify.json"), {})
    samples = baseline.get("samples", []) if baseline else []
    if not baseline:
        err("baseline.json missing")
    if not samples:
        err("baseline.json has no sampling points")
    read_fail = sum(1 for s in samples if not s.get("readOk", False))
    vres = verify.get("result", {}) if verify else {}
    if not verify:
        (info if args.allow_partial else err)("baseline_verify.json missing: final --verify not run")
    else:
        if vres.get("conclusion") != "SOURCE DISK UNCHANGED":
            err("final verify conclusion=%r" % vres.get("conclusion"))
        if vres.get("identityUnchanged") is not True:
            err("final verify identityUnchanged != true")
        if vres.get("changed"):
            err("final verify changed=%s" % json.dumps(vres.get("changed")))
        if vres.get("samplesCompared") != len(samples):
            err("final verify compared %s points, baseline has %d" %
                (vres.get("samplesCompared"), len(samples)))
        if verify.get("verifiedAtUtc", "") <= baseline.get("generatedAtUtc", ""):
            err("final verify timestamp is not later than baseline capture")
        info("zero-write: %s @ %s (%s points, changed=%s)" %
             (vres.get("conclusion"), verify.get("verifiedAtUtc"),
              vres.get("samplesCompared"), json.dumps(vres.get("changed"))))
    info("baseline: %d sampling points / %d read failures" %
         (len(samples), read_fail))

    # ---- 5. verdict ---------------------------------------------------------
    all_batches = bool(total) and len(done) == total
    if errors:
        conclusion = "ARCHIVE VERIFICATION FAILED"
    elif not all_batches:
        conclusion = "ARCHIVE PARTIAL (batches still running)"
    else:
        conclusion = "ARCHIVE COMPLETE AND VERIFIED"

    report = {
        "conclusion": conclusion,
        "strictMode": not args.allow_partial,
        "batchesCompleted": len(done),
        "batchesPlanned": total,
        "checkpoint": {"completed": done, "totals": t},
        "zeroWrite": {
            "baselineSamples": len(samples),
            "baselineReadFailures": read_fail,
            "verifiedAtUtc": verify.get("verifiedAtUtc"),
            "result": vres,
        },
        "batches": per_batch,
        "errors": errors,
        "warnings": warnings,
        "info": infos,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    with open(args.out, "r", encoding="utf-8") as f:
        assert json.load(f)["conclusion"] == conclusion  # durable write check

    print("=== %s ===" % conclusion)
    print("batches %d/%d | errors=%d warnings=%d" %
          (len(done), total, len(errors), len(warnings)))
    for m in errors:
        print("  ERROR: %s" % m)
    for m in warnings:
        print("  WARN : %s" % m)
    for m in infos:
        print("  info : %s" % m)
    print("report -> %s" % args.out)
    return 0 if not errors else 1


if __name__ == "__main__":
    raise SystemExit(main())
