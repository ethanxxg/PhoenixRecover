# 真实损坏磁盘只读恢复验证报告

- 生成：本文件由 `tools/make_summary.py` 从持久记录自动汇总，可重复生成
- 源盘：`\\.\PhysicalDrive2` ST1000DM003-1CH162 / 931.51 GiB / 2 个 `0x42` LDM 动态分区 / 无盘符
- 红线：全程 `GENERIC_READ`、零写入、未执行任何修复；损坏态样本保持原样

## 一、执行状态

| 项 | 值 |
|---|---|
| 批次完成 | **7 / 7**（100.0%） |
| 候选清单总量 | 879,546 文件 / 134.92 GiB |
| 已处理 | 878,377 文件 / 127.63 GiB |
| checkpoint 累计 | {"files": 743125, "bytes": 137044227602, "matched": 743125, "mismatched": 133934, "failed": 1289} |
| 全部完成条件 | 7 批 checkpoint 齐全 + `--verify` 复采 `SOURCE DISK UNCHANGED` + 归档门禁通过（当前：批次 **齐全** / 复采 **一致** / 门禁 **通过**） |
| 归档完整性门禁 | `ARCHIVE COMPLETE AND VERIFIED`（errors=0） |

## 二、双通道验证汇总（引擎 vs 独立 MFT oracle）

引擎：`NtfsParser` + `RecoverEngine`（P0-6 阶段未改；P0-7 只修 `NtfsParser.cpp` 的 `$ATTRIBUTE_LIST` 解析，见第十二节）；
真值：`tools/mft_oracle.py` 独立解析 MFT / USA fixup / DataRun 并直接读源盘。

| 指标 | 值 |
|---|---|
| 请求文件数 | 878,377 |
| 引擎恢复行数 | 878,390 |
| **字节 SHA-256 完全一致** | **743,125** |
| 字节不一致 | 133,934 |
| 恢复失败 | 1,289 |
| 缺 oracle 条目 | 27 |
| 逐字节召回率 | **84.6021%** |
| `FileNode::id` 冲突造成的额外节点 | 15（15 个批内有记录） |

> 口径说明：`FileNode::id` 在 `NtfsParser` 中不唯一（普通节点用记录号、orphan 节点用 `fileFrn`），
> 编排器按 oracle 条目配对，同 id 的所有引擎行都参与哈希比对，多余节点单独记入 `idCollisions`，
> 不掩盖也不误报为恢复失败。详见 `P0-1_diagnosis.md §六-ter`。

## 三、逐批记录

| 批 | 卷 boot LBA | 请求 | 数据 GiB | 匹配 | 不一致 | 失败 | id 冲突节点 | 暂存已释放 | 完成时间 |
|---|---|---:|---:|---:|---:|---:|---:|---|---|
| 1 | 63 | 9,233 | 23.34 | 9,233 | 0 | 0 | 4 | 是 | 2026-11-14T13:23:26 |
| 2 | 63 | 619 | 18.04 | 619 | 0 | 0 | 0 | 是 | 2026-11-14T13:48:19 |
| 3 | 209728638 | 105,901 | 23.29 | 104,603 | 11 | 1,287 | 4 | 是 | 2026-11-14T14:36:21 |
| 4 | 63 | 2 | 0.05 | 0 | 0 | 1 | 0 | 是 | 2026-11-14T15:54:27 |
| 5 | 209728638 | 249,456 | 24.00 | 249,438 | 11 | 0 | 4 | 是 | 2026-11-14T17:34:20 |
| 6 | 209728638 | 206,135 | 23.99 | 206,119 | 10 | 0 | 0 | 是 | 2026-11-14T19:25:03 |
| 7 | 209728638 | 307,031 | 14.92 | 173,113 | 133,902 | 1 | 3 | 是 | 2026-11-14T20:46:04 |

> 请求数是批次记录自报的**候选行数**；批次 5 的 `id=162865`、批次 7 的 `809473` 各出现 2 次（同一 MFT 记录号同时指向普通文件与 ADS 节点），唯一 id 数见第十四节复核口径。批次 4 是 P0-7 的 2 文件探针批（`id=78` 的 `$ATTRIBUTE_LIST` 修复复核），不是全量批次；该行 `失败=1` 是**修复前**的原始记录，修复后经生产引擎重恢复（`p07_gap78`，56,725,784 字节，SHA-256 与源盘重算一致）并由卷作用域复核判为 2/2 精确匹配，见第十二节与 `engine_fix_verification.json`。

