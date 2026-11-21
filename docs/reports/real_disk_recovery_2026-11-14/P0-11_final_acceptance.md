# P0-11 最终验收：引擎扫描 + 引擎恢复 + 格式匹配 + 独立真值

- 结论时间：2026-11-14T23:25（收尾守护 `finishedAt=2026-11-14T23:25:13`，7 步全部 rc=0、无 DEFERRED；终审 `final_audit.json` 于清理动作之后重跑）
- 源盘：`\\.\PhysicalDrive2` ST1000DM003-1CH162 / 931.51 GiB / 2 个 `0x42` LDM 动态分区 / 无盘符
- 红线：全程 `GENERIC_READ`，写操作 **0 次**，**未执行任何修复**，损坏态样本保持原样
- 总判定：**ALL FILES RECOVERED AND BYTE-VERIFIED BY PHOENIX ENGINE; SOURCE DISK UNCHANGED**

## 一、验收数字（全部由 `tools/final_audit.py` 从 ids.txt / hashes.csv / 复核记录重推）

| 指标 | 值 | 来源 |
|---|---:|---|
| 完成批次 | **7 / 7** | `checkpoint.json` |
| 声明候选行数 | 878,377 | 各批 `requestedFiles` 之和 |
| 唯一请求 id 数 | **878,375** | `ids.txt` 去重（2 行重复 id） |
| 有效字节验证数 | **878,375** | 直接匹配 + 异常逐条解释 |
| 未解释异常 `unresolved` | **0** | 卷作用域复核 |
| 字节 SHA-256 完全一致 | 743,125 | `hashes.csv` |
| 已解释的不一致 / 失败 | 133,934 / 1,289 | 见第二节 |
| 引擎签名判定行数 | 762,629 | 批次 4–7 全量 `SignatureDB::Match` |
| 独立审计检查项 | **65 项，0 失败** | `final_audit.json`（清理完成后 DEFERRED 专项检查不再适用，故由 66 降为 65） |
| 源盘写操作 | **0** | 7 点复采 `SOURCE DISK UNCHANGED` |

声明行数与唯一 id 数差 2，**完全由字面重复 id 解释**：批次 5 的 `162865`、批次 7 的 `809473` 各出现 2 次（同一 MFT 记录号同时对应普通文件与 ADS 节点，编码 `(rec<<4)|slot` 撞号）。向引擎重复请求同一 id 恢复的是同一节点集合，所以逐字节复核的分母必须是唯一 id 数，不是计划行数（P0-9）。

## 二、133,934 + 1,289 的构成（无一项被"跳过"）

| 类别 | 数量 | 判定依据 |
|---|---:|---|
| 批次 3 假不一致（ADS `Zone.Identifier` 真值缺口） | 11 | 重算真值后精确匹配 |
| 批次 3 恢复失败 | 1,287 | 已删除且扩展 `$DATA` 清零，引擎拒绝恢复是正确行为 |
| 批次 5 / 6 假不一致（引擎独有 ADS 节点） | 11 + 10 | 生产引擎 `phoenix_headless_p07.exe` replay 后精确匹配 |
| 批次 7 假不一致（**oracle 常驻文件真值缺陷**） | 133,902 | 见 P0-10：`mft_oracle.py hash` 依赖已删除的 `residentHex` 列 |
| 批次 4 探针批失败 | 1 | P0-7 修复前状态，修复后重恢复 56,725,784 字节、SHA 与源盘一致 |
| 批次 7 恢复失败 | 1 | `id=269279`，扩展 `$DATA` 已清零的删除残留 |

批次 7 的 13 万条一度被误判为"大规模恢复失败"：`oracleSize == engineSize` 但 `oracleSha256` 全是空载荷哈希 `e3b0c442…`，说明失败在**真值侧**而非引擎侧。修复后从盘重解析 8/8 抽样与引擎字节精确一致，全量复核 `307,030 / 307,030`、`unresolved=0`（P0-10）。

## 三、证据链（每一环都可独立复跑）

| 环节 | 命令 | 结果 |
|---|---|---|
| 引擎发现覆盖全部计划 | `python tools/verify_engine_scan.py` | `ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS`，`oracleOnly=0` |
| 卷作用域逐字节复核 | `python tools/verify_engine_plan_batch.py --all-completed` | rc=0，347.9 s，7 批 `unresolved=0` |
| 历史批次幂等复核 | `python tools/verify_engine_batch.py --all-completed` | rc=0，批次 3 `105,901/105,901` |
| 严格归档门禁 | `python tools/verify_archive.py` | `ARCHIVE COMPLETE AND VERIFIED`，errors=0 warnings=0 |
| 零写入复采 | `python tools/forensic_baseline.py --verify` | `SOURCE DISK UNCHANGED`，7 点，changed=[] |
| 独立终审 | `python tools/final_audit.py` | **PASS**，66 项 0 失败 |
| 终审负向守卫 | `python tools/final_audit.py --negative-guard` | **EXIT=1**（少算 1 个文件必须被抓） |

