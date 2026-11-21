# P0-8 · 复核工具链自身的卷隔离与收尾口径加固

> 日期：2026-11-14 · 触发：用户要求"一定要用引擎去扫描和恢复，包括文件格式匹配"之后的持续引擎验证
> 约束：**源盘全程只读**（`GENERIC_READ`），不修复坏盘，零 GUI 改动；本报告只涉及验证/编排工具与文档，不改生产 `scan/ fs/ recover/` 源码。

---

## 1. 为什么必须做这一轮

`plan_engine.json` 把引擎发现清单切成 4 批（batch 4–7）。在准备最终收尾序列时发现一个**会让门禁误判的结构性问题**：

| 卷 | MFT 记录号 78 | 大小 | 性质 |
|---|---|---|---|
| 63 | `aliworkbench.log.old` | 56,725,784 B | P0-7 修复的 `$ATTRIBUTE_LIST` 真实缺口 |
| 209728638 | `2ca450e6adbefab331e42713eaf5dd75.appinfo` | 220 B | 完全无关的普通文件 |

而 id **269279**（卷 209728638，`...svn-base`）是 `noPayloadDeletedExtension`（扩展 `$DATA` 已清零），恰好也在 batch 7。

**节点 id 只在单个卷内唯一**（本质是 MFT 记录索引 / ADS 编码）。P0-7 写复核工具时用了全局 id 集合，会把两个卷的同号记录混为一谈。这不是理论风险：batch 7 同时含 `id=78` 与 `id=269279`。

---

## 2. 查出的四个真实缺陷（均在验证工具链，不是生产引擎）

| # | 位置 | 缺陷 | 后果 |
|---|---|---|---|
| 1 | `verify_engine_plan_batch.py` `gap_ids` | 从 `engine_gap_evidence.json` 的**所有卷**收集 id，且把 `noPayloadDeletedExtension` 也当成真缺口 | 卷 209728638 的 id 78 / 269279 可被卷 63 的 P0-7 结论"解释"，绕过字节级复核 |
| 2 | `load_ads_ids()` | 两个卷的 ADS id 合并成一个 set | 另一卷的同号普通记录被当 ADS 解析，真值 SHA-256 算错 |
| 3 | `replay_ids()` | 返回 `id -> {sha,size}` 全局字典 | 跨卷 replay 哈希串扰 |
| 4 | `fix_ok` | 匹配 `verdict.startswith("ENGINE ATTRIBUTE LIST FIX")`（空格），实际 verdict 是 `ENGINE ATTRIBUTE_LIST FIX ...`（下划线）；且 `int(0 or -1)` 得到 -1 | **`fix_ok` 恒为 False**，P0-7 解释分支从未真正生效（此前 batch 4 是靠 replay 的 `exactSha256Match` 通过的，掩盖了这个问题） |

附带：`noPayloadDeletedExtension` 在 `classify()` 中排在通用 `zeroLengthNoPayload` 之后，语义会被降级；replay 目录固定名字，重复运行会与上次留下的零字节壳冲突。

---

## 3. 修复内容

**`tools/verify_engine_plan_batch.py`**
- 所有解释键改为 `(bootLBA, id)`：`gap_keys`、`ads_by_boot`、`replay`。`gap_keys` 只纳入 `truthSize > 0`、无 `classification` / `noPayload` / `error` 的**真实缺口**（当前唯一元素 `(63, 78)`）。
- `fix_ok` 不再信任 verdict 字符串，改为逐字段重推：`engineRecoveredSha256 == independentTruthSha256 == expectedSha256Used`、`engineRecoveredSize == independentTruthSize`、`gapId == 78`、`beforeFixClusterCount == 0 < afterFixClusterCount`、`errors=[]`、`negativeGuard=False`。
- `classify()` 增加 `boot` 参数；`noPayloadDeletedExtension` 提到通用零长度分支之前。
- `replay_ids()` 目录名加运行时间戳 `p07_replay_vol{boot}_{run_tag}`，避免与历史零字节壳冲突。
- 新增 `volume_scoping_ok()` 双向自检：正确卷隔离下卷 209728638 的 id 78 必须 `UNRESOLVED`；故意泄漏 gap 键后必须被**错误地**解释（证明泄漏是可检测的）。自检不过直接拒绝复核。
- 新增 `--negative-guard-cross-volume`：模拟修复前的全局 id 泄漏，自检必须失败 → EXIT=1。

**`tools/verify_archive.py`**
- `ep_valid()` 校验复核记录的 `volumeBootLBA` 与批次记录一致。
- `classification == "recoveredByP07AttributeListFix"` 只允许出现在 `volumeBootLBA == 63 && id == 78`，否则直接 ERROR。