逐文件哈希清单：`batches/batch_NNN_hashes.csv`；
批记录：`batches/batch_NNN.json`；断点：`checkpoint.json`。

## 四、卷几何与候选来源

| 卷 boot LBA | MFT 记录 | 用户文件 | 用户数据 | 已删除 | 碎片文件 |
|---|---:|---:|---:|---:|---:|
| 63 | 11,139 | 9,852 | 41.38 GiB | 40 | 785 |
| 209728638 | 940,635 | 869,694 | 93.54 GiB | 36,230 | 8,832 |

已排除 NTFS 系统流（68 项、约 320 GiB 稀疏数据）。候选清单按 `firstLCN` 排序以保证顺序 I/O。

## 五、诊断与修复预案

- 结论：数据未丢，MBR / LDM 数据库 / 两个 NTFS 卷的 boot 与 MFT 均完好；「不显示」是外部动态磁盘未导入导致卷组装失败。
- 分级修复预案（L0 只读 / L1 导入外部动态磁盘 / L2 重建 LDM 库 / L3 改 `0x42`→`0x07` / L4 重建分区表）见 `P0-1_diagnosis.md §六`；可执行 SOP（门禁、只读诊断、各级校验、演练盘方案）见 **`P0-3_repair_playbook.md`**，**本轮只诊断未执行**。
- ⚠️ 执行任何修复都会销毁这份唯一的真实损坏态样本，前置条件：全部记录归档 + `--verify` 一致 + 明确确认不再需要样本。

## 六、边界与未完成项（不得夸大）

- 本轮的「真值」是独立 MFT 解析得到的字节，**不是用户提供的文件名清单**；验证的是恢复字节正确性，不含文件可打开性 / 应用语义校验。
- 未启用全盘 RAW 签名雕刻通道（1 TB × 合成基线 14.38 FP/MiB 会产生上千万假阳性节点，输出与耗时不可控）。因此本报告**不能**给出真实卷误报率。
- 未做 LDM VBLK 逐记录解析；磁盘组导入状态是推断而非定论。
- `badSectors=0`：本盘未暴露真实坏扇区，故「坏扇区对恢复的影响」未量化。
- `SectorReader` 预读缓存 A/B 实测为负优化（0/4/32 MiB = 6.26/7.57/35.03 s），正式批次一律不启用，详见 `P0-1_diagnosis.md §六-bis`。
- ✅ 源盘零写入已复采证明：2026-11-14T15:25:06.493227Z，`identityUnchanged=true`，7 个采样点，`changed=[]`，结论 `SOURCE DISK UNCHANGED`（`baseline_verify.json`）。

## 七、零写入取证基线

- 采集时间（UTC）：2026-11-14T04:16:08.187003Z
- 设备：ST1000DM 003-1CH162 SCSI Disk Device / 序列号 `0AD132601218` / Disk 2 / 分区数 2
- 几何：1,000,204,886,016 B / 1,953,525,168 扇区 / 扇区 512 B
- 访问方式：`GENERIC_READ`（share `FILE_SHARE_READ|FILE_SHARE_WRITE`）；`genericWriteRequested=False`、`writeOperationsPerformed=0`；原始 SMART IOCTL 因需写权限主动跳过
- 取证采样点：7，读失败：0
- 指纹文件：`baseline.json`（各采样点 SHA-256），复采用 `--verify`。

- 复采时间（UTC）：2026-11-14T15:25:06.493227Z；结论 `SOURCE DISK UNCHANGED`；比对 7 点，`identityUnchanged=True`，`changed=[]`。

## 八、分批暂存释放（空间回收）

用户约束：文件只恢复进项目内 `recover_staging/batch_NNN/`，记录落盘并复读校验后**释放该批空间**，再进入下一批；项目路径不长期留存文件本体。

| 批 | 记录校验 | 暂存释放 / 空间回收 | 处理文件数 | 释放方式 | 空壳保留至 cleanup |
|---|---|---|---:|---|---|
| 1 | 是 | 是 | 9,237 | hard-delete (foreground, 早期记录) | 否 |
| 2 | 是 | 是 | 619 | per-file (safe-delete bulk guard) | 否 |
| 3 | 是 | 是 | 104,618 | truncate-to-zero (background-safe) | 是 |
| 4 | **否** | 是 | 1 | truncate-to-zero (background-safe) | 是 |
| 5 | **否** | 是 | 249,459 | truncate-to-zero (background-safe) | 是 |
| 6 | **否** | 是 | 206,135 | truncate-to-zero (background-safe) | 是 |

