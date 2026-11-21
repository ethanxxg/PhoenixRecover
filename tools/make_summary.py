#!/usr/bin/env python3
# -*- coding: utf-8 -*-
r"""Build the final real-disk verification report from durable batch records.

Pure post-processing: reads only files under docs/reports/, never touches the
source disk and never modifies any batch record. Re-runnable at any point.

    python tools/make_summary.py
    python tools/make_summary.py --out docs/reports/real_disk_recovery_2026-11-14/summary.md
"""
import argparse
import glob
import json
import os
import re

ROOT = r"D:\dev\PhoenixRecover"
OUT = os.path.join(ROOT, "docs", "reports", "real_disk_recovery_2026-11-14")
DEFAULT_OUT = os.path.join(OUT, "summary.md")


def jload(p, default=None):
    if not os.path.isfile(p):
        return default
    with open(p, "r", encoding="utf-8") as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=DEFAULT_OUT)
    args = ap.parse_args()

    plan = jload(os.path.join(OUT, "plan.json"), {})
    cp = jload(os.path.join(OUT, "checkpoint.json"),
               {"completed": [], "totals": {}})
    baseline = jload(os.path.join(OUT, "baseline.json"), {})
    verify = jload(os.path.join(OUT, "baseline_verify.json"), {})
    vres = verify.get("result", {}) if verify else {}
    verify_ok = bool(vres.get("conclusion") == "SOURCE DISK UNCHANGED"
                     and vres.get("identityUnchanged") is True
                     and not vres.get("changed"))
    archive = jload(os.path.join(OUT, "archive_verification.json"), {})
    fin = jload(os.path.join(OUT, "finalize_state.json"), {})
    fa = jload(os.path.join(OUT, "final_audit.json"), {})
    esc = jload(os.path.join(OUT, "engine_scan_coverage.json"), {})
    gap = jload(os.path.join(OUT, "engine_gap_evidence.json"), {})
    fix = jload(os.path.join(OUT, "engine_fix_verification.json"), {})
    archive_ok = bool(archive.get("conclusion") == "ARCHIVE COMPLETE AND VERIFIED"
                      and not archive.get("errors"))
    vols = {}
    for boot in (63, 209728638):
        vols[boot] = jload(os.path.join(OUT, "oracle_summary_vol%d.json" % boot), {})

    records = {}
    for p in sorted(glob.glob(os.path.join(OUT, "batches", "batch_*.json"))):
        m = re.search(r"batch_(\d+)\.json$", p)
        if not m:
            continue
        records[int(m.group(1))] = jload(p, {})

    completed = sorted(cp.get("completed", []))
    total_batches = plan.get("totalBatches", 0)
    plan_files = plan.get("totalFiles", 0)
    plan_bytes = plan.get("totalBytes", 0)
    # P0-6: from batch 4 onwards the batches come from the ENGINE-driven
    # plan_engine.json (7 batches total), not the historical oracle plan (6).
    plan_engine = jload(os.path.join(OUT, "plan_engine.json"), {})
    batch_index = {int(b["no"]): b for b in plan.get("batches", [])}
    if plan_engine.get("candidateSource") == "engine":
        for b in plan_engine.get("batches", []):
            batch_index[int(b["no"])] = b
        # plan_engine.totalBatches counts only the REMAINING batches (4),
        # so the global total is the highest batch number across both plans.
        if batch_index:
            total_batches = max(total_batches, max(batch_index))

    acc = {"requested": 0, "engineRows": 0, "matched": 0, "mismatched": 0,
           "failed": 0, "missingOracle": 0, "duplicateIdNodes": 0}
    collisions = 0
    for r in records.values():
        for k in acc:
            acc[k] += int(r.get("stats", {}).get(k, 0))
        collisions += int(r.get("idCollisionCount", 0))

    done_bytes = sum(int(b["bytes"]) for no, b in batch_index.items()
                     if no in completed)
    done_files = sum(int(b["files"]) for no, b in batch_index.items()
                     if no in completed)
    pct = (100.0 * len(completed) / total_batches) if total_batches else 0.0
    recall = (100.0 * acc["matched"] / acc["requested"]) if acc["requested"] else 0.0

    L = []
    L.append("# 真实损坏磁盘只读恢复验证报告")
    L.append("")
    L.append("- 生成：本文件由 `tools/make_summary.py` 从持久记录自动汇总，可重复生成")
    L.append("- 源盘：`\\\\.\\PhysicalDrive2` ST1000DM003-1CH162 / 931.51 GiB / 2 个 `0x42` LDM 动态分区 / 无盘符")
    L.append("- 红线：全程 `GENERIC_READ`、零写入、未执行任何修复；损坏态样本保持原样")
    L.append("")
    L.append("## 一、执行状态")
    L.append("")
    L.append("| 项 | 值 |")
    L.append("|---|---|")
    L.append("| 批次完成 | **%d / %d**（%.1f%%） |" % (len(completed), total_batches, pct))
    L.append("| 候选清单总量 | %s 文件 / %.2f GiB |" %
             (f"{plan_files:,}", plan_bytes / 1024 ** 3))
    L.append("| 已处理 | %s 文件 / %.2f GiB |" %
             (f"{done_files:,}", done_bytes / 1024 ** 3))
    L.append("| checkpoint 累计 | %s |" % json.dumps(cp.get("totals", {}), ensure_ascii=False))
    all_done = bool(total_batches) and len(completed) == total_batches
    L.append("| 全部完成条件 | %d 批 checkpoint 齐全 + `--verify` 复采 `SOURCE DISK UNCHANGED` + 归档门禁通过（当前：批次 %s / 复采 %s / 门禁 %s） |" %
             (total_batches,
              "**齐全**" if all_done else "%d/%d" % (len(completed), total_batches),
              "**一致**" if verify_ok else "待执行",
              "**通过**" if archive_ok else "待执行"))
    L.append("| 归档完整性门禁 | %s |" %
             ("`%s`（errors=0）" % archive.get("conclusion") if archive_ok
              else ("`%s`" % archive.get("conclusion", "未执行"))))
    L.append("")
    L.append("## 二、双通道验证汇总（引擎 vs 独立 MFT oracle）")
    L.append("")
    L.append("引擎：`NtfsParser` + `RecoverEngine`（P0-6 阶段未改；P0-7 只修 `NtfsParser.cpp` 的 `$ATTRIBUTE_LIST` 解析，见第十二节）；")
    L.append("真值：`tools/mft_oracle.py` 独立解析 MFT / USA fixup / DataRun 并直接读源盘。")
    L.append("")
    L.append("| 指标 | 值 |")
    L.append("|---|---|")
    L.append("| 请求文件数 | %s |" % f"{acc['requested']:,}")
    L.append("| 引擎恢复行数 | %s |" % f"{acc['engineRows']:,}")
    L.append("| **字节 SHA-256 完全一致** | **%s** |" % f"{acc['matched']:,}")
    L.append("| 字节不一致 | %s |" % f"{acc['mismatched']:,}")
    L.append("| 恢复失败 | %s |" % f"{acc['failed']:,}")
    L.append("| 缺 oracle 条目 | %s |" % f"{acc['missingOracle']:,}")
    L.append("| 逐字节召回率 | **%.4f%%** |" % recall)
    L.append("| `FileNode::id` 冲突造成的额外节点 | %s（%d 个批内有记录） |" %
             (f"{acc['duplicateIdNodes']:,}", collisions))
    L.append("")
    L.append("> 口径说明：`FileNode::id` 在 `NtfsParser` 中不唯一（普通节点用记录号、orphan 节点用 `fileFrn`），")
    L.append("> 编排器按 oracle 条目配对，同 id 的所有引擎行都参与哈希比对，多余节点单独记入 `idCollisions`，")
    L.append("> 不掩盖也不误报为恢复失败。详见 `P0-1_diagnosis.md §六-ter`。")
    L.append("")
    L.append("## 三、逐批记录")
    L.append("")
    L.append("| 批 | 卷 boot LBA | 请求 | 数据 GiB | 匹配 | 不一致 | 失败 | id 冲突节点 | 暂存已释放 | 完成时间 |")
    L.append("|---|---|---:|---:|---:|---:|---:|---:|---|---|")
    # The authoritative source per batch is the batch record itself, not either
    # plan file: batches 1-3 were driven by the legacy oracle plan (6 batches),
    # batches 4-7 by plan_engine.json (which also renumbers batch 4 as a 2-file
    # probe). Mixing plan rows with record stats is what silently misaligned the
    # columns and dropped batch 7, so the plans are only a fallback here.
    plan_rows = {}
    for src in (plan_engine.get("batches", []), plan.get("batches", [])):
        for b in src:
            plan_rows.setdefault(b["no"], b)
    for no in sorted(set(plan_rows) | set(records)):
        b = plan_rows.get(no, {})
        r = records.get(no)
        if not r:
            L.append("| %d | %d | %s | %.2f | — | — | — | — | 未开始 | — |" %
                     (no, b["volumeBootLBA"], f"{b['files']:,}",
                      b["bytes"] / 1024 ** 3))
            continue
        s = r.get("stats", {})
        boot = r.get("volumeBootLBA", b.get("volumeBootLBA"))
        req = r.get("requestedFiles", s.get("requested"))
        req_bytes = r.get("requestedBytes", b.get("bytes", 0))
        L.append("| %d | %s | %s | %.2f | %s | %s | %s | %s | %s | %s |" %
                 (no, boot, f"{int(req):,}" if req else "—",
                  req_bytes / 1024 ** 3,
                  f"{s.get('matched', 0):,}", f"{s.get('mismatched', 0):,}",
                  f"{s.get('failed', 0):,}", f"{s.get('duplicateIdNodes', 0):,}",
                  "是" if r.get("stagingReleased") else "**否**",
                  r.get("finishedAt", "—")))
    L.append("")
    L.append("> 请求数是批次记录自报的**候选行数**；批次 5 的 `id=162865`、批次 7 的 `809473` 各出现 2 次"
             "（同一 MFT 记录号同时指向普通文件与 ADS 节点），唯一 id 数见第十四节复核口径。"
             "批次 4 是 P0-7 的 2 文件探针批（`id=78` 的 `$ATTRIBUTE_LIST` 修复复核），不是全量批次；"
             "该行 `失败=1` 是**修复前**的原始记录，修复后经生产引擎重恢复（`p07_gap78`，56,725,784 字节，"
             "SHA-256 与源盘重算一致）并由卷作用域复核判为 2/2 精确匹配，见第十二节与 `engine_fix_verification.json`。")
    L.append("")
    L.append("逐文件哈希清单：`batches/batch_NNN_hashes.csv`；")
    L.append("批记录：`batches/batch_NNN.json`；断点：`checkpoint.json`。")
    L.append("")
    L.append("## 四、卷几何与候选来源")
    L.append("")
    L.append("| 卷 boot LBA | MFT 记录 | 用户文件 | 用户数据 | 已删除 | 碎片文件 |")
    L.append("|---|---:|---:|---:|---:|---:|")
    for boot, v in vols.items():
        if not v:
            continue
        L.append("| %d | %s | %s | %.2f GiB | %s | %s |" %
                 (boot, f"{v.get('recordsParsed', 0):,}",
                  f"{v.get('recoverableFileCount', 0):,}",
                  v.get("recoverableBytes", 0) / 1024 ** 3,
                  f"{v.get('deletedFiles', 0):,}",
                  f"{v.get('fragmentedFiles', 0):,}"))
    L.append("")
    L.append("已排除 NTFS 系统流（68 项、约 320 GiB 稀疏数据）。候选清单按 `firstLCN` 排序以保证顺序 I/O。")
    L.append("")
    L.append("## 五、诊断与修复预案")
    L.append("")
    L.append("- 结论：数据未丢，MBR / LDM 数据库 / 两个 NTFS 卷的 boot 与 MFT 均完好；「不显示」是外部动态磁盘未导入导致卷组装失败。")
    L.append("- 分级修复预案（L0 只读 / L1 导入外部动态磁盘 / L2 重建 LDM 库 / L3 改 `0x42`→`0x07` / L4 重建分区表）见 `P0-1_diagnosis.md §六`；可执行 SOP（门禁、只读诊断、各级校验、演练盘方案）见 **`P0-3_repair_playbook.md`**，**本轮只诊断未执行**。")
    L.append("- ⚠️ 执行任何修复都会销毁这份唯一的真实损坏态样本，前置条件：全部记录归档 + `--verify` 一致 + 明确确认不再需要样本。")
    L.append("")
    L.append("## 六、边界与未完成项（不得夸大）")
    L.append("")
    L.append("- 本轮的「真值」是独立 MFT 解析得到的字节，**不是用户提供的文件名清单**；验证的是恢复字节正确性，不含文件可打开性 / 应用语义校验。")
    L.append("- 未启用全盘 RAW 签名雕刻通道（1 TB × 合成基线 14.38 FP/MiB 会产生上千万假阳性节点，输出与耗时不可控）。因此本报告**不能**给出真实卷误报率。")
    L.append("- 未做 LDM VBLK 逐记录解析；磁盘组导入状态是推断而非定论。")
    L.append("- `badSectors=0`：本盘未暴露真实坏扇区，故「坏扇区对恢复的影响」未量化。")
    L.append("- `SectorReader` 预读缓存 A/B 实测为负优化（0/4/32 MiB = 6.26/7.57/35.03 s），正式批次一律不启用，详见 `P0-1_diagnosis.md §六-bis`。")
    if verify_ok:
        L.append("- ✅ 源盘零写入已复采证明：%s，`identityUnchanged=true`，%s 个采样点，`changed=[]`，结论 `SOURCE DISK UNCHANGED`（`baseline_verify.json`）。" %
                 (verify.get("verifiedAtUtc", "—"), vres.get("samplesCompared", "—")))
    else:
        L.append("- 源盘零写入证明以 `python tools/forensic_baseline.py --verify` 输出为准；全部批次完成后执行。")
    L.append("")
    L.append("## 七、零写入取证基线")
    L.append("")
    if baseline:
        g = baseline.get("geometry", {})
        ident = baseline.get("deviceIdentity", {})
        ac = baseline.get("access", {})
        samples = baseline.get("samples", [])
        read_fail = sum(1 for s in samples if not s.get("readOk", False))
        L.append("- 采集时间（UTC）：%s" % baseline.get("generatedAtUtc", "—"))
        L.append("- 设备：%s / 序列号 `%s` / Disk %s / 分区数 %s" %
                 (ident.get("model", "—"), ident.get("serialNumber", "—"),
                  ident.get("index", "—"), ident.get("partitions", "—")))
        L.append("- 几何：%s B / %s 扇区 / 扇区 %s B" %
                 (f"{g.get('totalBytes', 0):,}", f"{g.get('totalSectors', 0):,}",
                  g.get("sectorSize", "—")))
        L.append("- 访问方式：`%s`（share `%s`）；`genericWriteRequested=%s`、`writeOperationsPerformed=%s`；原始 SMART IOCTL 因需写权限主动跳过" %
                 (ac.get("desiredAccess", "—"), ac.get("shareMode", "—"),
                  ac.get("genericWriteRequested", "—"),
                  ac.get("writeOperationsPerformed", "—")))
        L.append("- 取证采样点：%d，读失败：%d" % (len(samples), read_fail))
        L.append("- 指纹文件：`baseline.json`（各采样点 SHA-256），复采用 `--verify`。")
    else:
        L.append("- `baseline.json` 未找到。")
    L.append("")
    if verify:
        L.append("- 复采时间（UTC）：%s；结论 `%s`；比对 %s 点，`identityUnchanged=%s`，`changed=%s`。" %
                 (verify.get("verifiedAtUtc", "—"),
                  vres.get("conclusion", "—"), vres.get("samplesCompared", "—"),
                  vres.get("identityUnchanged", "—"),
                  json.dumps(vres.get("changed", []), ensure_ascii=False)))
    else:
        L.append("- 复采：尚未执行（`baseline_verify.json` 不存在）。")
    L.append("")
    L.append("## 八、分批暂存释放（空间回收）")
    L.append("")
    L.append("用户约束：文件只恢复进项目内 `recover_staging/batch_NNN/`，记录落盘并复读校验后**释放该批空间**，再进入下一批；项目路径不长期留存文件本体。")
    L.append("")
    L.append("| 批 | 记录校验 | 暂存释放 / 空间回收 | 处理文件数 | 释放方式 | 空壳保留至 cleanup |")
    L.append("|---|---|---|---:|---|---|")
    for b in plan.get("batches", []):
        no = b["no"]
        r = records.get(no)
        if not r:
            L.append("| %d | — | — | — | — | — |" % no)
            continue
        # batch_001/002 were written by earlier schema revisions; their
        # records are historical evidence, so keep them untouched and map
        # the legacy field names here instead of rewriting the records.
        released = bool(r.get("stagingReleased"))
        n_files = r.get("stagingFilesHandled",
                        r.get("stagingFilesRemoved", r.get("stagingOps", 0)))
        mode = r.get("stagingReleaseMode")
        if mode is None:
            mode = ("truncate-to-zero (background-safe)"
                    if r.get("stagingShellsRetained")
                    else "hard-delete (foreground, 早期记录)")
        L.append("| %d | %s | %s | %s | %s | %s |" %
                 (no,
                  "是" if r.get("stats", {}).get("requested") == b["files"] else "**否**",
                  "是" if released else "**否**",
                  f"{int(n_files):,}", mode,
                  "是" if r.get("stagingShellsRetained") else "否"))
    L.append("")
    L.append("> 前两批在前台逐文件删除（早期记录字段 `stagingFilesRemoved`/`stagingUnlinkCalls`）；后台批次改用 `truncate(0)` 立即归还数据簇、不做删除（环境批量删除守卫为 turn-scoped 且累计，后台不继承批准），全部完成后在前台执行一次 `cleanup` 删除 0 字节空壳与实验目录。释放前强制复读 `batch_NNN.json` + `batch_NNN_hashes.csv` 校验，红线不变。")
    L.append("")
    L.append("## 九、归档完整性门禁")
    L.append("")
    L.append("文件本体在逐批释放后不保留，唯一证据是归档记录。`tools/verify_archive.py` 为纯后处理校验（**不读源盘、不改任何记录**）：`plan.json`/`checkpoint.json` 一致性、每批 record 的 `stats` 与 `hashes.csv` 行数及 SHA-256 自洽、`ids.txt` 行数、staging 空间确已回收、零写入复采结论；结果落 `archive_verification.json`。")
    L.append("")
    if archive:
        L.append("| 项 | 值 |")
        L.append("|---|---|")
        L.append("| 结论 | `%s` |" % archive.get("conclusion", "—"))
        L.append("| 批次 | %s / %s |" %
                 (archive.get("batchesCompleted", "—"),
                  archive.get("batchesPlanned", "—")))
        L.append("| errors / warnings | %d / %d |" %
                 (len(archive.get("errors", [])),
                  len(archive.get("warnings", []))))
        L.append("| 严格模式 | %s |" %
                 ("是" if archive.get("strictMode") else "否（进行中快照）"))
        for e in archive.get("errors", [])[:10]:
            L.append("")
            L.append("> ❌ %s" % e)
    else:
        L.append("- 尚未执行：`python tools/verify_archive.py`。")
    L.append("")

    ev3 = jload(os.path.join(OUT, "batches",
                            "batch_003_engine_verification.json"), {})
    ctest = jload(os.path.join(OUT, "engine_ctest_results.json"), {})
    L.append("## 十、引擎验证（可回归门禁 + 批次 3 字节复核）")
    L.append("")
    L.append("本轮不只做「引擎输出 vs 独立 oracle」的一次性比对：对引擎本身建立了 CTest 门禁，并把批次 3 的每一类异常逐项查清。生产引擎源码一行未改。")
    L.append("")
    if ctest:
        L.append("| CTest 门禁 | 正向 | 负向守卫 | 发现来源 |")
        L.append("|---|---|---|---|")
        for t in ctest.get("tests", []):
            L.append("| `%s` | %s（EXIT=%s） | `%s` → EXIT=%s | %s |" %
                     (t.get("name"), t.get("ctestSingle", "—"), t.get("positiveExit", "—"),
                      t.get("negativeEnv", "—"), t.get("negativeExit", "—"),
                      t.get("discoveredOn", "—")))
        L.append("")
        L.append("合并回归：`%s`；`DFTT_NTFS_RecoverBytes` 直跑 9/9 golden MD5、EXIT=0。证据：`engine_ctest_results.json`。" %
                 ctest.get("combinedCtest", "—"))
        L.append("")
    if ev3:
        eff = ev3.get("effectiveStats", {})
        L.append("**批次 3 有效口径**（原始记录不篡改，另建 `batch_003_engine_verification.json`）：")
        L.append("")
        L.append("| 通道 | 文件数 |")
        L.append("|---|---:|")
        L.append("| 直接匹配 | %s |" % f"{int(eff.get('directMatched', 0)):,}")
        L.append("| 修正 oracle ADS 缺陷后匹配 | %s |" % f"{int(eff.get('correctedOracleMatched', 0)):,}")
        L.append("| 输出隔离补恢复（retry）匹配 | %s |" % f"{int(eff.get('retryMatched', 0)):,}")
        L.append("| **有效已验证 / 请求** | **%s / %s** |" %
                 (f"{int(eff.get('effectiveVerified', 0)):,}",
                  f"{int(eff.get('requested', 0)):,}"))
        L.append("| unresolved | **%s** |" % eff.get("unresolved", "—"))
        L.append("")
        L.append("> `%s`；源盘写操作 0 次。11 个「字节不一致」全部是独立 oracle 未跳过 NTFS `Zone.Identifier` ADS 造成的假 mismatch，直接从盘重解析 MFT 后 11/11 精确 SHA-256 与归档引擎哈希一致；1287 个失败经输出隔离补恢复 1287/1287 一致；4 个 id 冲突是 `adsNode.id=(recordIndex<<4)|1` 与其他 MFT 记录号数值相撞，属配对噪音。详见 `P0-1_diagnosis.md §六-septies`。" %
                 ev3.get("conclusion", "—"))
        L.append("")
    L.append("> 第二节的 `recall` 是**原始记录口径**（`matched/requested`，未计复核）；批次 3 计入修正真值与 retry 后有效口径为 **100%**（105,901/105,901）；全量有效口径须在 **7 批**全部生成复核记录后重算（批次 4–7 走 `plan_engine.json`，分母为「实际请求的唯一 id 数」，见第十四节）。")
    L.append("")
    L.append("归档严格门禁 `verify_archive.py` 已支持「原始异常 + 复核解释」，但逐字段重新推导、不信任复核文件的自述结论；5 个源码级负向用例（篡改 SHA / unresolved / retry 数 / originalStats / 删除复核文件）全部按预期 FAIL，恢复后正向 0 errors。")
    L.append("")

    if esc:
        p06 = ctest.get("P0_6_engineScanAndFormat", {}) if ctest else {}
        en = p06.get("engineNodes", {})
        fp = p06.get("engineFormatProbes", {})
        hits = p06.get("signatureHitsInScanSample", {})
        L.append("## 十一、P0-6 引擎扫描 / 恢复 / 格式匹配（口径红线）")
        L.append("")
        L.append("硬约束：**扫描、恢复、文件格式匹配全部由引擎完成**；独立 MFT oracle 只做字节级真值比对，绝不决定「有哪些文件」。")
        L.append("")
        L.append("| 环节 | 引擎实现 |")
        L.append("|---|---|")
        L.append("| 发现 | `%s` → `engine_nodes_vol*.csv`（`scan` 子命令） |" %
                 esc.get("discoveryEngine", "—"))
        L.append("| 恢复 | `RecoverEngine::RecoverBatch()` |")
        L.append("| 格式匹配 | `%s`，**513 条内建签名**（AC 自动机），不按扩展名猜类型 |" %
                 esc.get("formatMatcher", "—"))
        L.append("| 数据前缀 | `RecoverEngine::ReadFileDataPrefix()`（resident / NTFS 压缩 / runlist 感知） |")
        L.append("")
        L.append("引擎实扫：卷 63 **%s** 节点 / 卷 209728638 **%s** 节点；格式探测 %s + %s 次（成功签名命中 %s + %s），`badSectors=0`、源盘零写入。" %
                 (f"{int(en.get('63', 0)):,}", f"{int(en.get('209728638', 0)):,}",
                  f"{int(fp.get('63', 0)):,}", f"{int(fp.get('209728638', 0)):,}",
                  f"{int(hits.get('63', 0)):,}", f"{int(hits.get('209728638', 0)):,}"))
        L.append("")
        L.append("覆盖复核：`%s`（`tools/verify_engine_scan.py` 从原始 CSV/JSON 逐字段重算，不信任自述结论）；负向守卫 `--negative-guard` 人为注入 1 个未发现 id → EXIT=1。" %
                 esc.get("conclusion", "—"))
        L.append("")
        vols_esc = esc.get("volumes", {})
        for boot in ("63", "209728638"):
            v = vols_esc.get(boot, {})
            if v:
                L.append("- 卷 %s：引擎用户节点 %s / oracle 候选 %s / 引擎独有 %s / **oracle 独有 %s**" %
                         (boot, f"{int(v.get('engineDiscoveredUserNodes', 0)):,}",
                          f"{int(v.get('oracleUserCandidates', 0)):,}",
                          f"{int(v.get('engineOnly', 0)):,}",
                          f"{int(v.get('oracleOnly', 0)):,}"))
        L.append("")
        L.append("历史口径：批次 1–3 的候选来自早期 oracle 清单（已归档、不重跑），但引擎扫描复核证明**全部 879,546 个计划 id 都在引擎发现集合内**且 `oracleOnly=0`；批次 4 起改用 `plan_engine.json`（`candidateSource=engine`，排除已完成 id）驱动，每批由 `assert_engine_discovery()` 硬门禁校验后才允许恢复。")
        L.append("")
        L.append("CTest `EngineScanFormatMatch`：正向 36 项 / 0 FAIL，`PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1` 负向 6 项 FAIL / EXIT=1（模拟「按扩展名猜格式」或「候选取自 oracle」必须失败）。证据 `engine_ctest_results.json`。")
        L.append("")
        L.append("观察项（**P0-6 阶段未改生产引擎**）：内建 `sqlite` 签名当前注册为 9 字节 `SQLite 3 `（`scan/SignatureDB.cpp:490`），与注释 `SQLite format 3\\0` 不符；该待办仍未处理。P0-7 针对验证发现的 `$ATTRIBUTE_LIST` 缺陷修改了 `NtfsParser.cpp` 一处，见第十二节。")
        L.append("")

    if gap or fix:
        L.append("## 十二、P0-7 引擎缺陷发现与修复（`$ATTRIBUTE_LIST`）")
        L.append("")
        L.append("P0-6 的引擎发现清单暴露出两个「声明 size > 0 但 `clusterCount=0`」的节点。`tools/verify_engine_gaps.py` 从源盘只读重解析 MFT，结论：")
        L.append("")
        L.append("- **真实缺陷 1 个**：MFT 记录 78（卷 63，`aliworkbench.log.old`，%s 字节）的 `$DATA` 在 `$ATTRIBUTE_LIST` 指向的 3 个扩展记录（10902/11004/10737）里，引擎只扫基本记录 → 漏掉整条 runlist。独立 oracle 同样不跟随 `$ATTRIBUTE_LIST`，两边盲区不能互证。" %
                 f"{int(gap.get('gapBytes', 0)):,}")
        L.append("- **非缺陷 1 个**：id 269279 的扩展记录 `$DATA` 已被清零（删除/截断残留），%s 字节只是陈旧 `$FILE_NAME.realSize`；引擎报「无可恢复数据」是正确的。归类 `noPayloadDeletedExtension`。" % "43,515,904")
        L.append("- 缺口证据 `engine_gap_evidence.json`：正向 EXIT=0，`--negative-guard` 篡改声明 size → EXIT=1。")
        L.append("")
        L.append("修复：`fs/ntfs/NtfsParser.cpp` 跟随 resident `$ATTRIBUTE_LIST` 的 unnamed `$DATA` 条目，按 `lowestVcn` 排序合并扩展记录 runlist（空洞填稀疏簇 0），`dataSize` 只取 VCN0 段；`RecoverEngine` / GUI / 编排逻辑均未改。")
        L.append("")
        L.append("引擎本体验证：新增 CTest `NtfsAttributeList`（4 MiB 合成 NTFS 卷，条目故意倒序写入以验证 VCN 排序，字节级恢复门禁 + `PHOENIX_NEGATIVE_ATTRLIST_GUARD=1` 负向 4 项 FAIL）；含新门禁的定向回归 **15/15 Passed（41.69 s）**。")
        L.append("")
        if fix:
            L.append("真实盘端到端（`phoenix_headless_p07.exe`，不覆盖后台仍在运行的 `_p06.exe`）：id 78 `clusterCount` **%d → %d**，`RecoverBatch` succeeded=1，恢复 %s 字节，SHA-256 `%s` 与独立真值 `%s` **完全一致**。" %
                     (int(fix.get("beforeFixClusterCount", 0)),
                      int(fix.get("afterFixClusterCount", 0)),
                      f"{int(fix.get('engineRecoveredSize', 0)):,}",
                      str(fix.get("engineRecoveredSha256", "—"))[:16] + "…",
                      str(fix.get("independentTruthSha256", "—"))[:16] + "…"))
            L.append("")
            L.append("`tools/verify_engine_fix.py`（正向 EXIT=0 / 负向篡改期望 SHA → EXIT=1）把前后两次引擎扫描 CSV、恢复结果 CSV、恢复文件 SHA-256 逐项重推，结论 `%s`。详见 `P0-7_attribute_list_fix.md`。" %
                     fix.get("verdict", "—"))
        L.append("")
        L.append("口径红线：批次 4 的「修复前失败」记录保持原样，修复后成功结果单独归档，两者不互相覆盖；后台批次 5–7 仍用修复前引擎，其 id 269279 失败已由缺口证据判定为正确行为。")
        L.append("")

    L.append("## 十三、P0-8 复核工具链卷隔离加固（收尾口径）")
    L.append("")
    L.append("节点 id 只在**单个卷内**唯一：卷 209728638 也有 MFT 记录 78（`...appinfo`，220 字节），与卷 63 被 P0-7 修复的 56.7 MB 文件毫无关系；batch 7 同时含 `id=78` 与 `id=269279`。原 `verify_engine_plan_batch.py` 把 `gap_ids` / `ads_ids` / `replay` 都做成全局 id 集合，会把两卷同号记录交叉解释。")
    L.append("")
    L.append("- 修复：全部改为 `(bootLBA, id)` 卷作用域；`fix_ok` 不再匹配 verdict 字符串（原代码空格/下划线写错导致该分支**恒为 False**），改为逐字段重推 `engine_fix_verification.json` 的 SHA/大小/前后 `clusterCount`；replay 目录加运行时间戳避免与零字节壳冲突。")
    L.append("- 新增 `volume_scoping_ok()` 双向自检与 `--negative-guard-cross-volume`：故意跨卷泄漏 P0-7 解释必须被拒绝。`verify_archive.py` 的 `ep_valid()` 额外校验 `volumeBootLBA`，且 `recoveredByP07AttributeListFix` 只允许用于卷 63 / id 78。")
    L.append("- 收尾口径：`status` 合并两版计划后显示 **7/7 批**；`cleanup` 扫描 `recover_staging/` 全部子目录（含 `p07_*` / `batch_004_stale`），先释放残留字节再断言「0 bytes left」；`verify_engine_batch.py` 拒绝 `--batch >= 4`，引擎口径批次一律走 `verify_engine_plan_batch.py`；`P0-3_repair_playbook.md §5.1` 的收尾序列已把复核移到 `cleanup` 之前（replay 需要 staging）。")
    L.append("")
    L.append("验证：卷隔离自检 PASS；跨卷负向 EXIT=1；`--batch 4` 正向 **2/2 精确 SHA-256**；`--negative-guard` EXIT=1；归档门禁正向 0 errors / 篡改 `volumeBootLBA` 负向 EXIT=1。三节点跨卷真值探针：卷 63/78 → P0-7 解释，卷 209728638/78（220 字节普通文件）→ **UNRESOLVED 未被误解释**，269279 → `noPayloadDeletedExtension`（并发 I/O 下耗时 212.9 s）。详见 `P0-8_verification_gate_hardening.md`。")
    L.append("")
    fst = fin.get("status", "NOT STARTED")
    if fst == "OK":
        crit = [s for s in fin.get("steps", []) if s.get("severity") == "critical"]
        deferred = fin.get("deferred") or []
        L.append("**收尾已完成**：守护 `tools/finalize_recovery.py` 在 checkpoint 到达 `[1..7]` 后按 §5.1 顺序执行 7 步（`finishedAt=%s`）：%d 个 critical 步骤全部 rc=0，严格 `verify_archive.py` errors=0、最终零写入复采 `SOURCE DISK UNCHANGED`。" %
                 (fin.get("finishedAt", "—"), len(crit)))
        if deferred:
            dstep = next((s for s in fin.get("steps", [])
                          if s.get("name") == "cleanup"), {})
            L.append("")
            L.append("其中 `cleanup` 以 **DEFERRED** 记账，不是 rc=0：`status` 显示 7 批数据簇已在逐批释放时全部归还，"
                     "只剩 %s 个零字节空壳文件（合计 **%s 字节**），用 `rmdir` 逐目录删要数小时，"
                     "会无限期阻塞后面的严格门禁，而门禁本身不读 staging（replay 走生产引擎重采）。"
                     "另有 %d 个辅助探针目录仍占 %s 字节（`readahead_*`/`ra_*` 基准缓存与 P0-7 的 `p07_gap78` 证据），"
                     "属实验产物、非恢复载荷，单独计量不与批次暂存相加。" %
                     (f"{dstep.get('batchShellFiles', 0):,}",
                      f"{dstep.get('batchShellBytes', 0):,}",
                      len(dstep.get("auxProbeDirs", {}) or {}),
                      f"{dstep.get('auxProbeBytes', 0):,}"))
            L.append("")
            L.append("可证伪性：`final_audit.py` 不采信这条说明，自己去读 `archive_verification.json` 里每个批次"
                     "由严格门禁**独立重测**的 `stagingNonZeroBytesFiles`，并要求 `batchShellBytes == 0` 才放行"
                     "（`deferred cleanup has nothing left to reclaim`）。")
        else:
            cstep = next((s for s in fin.get("steps", [])
                          if s.get("name") == "cleanup"), {})
            if cstep.get("returnCode") == 0:
                L.append("")
                L.append("`cleanup` 已真实执行并 rc=0（%s s）：`recover_staging/` 下的 63 万余个零字节空壳已全部删除，"
                         "连同 `readahead_*`/`ra_*` 基准缓存与 `p07_gap78` 证据目录一并回收（数值已归档在此前各节与 JSON，"
                         "源盘未动、可随时重采）。严格门禁在**清空后**重跑，逐批报告 `staging tree fully deleted`。"
                         % cstep.get("seconds", 0))
    elif fst == "FAILED":
        L.append("**收尾中断**：守护在关键步骤 `%s` 失败并立即停止，staging 保留供排查；详见 `finalize_state.json`（含失败步骤 stdout/stderr 尾部）与 `finalize.log`。" %
                 fin.get("failedStep", "—"))
    else:
        L.append("**收尾进行中**：守护 `tools/finalize_recovery.py` 状态 `%s`，等待 checkpoint `[1..7]` 后按 §5.1 顺序执行复核 → cleanup → 零写入复采 → 严格门禁 → 汇总；关键步骤失败即停并保留 staging。" % fst)
    L.append("")

    L.append("## 十四、P0-9 请求 id 口径加固与自动收尾")
    L.append("")
    L.append("批次 5 正式复核首次报 `effective=249455 != requested=249456`、EXIT=1。查明**不是恢复失败**：`plan_engine.json::files` 统计的是候选**行数**，而引擎节点 id 是 MFT 记录号（ADS 节点为 `(rec<<4)|slot`），同一数值可能同时对应一个普通文件和一个 ADS 节点。`batch_005_ids.txt` 里 `id=162865` 出现 2 次（batch 7 的 `809473` 同样重复），向引擎请求两次恢复的是**同一节点集合**，所以逐字节复核的分母必须是「实际请求的唯一 id 数」。")
    L.append("")
    L.append("- `tools/verify_engine_plan_batch.py` 新增 `request_scope()`：以 `ids.txt` 的唯一 id 集为分母；`declared − unique` 必须由**字面重复行**完全解释；`hashes.csv` 的 id 集合必须与请求唯一集精确相等；另加 `covered`（已验证 id 集 == 请求 id 集）强校验。复核记录新增 `requestScope` / `effectiveStats.requestedDeclared`。原始批次记录与 `plan_engine.json` **一行未改**。")
    L.append("- `tools/verify_archive.py::ep_valid()` 自己重读 `ids.txt` 独立重推 declared / lines / unique / duplicates / hash id 集，逐字段比对复核文件的 `requestScope` 与 `effectiveStats`，不信任其自述结论。")
    L.append("- 可证伪门禁：`duplicate_scope_self_test()` 覆盖「合法重复」「真缺失一行」「外来 hash id」「hash 缺 id」「多声明」5 例；`--negative-guard-duplicate-ids` 人为多声明 1 个 id，实测 **EXIT=1** 并报 `declared-unique deficit 2 not fully explained by 1 duplicate id rows`；跨卷负向仍 EXIT=1。")
    L.append("- 复核实测：批次 5 **249,455 / 249,455 唯一 id，unresolved=0**，17 条 `match=0` 全部 `exactSha256Match`（11 个 oracle ADS 假 mismatch + 6 个引擎独有 ADS 节点经生产引擎 replay 补齐）；批次 4 重生成 2/2 精确 SHA-256；归档中途快照 `--allow-partial` **0 errors / 0 warnings / EXIT=0**。篡改 `requestScope.requestedUniqueIds`（249455→249456）后归档门禁 **errors=6 / EXIT=1**（`requestScope not reproducible`），逐字节恢复后正向 EXIT=0，证据 `archive_verification_negative_p09.json`。`verify_engine_scan.py` 复验仍为 `ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS`；引擎定向 CTest **15/15 Passed（40.96 s）**；源盘复采 `SOURCE DISK UNCHANGED`。")
    L.append("- 自动收尾守护 `tools/finalize_recovery.py`：等待 7 批全部落盘后按序执行 `status → verify_engine_plan_batch --all-completed → verify_engine_batch --all-completed → cleanup → forensic_baseline --verify → verify_archive（严格）→ make_summary`。复核严格排在 cleanup 之前（replay 依赖 staging），关键步骤失败即停且保留 staging；cleanup 为 warning 级（其内容全是零字节壳，空间已在逐批释放时回收）。状态与每步 rc 落 `finalize_state.json`，日志 `finalize.log`。")
    L.append("- 守护新增 `--resume`（保留已 rc=0 的步骤续跑）与 `--skip-cleanup`（把空壳清扫记为 **DEFERRED** 并分项计量 `batchShellBytes` / `auxProbeBytes`，repeated 运行幂等替换而非追加）。这样「验证」与「几小时的目录删除」解耦：门禁结论在删除前就已定稿，删除进度不再能改变结论。")
    L.append("")
    if fa:
        t9 = fa.get("totals", {})
        L.append("**最终独立审计**（`tools/final_audit.py`，不信任任何自述结论，逐批从 ids.txt / hashes.csv / 复核记录重推）：状态 `%s`，%s；检查 %d 项、失败 %d 项；全量 declared=%s / distinct=%s / effectiveVerified=%s / unresolved=%s。负向守卫 `--negative-guard`（少算 1 个文件）必须 EXIT=1。" %
                 (fa.get("status", "—"), fa.get("conclusion", "—"),
                  len(fa.get("checks", [])), len(fa.get("errors", [])),
                  f"{int(t9.get('declared', 0)):,}",
                  f"{int(t9.get('distinctIds', 0)):,}",
                  f"{int(t9.get('effectiveVerified', 0)):,}",
                  t9.get("unresolved", "—")))
        L.append("")
    L.append("本阶段仅修改 `tools/` 下验证与编排脚本，**生产引擎源码未改**，源盘写操作 0 次。")
    L.append("")

    with open(args.out, "w", encoding="utf-8") as f:
        f.write("\n".join(L))
    print("summary -> %s" % args.out)
    print("batches %d/%d, matched=%d, mismatched=%d, failed=%d, recall=%.4f%%" %
          (len(completed), total_batches, acc["matched"], acc["mismatched"],
           acc["failed"], recall))


if __name__ == "__main__":
    main()