**收尾工具口径**
- `tools/batch_recover.py status`：合并 `plan.json`(1–3) + `plan_engine.json`(4–7) → `batches 4/7`，不再是 6 或 4。
- `tools/batch_recover.py cleanup`：扫描 `recover_staging/` 下**全部**子目录（含 `p07_*`、`batch_004_stale`）；对残留非零字节先用与批次相同的 `release_staging_tree()` 释放，再执行「0 bytes left」硬断言，最后原生 `rmdir /S /Q`。不会删除仍有数据的目录。
- `tools/verify_engine_batch.py`：`--all-completed` 只处理 batch ≤ 3；显式 `--batch >= 4` 直接报错（旧 schema 记录可能让 `ev_valid()` 误纳）。引擎口径批次一律走 `verify_engine_plan_batch.py`。
- `P0-3_repair_playbook.md §5.1`：收尾命令序列改为 7 批口径，并把复核移到 `cleanup` **之前**（replay 需要 staging）。

---

## 4. 验证结果（全部可证伪）

| 检查 | 期望 | 实测 |
|---|---|---|
| `verify_engine_plan_batch.py --batch 4 --dry-run` | 卷隔离自检 PASS | EXIT=0，`volume scoping self-test: PASS (1 volume-scoped gap keys)` |
| `--batch 4 --negative-guard-cross-volume` | 拒绝 | EXIT=1，`FAILED (leak detected)` |
| `--batch 4` 正向 | 2/2 精确 SHA | EXIT=0，`effective=2/2 unresolved=0`，id 78 与 66865 均 `exactSha256Match` |
| `--batch 4 --negative-guard` | 拒绝 | EXIT=1，id 78 `UNRESOLVED`（篡改 replay SHA + 移除 P0-7 解释） |
| `verify_archive.py --allow-partial` 正向 | 0 errors | EXIT=0 |
| 同上，篡改复核记录 `volumeBootLBA=209728638` | 拒绝 | EXIT=1，`engine-plan volumeBootLBA=209728638 != archived 63` |
| `verify_engine_batch.py --batch 4` | 拒绝 | EXIT=2，`batch 4 is engine-driven; use ... instead` |
| `verify_engine_batch.py --all-completed --dry-run` | 仅 1–3 | batch 001/002 无需复核，batch 003 `105901/105901 OK` |
| `batch_recover.py status` | 合并口径 | `batches 4/7 (merged plans)` |
| 跨卷真值解析探针（三节点） | 卷隔离 | vol63/78 → `recoveredByP07AttributeListFix`；vol209728638/78 → `baseRecordData` 220 B / SHA `f97bf5a9ed23…` → **UNRESOLVED**（未被另一卷的修复结论误解释）；vol209728638/269279 → `noPayloadDeletedExtension` |

`batch_004_engine_plan_verification.json` 已用修复后的工具重新生成（2/2 `exactSha256Match`），归档门禁复验通过。

---

## 5. 当前状态与边界

- **后台恢复循环仍在运行**：`batch_recover.py run --plan plan_engine.json`。batch 5 引擎阶段已完成（249,459 成功 / 0 失败 / 24.00 GiB），正在做独立 oracle 的逐文件 SHA-256 比对；checkpoint 仍为 `[1,2,3,4]`。batch 6、7 未开始。
- **最终收尾序列尚未执行**：`--all-completed` 复核、`cleanup`、最终 `forensic_baseline.py --verify`、严格 `verify_archive.py`、`make_summary.py` 都要等 checkpoint 到 `[1..7]` 后按 `P0-3_repair_playbook.md §5.1` 顺序跑。
- 中途快照 `forensic_baseline.py --verify` 仍为 `SOURCE DISK UNCHANGED`（7 个采样点，`changed=[]`）；本轮全部操作只读源盘。
- 归档的 P0-6 CSV、P0-7 修复前后证据、批次 1–4 原始记录**均未改写**（仅重新生成 batch 4 的复核记录，它由工具派生、非原始证据）。
- **收尾性能风险（已实测）**：两个 `Volume` 初始化本身只要 0.4 s，但在大盘与独立 oracle 哈希并发 I/O 时，重算卷 63 / id 78 的 56.7 MB 载荷 SHA 让三节点探针耗时 **212.9 s**，前台版本因此被 3 分钟超时 SIGTERM（独立 oracle 哈希进程当时累计读盘 184 GB）。最终 `--all-completed` 必须等恢复循环结束、磁盘空闲后再跑，并建议用后台任务 + 长超时。同理，最终 `cleanup` 要删除约百万量级的零字节壳目录，单次原生 `rmdir /S /Q` 曾被环境超时中断（18 万目录耗时 >7 分钟），需分段/后台执行，不能按"秒级完成"预期。
- 待办不变：批次 5–7 逐文件比对、最终 cleanup、全量严格归档门禁；内建 `sqlite` 签名 9 字节口径问题；真实磁盘端到端性能与误报率未量化。

---

## 6. 结论

引擎的扫描 / 恢复 / 格式匹配三段已由 P0-6 切入生产代码，P0-7 修掉了引擎自身的 `$ATTRIBUTE_LIST` 缺陷；**P0-8 修的是"证明引擎正确"的那套工具本身**——保证两个卷的同号 MFT 记录不会被交叉解释，且每一条解释都可在负向守卫下被推翻。收尾命令序列已与此口径对齐。