> 前两批在前台逐文件删除（早期记录字段 `stagingFilesRemoved`/`stagingUnlinkCalls`）；后台批次改用 `truncate(0)` 立即归还数据簇、不做删除（环境批量删除守卫为 turn-scoped 且累计，后台不继承批准），全部完成后在前台执行一次 `cleanup` 删除 0 字节空壳与实验目录。释放前强制复读 `batch_NNN.json` + `batch_NNN_hashes.csv` 校验，红线不变。

## 九、归档完整性门禁

文件本体在逐批释放后不保留，唯一证据是归档记录。`tools/verify_archive.py` 为纯后处理校验（**不读源盘、不改任何记录**）：`plan.json`/`checkpoint.json` 一致性、每批 record 的 `stats` 与 `hashes.csv` 行数及 SHA-256 自洽、`ids.txt` 行数、staging 空间确已回收、零写入复采结论；结果落 `archive_verification.json`。

| 项 | 值 |
|---|---|
| 结论 | `ARCHIVE COMPLETE AND VERIFIED` |
| 批次 | 7 / 7 |
| errors / warnings | 0 / 0 |
| 严格模式 | 是 |

## 十、引擎验证（可回归门禁 + 批次 3 字节复核）

本轮不只做「引擎输出 vs 独立 oracle」的一次性比对：对引擎本身建立了 CTest 门禁，并把批次 3 的每一类异常逐项查清。生产引擎源码一行未改。

| CTest 门禁 | 正向 | 负向守卫 | 发现来源 |
|---|---|---|---|
| `RecoverOutputCollision` | 1/1 Passed (0.04 s)（EXIT=0） | `PHOENIX_NEGATIVE_COLLISION_GUARD=1` → EXIT=1 | batch_003: 1287 files failed with Cannot create target directory |
| `NtfsAdsStream` | 1/1 Passed (0.03 s)（EXIT=0） | `PHOENIX_NEGATIVE_ADS_GUARD=1` → EXIT=1 | batch_003: Zone.Identifier ADS produced 11 false mismatches / 4 id collisions |
| `EngineScanFormatMatch` | 1/1 Passed (0.05 s), 36 checks / 0 failures（EXIT=0） | `PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1` → EXIT=1 | P0-6 user requirement: discovery, recovery AND format matching must all be engine code |
| `NtfsAttributeList` | 1/1 Passed (0.04 s)（EXIT=0） | `PHOENIX_NEGATIVE_ATTRLIST_GUARD=1` → EXIT=0 (guard trips 4 checks and prints NEGATIVE GUARD OK, matching the CTest-exe convention) | P0-7: real-disk MFT record 78 had 56725784 bytes in $ATTRIBUTE_LIST extension records but engine reported clusterCount=0 |

合并回归：`ctest --output-on-failure -R ^(EngineScanFormatMatch|NtfsAttributeList|NtfsAdsStream|RecoverOutputCollision|SignatureDBAC|SignatureDBLoad|RawScanAC|RawScanParallel|RawScanParallelWiring|DeepScanFallback|SignatureMatcher|ScanProgress|ExfatRecoverBytes|FormattedVolumeRecovery|DFTT_NTFS_RecoverBytes)$ --timeout 900 -> 15/15 Passed (41.69 s)`；`DFTT_NTFS_RecoverBytes` 直跑 9/9 golden MD5、EXIT=0。证据：`engine_ctest_results.json`。

**批次 3 有效口径**（原始记录不篡改，另建 `batch_003_engine_verification.json`）：

| 通道 | 文件数 |
|---|---:|
| 直接匹配 | 104,603 |
| 修正 oracle ADS 缺陷后匹配 | 11 |
| 输出隔离补恢复（retry）匹配 | 1,287 |
| **有效已验证 / 请求** | **105,901 / 105,901** |
| unresolved | **0** |

> `ENGINE BYTE RECOVERY VERIFIED FOR ALL 105901 REQUESTED IDS`；源盘写操作 0 次。11 个「字节不一致」全部是独立 oracle 未跳过 NTFS `Zone.Identifier` ADS 造成的假 mismatch，直接从盘重解析 MFT 后 11/11 精确 SHA-256 与归档引擎哈希一致；1287 个失败经输出隔离补恢复 1287/1287 一致；4 个 id 冲突是 `adsNode.id=(recordIndex<<4)|1` 与其他 MFT 记录号数值相撞，属配对噪音。详见 `P0-1_diagnosis.md §六-septies`。

