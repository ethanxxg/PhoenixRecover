# P0-10 · 独立真值工具的常驻文件哈希缺陷（批次 7 的 13 万条假 mismatch）

日期：2026-11-14 20:46–21:20
范围：仅 `tools/mft_oracle.py`（独立真值/字节校验工具）；**生产引擎源码未改**；**已归档批次记录一行未改**；源盘写操作 0 次。
关联：`P0-1_diagnosis.md §六-septies`、`P0-9_plan_duplicate_id_scope.md`。

## 1. 现象

批次 7（卷 `209728638`，307,031 个请求）的 `batch_007.json` 出现反常量级：

```json
"stats": {"requested": 307031, "engineRows": 307033,
          "matched": 173113, "mismatched": 133902,
          "failed": 1, "missingOracle": 14, "duplicateIdNodes": 3}
```

此前批次 5 / 6 的 `mismatched` 分别只有 11 / 16。抽样 `batch_007_hashes.csv`：

| id | oracleSize | engineSize | oracleSha256 | engineSha256 |
|---:|---:|---:|---|---|
| 43 | 129 | 129 | `e3b0c442…`（空串 SHA） | `e1b9ce9b…` |
| 78 | 220 | 220 | `e3b0c442…` | `f97bf5a9…` |
| 80 | 220 | 220 | `e3b0c442…` | `db84b9a6…` |
| 82 | 308 | 308 | `e3b0c442…` | `1f819c7c…` |

`match=0` 共 133,920 行，其中 **133,902 行的 oracle 哈希都是空串 SHA-256**，而 `oracleSize` 与 `engineSize` 逐条**完全一致**。真值侧「知道大小、没有字节」，是典型的载荷读取缺失，不是引擎恢复错误。

## 2. 根因

`tools/mft_oracle.py::hash_ids()` 对常驻 `$DATA` 走的是：

```python
if resident:
    payload = bytes.fromhex(row.get("residentHex") or "")   # 旧逻辑
```

但 `candidates_vol209728638.csv` 的列名是
`id, volumeBootLBA, name, path, size, isDir, inUse, resident, compressed, compressionUnit, runs, firstLCN, ext, isSystem`
—— **根本没有 `residentHex` 列**（ADS 安全解析器改造 `parse_record` 时，扫描输出不再持久化常驻字节）。于是 `bytes.fromhex("") == b""`，每一个 `resident=1` 的文件都被算成空串哈希。

为什么前几批没暴露：批次 1–6 的 id 集合以非驻留大文件为主，常驻文件占比极低（批次 5/6 各 11/16 个 mismatch 中已包含少量同类，被 P0-5 的 `verify-ids` 从盘重解析顺带修正，但当时未追到 `hash_ids` 的这一层）。批次 7 覆盖大量**低 MFT 记录号的系统小文件**（43/78/80/82/84/86/87/93… 均为 68–460 字节常驻 `$DATA`），缺陷因此集中爆发。

## 3. 修复

`tools/mft_oracle.py::hash_ids()` 的 resident 分支改为**只读源盘重解析 MFT 记录**，与已有的 `verify-ids` 子命令口径一致：

```python
pos = vol.record_byte_offset(rid)
info = parse_record(disk.read(pos, vol.recordSize), vol.recordSize)
payload = info["resident"]
```

不再信任任何可能过期的候选 CSV 列。非驻留分支仍按 runlist 顺序读盘（保持原有 I/O 优化）。

**重要**：本轮没有改写 `batch_007.json`、`batch_007_hashes.csv`、`batch_007_oracle_hashes.json`。按项目一贯口径，原始记录保留 133,902 条原始 mismatch 作为证据，由 `batch_007_engine_plan_verification.json` 逐条解释。修复 `hash_ids` 是为了让独立真值工具今后可信，不是为了美化历史记录。

## 4. 验证

| 项目 | 结果 |
|---|---|
| 从盘重解析 8 个样本常驻 `$DATA` 与归档引擎 SHA-256 | **8/8 精确一致**（`e1b9ce9b…` / `f97bf5a9…` / `db84b9a6…` …） |
| `resolve_truth` 吞吐实测 | 200 id / 0.15 s ≈ 0.0007 s/id → 133,917 条约 **1.6 分钟**（MFT 顺序区缓存命中） |
| `verify_engine_plan_batch.py --batch 7` | **EXIT=0，307,030 / 307,030 唯一 id，unresolved=0**；133,917 条 `match=0` 全部解释 |
| 守护 `verify_engine_plan_batch.py --all-completed`（4–7 批） | **rc=0，347.9 s** |
| 唯一 `failed=1` | id 269279，`No recoverable data`，P0-7 已判定为删除残留（扩展 `$DATA` 清零），归类 `noPayloadDeletedExtension`，引擎拒绝恢复正确 |
| replay 15 ids | `succeeded=14 failed=1`（即 269279），不影响复核结论 |
| 归档门禁差值公式核对 | 133,920 `match=0` = 1 failed + 133,902 mismatch + 3 ADS-id 碰撞 + 14 missingOracle，**精确成立** |
| 源盘 | 只读，写操作 0 次 |

批次 7 的 `requestScope`：declared 307,031 / ids.txt 307,031 行 / **唯一 id 307,030**（`809473` 重复 1 次，P0-9 口径），`effectiveVerified=307,030`。

## 5. 口径声明

`checkpoint.totals.mismatched = 133,934` 是**原始记录口径**（批次 3 的 11 + 批次 5/6 的 27 + 批次 7 的 133,902 等），**不是恢复失败数**。全部异常都有逐字节复核记录解释：

- 批次 3：11 个 oracle ADS 假 mismatch + 1287 个输出路径冲突 retry → 105,901/105,901
- 批次 4：2/2（含 P0-7 `$ATTRIBUTE_LIST` 修复）
- 批次 5：249,455/249,455
- 批次 6：206,135/206,135
- 批次 7：307,030/307,030

有效口径为 **7 批全部请求的唯一 id 均逐字节验证通过、unresolved=0**。引擎发现源、恢复动作与 `SignatureDB` 格式匹配均未被本修复触碰。
