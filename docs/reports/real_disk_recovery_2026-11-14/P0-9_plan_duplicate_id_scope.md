# P0-9 · 引擎驱动计划的重复 id 口径加固 + 自动收尾

日期：2026-11-14 17:45–18:05
范围：`tools/` 下验证与编排脚本；**生产引擎源码未改**；源盘写操作 0 次。
关联：`P0-6_engine_scan_format.md`、`P0-7_attribute_list_fix.md`、`P0-8_verification_gate_hardening.md`、`P0-3_repair_playbook.md §5.1`。

## 1. 触发

批次 5（卷 `209728638`，引擎发现源）的正式逐字节复核首次失败：

```text
batch_005: requested=249456 direct=249438 explained=17
           effective=249455/249456 unresolved=0 INCOMPLETE
   ERROR: batch_005 effective 249455 != requested 249456
EXIT=1
```

`unresolved=0` 却差 1，说明分母口径有问题，而不是有文件没恢复。

## 2. 根因：计划数是「行数」，恢复请求数是「唯一 id 数」

`plan_engine.json::files` 由 `engine_nodes_vol*.csv` 的**候选行**累加得到。引擎节点 id 是 MFT 记录索引，ADS 节点是 `(recordIndex << 4) | slot`；两种编码在数值上可能相撞，于是同一数值 id 会对应两个不同节点。计划把这两行都写进 ids 列表，但 `RecoverEngine::RecoverBatch` 按 id 查找节点，重复请求同一个 id 恢复的是**同一节点集合**。

| 批次 | 计划 `files` | `ids.txt` 行数 | 唯一 id | 重复 id | `engineRows` |
|---|---:|---:|---:|---|---:|
| 4 | 2 | 2 | 2 | — | 2 |
| 5 | 249,456 | 249,456 | **249,455** | `162865` ×2 | 249,459 |
| 6 | 206,135 | — | 206,135 | — | — |
| 7 | 307,031 | — | **307,030** | `809473` ×2 | — |

`batch_005_idCollisions` 已记录 4 个 id 各产生 2 行引擎结果（`162865` / `209073` / `240257` / `307105`）：一个是 `Zone.Identifier` ADS 节点，另一个是恰好同号的普通 MFT 记录。249,455 唯一 id + 4 个双行 id = 249,459 行，与归档 CSV 精确吻合。批次 7 的 `809473` 是同一现象，最终复核会遇到同样的 1 个差额。

## 3. 修复（不篡改任何已归档记录）

`tools/verify_engine_plan_batch.py`
- 新增 `request_scope(declared, id_lines, hash_ids)`：分母 = `ids.txt` 唯一 id 数；要求 `len(id_lines) == declared`、`declared − unique == sum(重复行数)`、`hashes.csv` 的 id 集合与请求唯一集**精确相等**。任何「真缺失一个 id」都不能靠「重复」搪塞。
- `build_record()` 写入 `requestScope`、`effectiveStats.requested`（唯一 id 数）、`effectiveStats.requestedDeclared`，并新增 `covered`（已验证 id 集 == 请求 id 集）强校验。门禁改为 `effectiveVerified == requestedUniqueIds`。
- 新增 `duplicate_scope_self_test()`：合法重复 / 真缺一行 / 外来 hash id / hash 缺 id / 多声明，5 例正向自检；失败即拒绝复核。
- 新增 `--negative-guard-duplicate-ids`：人为把 declared +1，必须被拒绝。
- 新增 `covered` 检查同时防止「多解释一个 id」被放过。

`tools/verify_archive.py::ep_valid()`
- 自己重读 `ids.txt` 独立重推 declared / lines / unique / duplicates / hash id 集，逐字段比对复核文件的 `requestScope` 与 `effectiveStats`；不信任复核文件自述结论（沿用 P0-5 起的原则）。

`tools/finalize_recovery.py`（新增）
- 轮询 checkpoint 直到 `[1..7]`，按 `§5.1` 顺序执行 7 步。复核严格在 cleanup 前；关键步骤 rc≠0 立即停止并**保留 staging**；cleanup 为 warning 级（全是零字节壳，数据簇已在逐批释放时回收）。每步 rc / 耗时 / 输出尾部写入 `finalize_state.json`，日志 `finalize.log`，`--status-only` 可查询，状态 `OK` 时幂等退出。

`tools/make_summary.py`
- 新增第十四节；第二节「6 批」措辞改为 **7 批**并说明分母口径；第十三节末尾的收尾状态改为依据 `finalize_state.json` 动态输出。

`P0-3_repair_playbook.md §5.1`
- 增补无人值守执行方式与 P0-9 分母口径说明。

## 4. 验证结果

| 项目 | 结果 |
|---|---|
| `py_compile` 三个脚本 | OK |
| 卷隔离自检 / 重复 id 自检 | PASS / PASS |
| `--batch 5` 正式复核 | **EXIT=0**，249,455/249,455，unresolved=0 |
| 批次 5 的 17 条 `match=0` | 全部 `exactSha256Match`（11 个 oracle ADS 假 mismatch + 6 个引擎独有 ADS 节点经 `phoenix_headless_p07.exe` replay 补齐） |
| `--batch 4` 重生成复核 | EXIT=0，2/2 精确 SHA-256（id 78 = `4992458186e6…`，id 66865 = `d829857eb136…`） |
| `--batch 5 --negative-guard-duplicate-ids` | **EXIT=1**，`ids.txt has 249456 lines != declared requested=249457`、`deficit 2 not fully explained by 1 duplicate id rows` |
| `--all-completed --negative-guard-cross-volume` | EXIT=1（跨卷泄漏被检出，P0-8 守卫未回退） |
| `verify_engine_batch.py --all-completed`（历史 1–3） | EXIT=0，批次 3 有效 105,901/105,901 |
| `verify_archive.py --allow-partial` | **0 errors / 0 warnings / EXIT=0** |
| 篡改 `requestScope.requestedUniqueIds`（249455→249456） | 归档门禁 **errors=6 / EXIT=1**（`engine-plan requestScope not reproducible`），逐字节恢复后正向 EXIT=0；证据 `archive_verification_negative_p09.json` |
| `verify_engine_scan.py` 正向复验 | `ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS`，6/6 批 `missing=0`，`oracleOnly=0`，EXIT=0 |
| 引擎定向 CTest 回归 | **15/15 Passed（40.96 s）** |
| `forensic_baseline.py --verify` | `SOURCE DISK UNCHANGED`（7 个采样点） |

## 5. 边界与未执行项

- 批次 6 / 7 仍在后台循环；守护 `finalize_recovery.py` 已启动，等待 checkpoint `[1..7]` 后自动完成 `--all-completed` 复核、cleanup、最终零写入复采、严格 `verify_archive.py` 与 `make_summary.py`。
- 本阶段验证的是**编排/复核口径**，不是引擎恢复能力；引擎字节级能力由 CTest `EngineScanFormatMatch` / `NtfsAttributeList` / `NtfsAdsStream` / `RecoverOutputCollision` 与真实盘回放共同证明。
- 重复 id 是引擎节点 id 编码的固有属性，`plan_engine.json` 与批次原始记录**保持原样不重写**；只在复核层显式解释。
- 全盘 RAW 雕刻、真实坏扇区、峰值 RSS、真实卷端到端提速仍未量化（沿用 `ROADMAP_RECOVERY.md §8`）。