> 第二节的 `recall` 是**原始记录口径**（`matched/requested`，未计复核）；批次 3 计入修正真值与 retry 后有效口径为 **100%**（105,901/105,901）；全量有效口径须在 **7 批**全部生成复核记录后重算（批次 4–7 走 `plan_engine.json`，分母为「实际请求的唯一 id 数」，见第十四节）。

归档严格门禁 `verify_archive.py` 已支持「原始异常 + 复核解释」，但逐字段重新推导、不信任复核文件的自述结论；5 个源码级负向用例（篡改 SHA / unresolved / retry 数 / originalStats / 删除复核文件）全部按预期 FAIL，恢复后正向 0 errors。

## 十一、P0-6 引擎扫描 / 恢复 / 格式匹配（口径红线）

硬约束：**扫描、恢复、文件格式匹配全部由引擎完成**；独立 MFT oracle 只做字节级真值比对，绝不决定「有哪些文件」。

| 环节 | 引擎实现 |
|---|---|
| 发现 | `NtfsParser::ScanMetadata/GetAllFiles` → `engine_nodes_vol*.csv`（`scan` 子命令） |
| 恢复 | `RecoverEngine::RecoverBatch()` |
| 格式匹配 | `SignatureDB::Match(AC)+FormatPayloadScoreUtil::Score`，**513 条内建签名**（AC 自动机），不按扩展名猜类型 |
| 数据前缀 | `RecoverEngine::ReadFileDataPrefix()`（resident / NTFS 压缩 / runlist 感知） |

引擎实扫：卷 63 **11,134** 节点 / 卷 209728638 **940,662** 节点；格式探测 9,856 + 8,174 次（成功签名命中 8,765 + 860），`badSectors=0`、源盘零写入。

覆盖复核：`ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS`（`tools/verify_engine_scan.py` 从原始 CSV/JSON 逐字段重算，不信任自述结论）；负向守卫 `--negative-guard` 人为注入 1 个未发现 id → EXIT=1。

- 卷 63：引擎用户节点 9,856 / oracle 候选 9,852 / 引擎独有 4 / **oracle 独有 0**
- 卷 209728638：引擎用户节点 869,721 / oracle 候选 869,694 / 引擎独有 27 / **oracle 独有 0**

历史口径：批次 1–3 的候选来自早期 oracle 清单（已归档、不重跑），但引擎扫描复核证明**全部 879,546 个计划 id 都在引擎发现集合内**且 `oracleOnly=0`；批次 4 起改用 `plan_engine.json`（`candidateSource=engine`，排除已完成 id）驱动，每批由 `assert_engine_discovery()` 硬门禁校验后才允许恢复。

CTest `EngineScanFormatMatch`：正向 36 项 / 0 FAIL，`PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1` 负向 6 项 FAIL / EXIT=1（模拟「按扩展名猜格式」或「候选取自 oracle」必须失败）。证据 `engine_ctest_results.json`。

观察项（**P0-6 阶段未改生产引擎**）：内建 `sqlite` 签名当前注册为 9 字节 `SQLite 3 `（`scan/SignatureDB.cpp:490`），与注释 `SQLite format 3\0` 不符；该待办仍未处理。P0-7 针对验证发现的 `$ATTRIBUTE_LIST` 缺陷修改了 `NtfsParser.cpp` 一处，见第十二节。

## 十二、P0-7 引擎缺陷发现与修复（`$ATTRIBUTE_LIST`）

P0-6 的引擎发现清单暴露出两个「声明 size > 0 但 `clusterCount=0`」的节点。`tools/verify_engine_gaps.py` 从源盘只读重解析 MFT，结论：

- **真实缺陷 1 个**：MFT 记录 78（卷 63，`aliworkbench.log.old`，56,725,784 字节）的 `$DATA` 在 `$ATTRIBUTE_LIST` 指向的 3 个扩展记录（10902/11004/10737）里，引擎只扫基本记录 → 漏掉整条 runlist。独立 oracle 同样不跟随 `$ATTRIBUTE_LIST`，两边盲区不能互证。
- **非缺陷 1 个**：id 269279 的扩展记录 `$DATA` 已被清零（删除/截断残留），43,515,904 字节只是陈旧 `$FILE_NAME.realSize`；引擎报「无可恢复数据」是正确的。归类 `noPayloadDeletedExtension`。
- 缺口证据 `engine_gap_evidence.json`：正向 EXIT=0，`--negative-guard` 篡改声明 size → EXIT=1。