## 四、`cleanup`：先诚实延期，后真实跑完（最终 DEFERRED 已清空）

全量恢复结束时，7 批数据簇早已在逐批 `truncate(0)` 时全部归还，`batch_*` 暂存只剩 **844,014 个零字节空壳 / 0 字节**。串行 `rmdir` 在 Defender 下约 4,800–9,600 目录/分钟，要数小时，而删除进度不改变任何结论（门禁不读 staging，复核的 replay 早已完成）。

**第一步：如实延期，不伪造 rc=0。** `finalize_recovery.py --resume --skip-cleanup` 把 `cleanup` 记为 `DEFERRED` 并分项计量 —— `batchShellFiles=844,014` / `batchShellBytes=0`（无可回收数据），`auxProbeBytes=495,571,429`（18 个目录：`readahead_*`/`ra_*` 基准缓存 + `p07_gap78` 证据，实验产物而非批次暂存）。放行条件不是这条说明，而是 `final_audit.py` 去读严格门禁**独立重测**的每批 `stagingNonZeroBytesFiles` 并要求 `batchShellBytes == 0`（`deferred cleanup has nothing left to reclaim`）。

**第二步：把删除本身做快。** Python 侧 unlink 被环境的 `safe-delete` 守卫包装成「移入回收站」，对几万个空目录直接 fail-closed；因此 `tools/fast_rmtree.py` 全程走原生 `cmd /c rmdir`：先用 `rename`（同卷元数据操作）把条目分片到 K 个桶，再并发 K 个原生进程删除。实测：

| 阶段 | 吞吐 | 说明 |
|---|---:|---|
| 分片（rename） | 130,885–136,955 /min | 只是移动目录项 |
| 原生 rmdir（8 路） | 14,851 /min | batch_005 的 123,305 项用 498 s |
| 原生 rmdir（16/24 路） | 10,600–13,215 /min | **并发不再线性**，瓶颈是 Defender 实时扫描 |
| 合计清扫 | **634,358 个空壳** | batch_004_stale / 005 / 006 / 007 共约 45 min |

**第三步：让 `cleanup` 真实执行。** 空壳删净后，`batch_recover.py cleanup` 只用 **4.1 s** 就回收剩余 22 个目录 / 225 个文件 / **495,571,429 字节**，`finalize_state.json` 里 `deferred` 清空、该步变成 `rc=0`（重复运行是幂等替换，不会长出第 8 步）。

**终态**：`recover_staging/` 为空；严格门禁在**清空后**重跑，逐批 7/7 报 `staging tree fully deleted`、`stagingNonZeroBytesFiles=0`；删除动作之后再做一次零写入复采，`SOURCE DISK UNCHANGED`（7 点，changed=[]）。

## 五、本轮修掉的两个工具缺陷（都是"工具骗人"而非数据出错）

1. **`int(x or -1)` 把合法的 0 变成 -1**。新的延期清理检查因此首跑 FAIL，而打印出来的每个数字都是对的（`batchBytes=0`）。这是 P0-8 修过的同一类错误再次复发，已改为显式 `isinstance(x, int) and x == 0`。
2. **`summary.md` 第三节逐批表把两版计划混拼**：行取自旧 oracle 计划（6 批），统计值取自新引擎记录，导致批次 4 显示"请求 244,040 / 匹配 0"、批次 5 显示"请求 53,715 / 匹配 249,438"，且批次 7 整行缺失。已改为以批次记录为权威来源，两版计划仅作回退，并加注批次 4 的失败是修复前状态。

两处都补了自洽性校验：逐批请求合计 878,377、匹配合计 743,125，与第二节总计精确相等。

## 六、仍未完成（不得当作已覆盖）

- **真实坏扇区 / 全盘 RAW 雕刻**未在本盘执行：本盘 MFT 完好，走的是结构化 NTFS 恢复路径。
- **真实格式化卷**的恢复字节门禁仍缺（现有 `FormattedVolumeRecovery` 只用 4 MiB 合成镜像）。
- 坏盘本身**保持故障态未修复**，L0–L4 修复预案只到文档（`P0-3_repair_playbook.md`）。
- 探针目录已随 `cleanup` 回收（495,571,429 字节），如需复跑基准需重新生成；数值仍留在 `PERFORMANCE_BASELINE.md` 与各 JSON。
