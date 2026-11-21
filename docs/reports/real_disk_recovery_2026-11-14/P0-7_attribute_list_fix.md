# P0-7 · 引擎 `$ATTRIBUTE_LIST` 缺陷发现、修复与端到端字节验证

日期：2026-11-14 · 对象：`\\.\PhysicalDrive2`（ST1000DM003-1CH162，两块 `0x42` LDM 分区）
前提：**源盘只读**（`GENERIC_READ`），零修复、零写入；`--verify` 仍为 `SOURCE DISK UNCHANGED`。
本阶段是 P0-6「扫描 / 恢复 / 格式匹配必须用引擎」之后的**引擎本体验证**产物：验证过程真的发现并修掉了一个生产引擎缺陷。

---

## 一、缺陷是怎么暴露的

P0-6 把「有哪些文件」改成引擎自己的 `NtfsParser::ScanMetadata/GetAllFiles()` 输出后，编排器直接读 `engine_nodes_vol*.csv`。全量对照发现两个节点：

| 卷 | id | 名称 | 引擎声明 size | clusterCount | residentSize | 引擎行为 |
|---|---|---|---|---|---|---|
| 63 | 78 | `aliworkbench.log.old` | 56,725,784 | **0** | 0 | `No recoverable data (no clusters and no resident data)` |
| 209728638 | 269279 | `535c4f30…a63.svn-` | 43,515,904 | **0** | 0 | 同上（批次 5–7 会遇到） |

`size > 0` 却没有任何 runlist，只可能是三件事之一：解析遗漏、文件被截断、或 `$FILE_NAME.realSize` 陈旧值。要区分必须从源盘重解析 MFT，不能靠猜。

## 二、独立真值重解析（只读）

直接读 MFT 记录 78，属性只有 `$STANDARD_INFORMATION(0x10)`、`$ATTRIBUTE_LIST(0x20)`、`$FILE_NAME(0x30)` — **基本记录里根本没有 `$DATA`**。$ATTRIBUTE_LIST 的 3 个 unnamed `$DATA` 条目指向扩展记录：

| lowestVCN | 扩展记录 | 该段 `$DATA` |
|---|---|---|
| 0 | 10902 | realSize=56,725,784，runlist 有效（`31 01 e9 9a 01 …`） |
| 4508 | 11004 | realSize=0（NTFS 只在 VCN0 段写权威大小），runlist 有效 |
| 9345 | 10737 | realSize=0，runlist 有效 |

结论：**文件数据是真实存在且可恢复的，引擎漏了**。引擎看到的大 size 其实来自 `$FILE_NAME.realSize`（恰好与 VCN0 段一致）；独立 oracle 的 `parse_record` 同样不跟随 `$ATTRIBUTE_LIST`，所以它报 0 字节 —— 两边都有盲区，不能互相印证，必须各自修。

id 269279 则不同：扩展记录 269280 的 `$DATA` 头 `realSize=0 / runlist 空 / 记录 flags=0（未占用）`，是删除/截断残留；43.5 MB 只是陈旧 `$FILE_NAME.realSize`。**引擎报「无可恢复数据」是正确的**，不算缺陷。

`tools/verify_engine_gaps.py` 把这套判定固化：从引擎自己的 CSV 找缺口 → 从源盘跟随 `$ATTRIBUTE_LIST` 重建 runlist（按 lowestVCN 排序、空洞补稀疏簇）→ 独立计算 SHA-256 → 与引擎声明 size 交叉核对。结果 `engine_gap_evidence.json`：

- 真实缺口 **1 个**（id 78，56,725,784 字节，真值 SHA-256 `4992458186e62e89…536c`）
- `noPayloadDeletedExtension` **1 个**（id 269279）
- 正向 EXIT=0；`--negative-guard` 篡改 id 78 声明 size +1 → EXIT=1（size 交叉核对被拒）

## 三、生产引擎修复（唯一改动的引擎源文件）

`fs/ntfs/NtfsParser.cpp`：

1. `ParseRecord` 遇到 resident `$ATTRIBUTE_LIST` 时保存其值；**仅当基本记录没有 unnamed `$DATA`** 才走扩展解析（不影响普通文件）。
2. `CollectAttrListDataRefs()` 解析条目，只取 `type=$DATA && nameLen==0`（命名 `$DATA` 是 ADS，由既有逻辑处理），按 `lowestVcn` 升序排序。
3. 对每个扩展记录：`MftRecordByteOffset()` 定位 → `ReadBytes` 读入 → `ApplyUSA` → 找 unnamed `$DATA` → 复用既有 `ParseDataAttr()` 解码 runlist。
4. 按 `lowestVcn` 把簇写入 `node.clusters`，段间空洞填 0（稀疏簇，`RecoverEngine` 原生支持）；**`dataSize` 只取 VCN0 段**，避免后续段的 0 覆盖真实大小；压缩标志一并继承。
5. 上限保护：单文件最多 `2^22` 簇（约 16 GiB @4 KiB），越界即停止，不递归扩展记录。

未改 `RecoverEngine`、未改 GUI、未改编排逻辑。

## 四、可证伪门禁 `NtfsAttributeList`