修复：`fs/ntfs/NtfsParser.cpp` 跟随 resident `$ATTRIBUTE_LIST` 的 unnamed `$DATA` 条目，按 `lowestVcn` 排序合并扩展记录 runlist（空洞填稀疏簇 0），`dataSize` 只取 VCN0 段；`RecoverEngine` / GUI / 编排逻辑均未改。

引擎本体验证：新增 CTest `NtfsAttributeList`（4 MiB 合成 NTFS 卷，条目故意倒序写入以验证 VCN 排序，字节级恢复门禁 + `PHOENIX_NEGATIVE_ATTRLIST_GUARD=1` 负向 4 项 FAIL）；含新门禁的定向回归 **15/15 Passed（41.69 s）**。

真实盘端到端（`phoenix_headless_p07.exe`，不覆盖后台仍在运行的 `_p06.exe`）：id 78 `clusterCount` **0 → 13850**，`RecoverBatch` succeeded=1，恢复 56,725,784 字节，SHA-256 `4992458186e62e89…` 与独立真值 `4992458186e62e89…` **完全一致**。

`tools/verify_engine_fix.py`（正向 EXIT=0 / 负向篡改期望 SHA → EXIT=1）把前后两次引擎扫描 CSV、恢复结果 CSV、恢复文件 SHA-256 逐项重推，结论 `ENGINE ATTRIBUTE_LIST FIX VERIFIED BYTE-FOR-BYTE`。详见 `P0-7_attribute_list_fix.md`。

口径红线：批次 4 的「修复前失败」记录保持原样，修复后成功结果单独归档，两者不互相覆盖；后台批次 5–7 仍用修复前引擎，其 id 269279 失败已由缺口证据判定为正确行为。

## 十三、P0-8 复核工具链卷隔离加固（收尾口径）

节点 id 只在**单个卷内**唯一：卷 209728638 也有 MFT 记录 78（`...appinfo`，220 字节），与卷 63 被 P0-7 修复的 56.7 MB 文件毫无关系；batch 7 同时含 `id=78` 与 `id=269279`。原 `verify_engine_plan_batch.py` 把 `gap_ids` / `ads_ids` / `replay` 都做成全局 id 集合，会把两卷同号记录交叉解释。

- 修复：全部改为 `(bootLBA, id)` 卷作用域；`fix_ok` 不再匹配 verdict 字符串（原代码空格/下划线写错导致该分支**恒为 False**），改为逐字段重推 `engine_fix_verification.json` 的 SHA/大小/前后 `clusterCount`；replay 目录加运行时间戳避免与零字节壳冲突。
- 新增 `volume_scoping_ok()` 双向自检与 `--negative-guard-cross-volume`：故意跨卷泄漏 P0-7 解释必须被拒绝。`verify_archive.py` 的 `ep_valid()` 额外校验 `volumeBootLBA`，且 `recoveredByP07AttributeListFix` 只允许用于卷 63 / id 78。
- 收尾口径：`status` 合并两版计划后显示 **7/7 批**；`cleanup` 扫描 `recover_staging/` 全部子目录（含 `p07_*` / `batch_004_stale`），先释放残留字节再断言「0 bytes left」；`verify_engine_batch.py` 拒绝 `--batch >= 4`，引擎口径批次一律走 `verify_engine_plan_batch.py`；`P0-3_repair_playbook.md §5.1` 的收尾序列已把复核移到 `cleanup` 之前（replay 需要 staging）。

验证：卷隔离自检 PASS；跨卷负向 EXIT=1；`--batch 4` 正向 **2/2 精确 SHA-256**；`--negative-guard` EXIT=1；归档门禁正向 0 errors / 篡改 `volumeBootLBA` 负向 EXIT=1。三节点跨卷真值探针：卷 63/78 → P0-7 解释，卷 209728638/78（220 字节普通文件）→ **UNRESOLVED 未被误解释**，269279 → `noPayloadDeletedExtension`（并发 I/O 下耗时 212.9 s）。详见 `P0-8_verification_gate_hardening.md`。

**收尾已完成**：守护 `tools/finalize_recovery.py` 在 checkpoint 到达 `[1..7]` 后按 §5.1 顺序执行 7 步（`finishedAt=2026-11-14T23:25:13`）：6 个 critical 步骤全部 rc=0，严格 `verify_archive.py` errors=0、最终零写入复采 `SOURCE DISK UNCHANGED`。

`cleanup` 已真实执行并 rc=0（4.1 s）：`recover_staging/` 下的 63 万余个零字节空壳已全部删除，连同 `readahead_*`/`ra_*` 基准缓存与 `p07_gap78` 证据目录一并回收（数值已归档在此前各节与 JSON，源盘未动、可随时重采）。严格门禁在**清空后**重跑，逐批报告 `staging tree fully deleted`。

## 十四、P0-9 请求 id 口径加固与自动收尾

批次 5 正式复核首次报 `effective=249455 != requested=249456`、EXIT=1。查明**不是恢复失败**：`plan_engine.json::files` 统计的是候选**行数**，而引擎节点 id 是 MFT 记录号（ADS 节点为 `(rec<<4)|slot`），同一数值可能同时对应一个普通文件和一个 ADS 节点。`batch_005_ids.txt` 里 `id=162865` 出现 2 次（batch 7 的 `809473` 同样重复），向引擎请求两次恢复的是**同一节点集合**，所以逐字节复核的分母必须是「实际请求的唯一 id 数」。

- `tools/verify_engine_plan_batch.py` 新增 `request_scope()`：以 `ids.txt` 的唯一 id 集为分母；`declared − unique` 必须由**字面重复行**完全解释；`hashes.csv` 的 id 集合必须与请求唯一集精确相等；另加 `covered`（已验证 id 集 == 请求 id 集）强校验。复核记录新增 `requestScope` / `effectiveStats.requestedDeclared`。原始批次记录与 `plan_engine.json` **一行未改**。
- `tools/verify_archive.py::ep_valid()` 自己重读 `ids.txt` 独立重推 declared / lines / unique / duplicates / hash id 集，逐字段比对复核文件的 `requestScope` 与 `effectiveStats`，不信任其自述结论。
- 可证伪门禁：`duplicate_scope_self_test()` 覆盖「合法重复」「真缺失一行」「外来 hash id」「hash 缺 id」「多声明」5 例；`--negative-guard-duplicate-ids` 人为多声明 1 个 id，实测 **EXIT=1** 并报 `declared-unique deficit 2 not fully explained by 1 duplicate id rows`；跨卷负向仍 EXIT=1。
- 复核实测：批次 5 **249,455 / 249,455 唯一 id，unresolved=0**，17 条 `match=0` 全部 `exactSha256Match`（11 个 oracle ADS 假 mismatch + 6 个引擎独有 ADS 节点经生产引擎 replay 补齐）；批次 4 重生成 2/2 精确 SHA-256；归档中途快照 `--allow-partial` **0 errors / 0 warnings / EXIT=0**。篡改 `requestScope.requestedUniqueIds`（249455→249456）后归档门禁 **errors=6 / EXIT=1**（`requestScope not reproducible`），逐字节恢复后正向 EXIT=0，证据 `archive_verification_negative_p09.json`。`verify_engine_scan.py` 复验仍为 `ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS`；引擎定向 CTest **15/15 Passed（40.96 s）**；源盘复采 `SOURCE DISK UNCHANGED`。
- 自动收尾守护 `tools/finalize_recovery.py`：等待 7 批全部落盘后按序执行 `status → verify_engine_plan_batch --all-completed → verify_engine_batch --all-completed → cleanup → forensic_baseline --verify → verify_archive（严格）→ make_summary`。复核严格排在 cleanup 之前（replay 依赖 staging），关键步骤失败即停且保留 staging；cleanup 为 warning 级（其内容全是零字节壳，空间已在逐批释放时回收）。状态与每步 rc 落 `finalize_state.json`，日志 `finalize.log`。
- 守护新增 `--resume`（保留已 rc=0 的步骤续跑）与 `--skip-cleanup`（把空壳清扫记为 **DEFERRED** 并分项计量 `batchShellBytes` / `auxProbeBytes`，repeated 运行幂等替换而非追加）。这样「验证」与「几小时的目录删除」解耦：门禁结论在删除前就已定稿，删除进度不再能改变结论。

**最终独立审计**（`tools/final_audit.py`，不信任任何自述结论，逐批从 ids.txt / hashes.csv / 复核记录重推）：状态 `PASS`，ALL FILES RECOVERED AND BYTE-VERIFIED BY PHOENIX ENGINE; SOURCE DISK UNCHANGED；检查 65 项、失败 0 项；全量 declared=878,377 / distinct=878,375 / effectiveVerified=878,375 / unresolved=0。负向守卫 `--negative-guard`（少算 1 个文件）必须 EXIT=1。

本阶段仅修改 `tools/` 下验证与编排脚本，**生产引擎源码未改**，源盘写操作 0 次。