`tests/test_ntfs_attribute_list.cpp`：4 MiB 合成 NTFS 卷，基本记录只有 `$FILE_NAME` + `$ATTRIBUTE_LIST`，`$DATA` 拆到两个扩展记录；**条目故意按 VCN1、VCN0 的倒序写入**，不排序就会拼出错误簇序。

| 门禁 | 判定 |
|---|---|
| 权威大小来自 VCN0 段（VCN1 段 `dataSize=0` 不得覆盖） | `size == 1500` |
| 按 VCN 排序合并 | `clusters == [48, 49]`，不是 `[49, 48]` |
| 字节级恢复 | `RecoverEngine::RecoverBatch()` 写出的 1500 字节与 oracle 完全一致 |
| 扩展记录（无 `$FILE_NAME`）不产生额外节点 | `nodes == 1` |

正向 EXIT=0；`PHOENIX_NEGATIVE_ATTRLIST_GUARD=1` 去掉 `$ATTRIBUTE_LIST` → 4 项检查 FAIL（簇数、恢复成功数、失败数、字节存在），打印 `NEGATIVE GUARD OK`（与本项目 CTest-exe 负向约定一致）。

**定向回归 15/15 Passed（41.69 s）**：`EngineScanFormatMatch`、`NtfsAttributeList`、`NtfsAdsStream`、`RecoverOutputCollision`、`SignatureDBAC`、`SignatureDBLoad`、`RawScanAC`、`RawScanParallel`、`RawScanParallelWiring`、`DeepScanFallback`、`SignatureMatcher`、`ScanProgress`、`ExfatRecoverBytes`、`FormattedVolumeRecovery`、`DFTT_NTFS_RecoverBytes`。

## 五、真实盘端到端复核（引擎 → 字节 → 独立真值）

为避免覆盖后台正在跑的 P0-6 批次循环，修复后的 headless 以新快照名 `phoenix_headless_p07.exe` 编译；`_p06.exe` 保持原样供批次 5–7 使用。

```
phoenix_headless_p07.exe scan    --disk 2 --boot 63 --out engine_nodes_vol63_p07.csv
phoenix_headless_p07.exe recover --disk 2 --boot 63 --ids p07_gap78_ids.txt \
    --out-dir recover_staging/p07_gap78 --result batch_gap78_engine.csv --engine-format 1
```

| 项目 | 修复前 | 修复后 |
|---|---|---|
| id 78 `clusterCount` | 0 | **13,850** |
| `firstCluster` | -1 | 105,193 |
| `RecoverBatch` | failed=1 | **succeeded=1 / failed=0** |
| 恢复字节 | — | 56,725,784 |
| SHA-256 | — | `4992458186e62e89a61cffc34b90a99629ad5df80b41d9763a8f62397c9d536c` |
| 独立真值 SHA-256 | `4992458186e62e89…536c` | **完全一致** |

恢复阶段仍走引擎格式匹配：`declaredExt=old` → `engineExt=ini`、`formatAgreement=mismatch`（按载荷判定，不按文件名猜），`SignatureDB` 513 条签名加载正常。

`tools/verify_engine_fix.py` 固化该链路：前后两次引擎扫描 CSV 的簇数、引擎恢复结果 CSV 的 status/targetPath、恢复文件 SHA-256 与独立真值逐项比对 → `engine_fix_verification.json`，正向 EXIT=0，负向（篡改期望 SHA）EXIT=1。

## 六、口径修正与边界

- **P0-6 报告中「未改生产引擎」的表述已过期**：P0-6 验证阶段确实没改；P0-7 针对验证发现的缺陷修改了 `NtfsParser.cpp` 一处，并配套 CTest + 负向守卫 + 15/15 回归。归档的 P0-6 证据（CSV / 覆盖报告 / 批次 1–4 记录）全部保持原样，不重写。
- 后台批次 5–7 仍用 `_p06.exe`（修复前引擎）。因此 id 269279 在批次记录里仍会显示 failed，但已由 `verify_engine_gaps.py` 判定为 `noPayloadDeletedExtension`，属正确行为；批次 4 的 id 78 记录保留为「修复前失败」证据，其修复后成功结果单独归档在 `engine_fix_verification.json`，两者不互相覆盖。
- 修复不会改变其他节点的发现结果：`p07` 重扫卷 63 仍为 11,134 节点，仅 id 78 的 `clusterCount/firstCluster` 变化。
- 未做：1 TB 全盘 RAW 雕刻（本盘 MFT 完好，走元数据扫描）；真实坏扇区 / 碎片化机械盘的端到端提速量化；`sqlite` 内建签名 9 字节与注释不符的待办仍未处理。
- 源盘 `forensic_baseline.py --verify`：本阶段全部读取均为只读，结论 `SOURCE DISK UNCHANGED`。

## 七、证据文件

`engine_gap_evidence.json`、`engine_gap_evidence_negative.json`、`engine_fix_verification.json`、`engine_fix_negative.json`、`engine_nodes_vol63_p07.csv`、`p07_scan_vol63.log`、`p07_recover_gap78.log`、`batch_gap78_engine.csv`、`engine_ctest_results.json`（新增 `NtfsAttributeList` / `P0_7_attributeListFix` / 15/15 回归）。
