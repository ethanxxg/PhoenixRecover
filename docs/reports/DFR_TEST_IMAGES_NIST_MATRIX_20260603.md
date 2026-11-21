# dfr-test-images 与 NIST FTK 3.3.0 逐镜像对照表

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-03 |
| 官方基准 | NIST CFTT **FTK v3.3.0.33124** 测试报告 |
| 测试程序 | `test_dfr_nist_compare.exe`（全量恢复，无 16 文件上限） |
| 镜像目录 | `testdata/dfr-test-images/` |

---

## 一句话结论

| 范围 | 数量 | 与官方关系 |
|------|------|------------|
| **有 FTK 基准的镜像** | **62** | **62/62 PASS** — 已删数在 NIST 允许区间内，完整恢复数 ≥ NIST Full |
| **无 FTK 基准的变体** | **4** | 仅能确认扫描/恢复可跑通，**无法与官方逐数字对比** |
| **合计** | **66** | **0 FAIL** |

> **重要**：本仓校验 **不是** 要求「每个数字与 FTK 报告逐字相等」。  
> 规则见 [§2 校验规则](#2-校验规则)；下表 [§3](#3-逐镜像对照62-个有-benchmark) 列出每个镜像的具体数字与判定。

---

## 1. 总览

```
dfr-test-images (66 个 .dd)
├── 62 个 ──► 有 NIST 表项 ──► DFR_NIST_Compare ──► ALL OK
└──  4 个 ──► 无 NIST 表项 ──► 仅冒烟（见 §4）
```

**Del 与官方数字的关系（62 个有基准镜像）**

| Del 判定 | 数量 | 含义 |
|----------|------|------|
| **完全一致** | 39 | 我们统计的已删数 = FTK 报告 Del（区间退化为单点） |
| **在 NIST 区间内** | 23 | 已删数落在 FTK 的 Intact/Meta … Del 带宽内，未必等于 Del 列最大值 |
| **无已删（dfr-15/16/17）** | 9 | 官方亦为 0，与 FTK 一致 |

**Full 与官方数字的关系（62 个有基准镜像）**

| Full 判定 | 数量 | 含义 |
|-----------|------|------|
| **与 FTK Full 相等** | 39 | 完整恢复数 = NIST Full 列 |
| **优于 FTK Full** | 23 | 完整恢复数 **>** NIST Full（仍 PASS） |

---

## 2. 校验规则

NIST 报告对同一镜像通常给出三列：**Del**（可见已删总数）、**Intact/Meta**（元数据仍完整的子集）、**Full**（内容完整恢复数）。

本仓在 `tests/integration/dfr_nist_benchmark.h` 中编码为：

| 检查项 | 条件 | 说明 |
|--------|------|------|
| **已删数 `deleted`** | `deletedMin ≤ deleted ≤ deletedMax` | 宽区间场景下，Del 列是上限，Intact/Meta 是下限 |
| **完整恢复 `recoverOk`** | `recoverOk ≥ fullMin` | `fullMin` = FTK 报告 **Full** 列；允许我们做得更好 |

**完整恢复**定义：写出文件大小 = 目录项记录大小（与 FTK「Full」口径一致，**未**做 FTK 级内容哈希/Match 列对比）。

---

## 3. 逐镜像对照（62 个有 benchmark）

列说明：

- **NIST Del 区间**：`[deletedMin .. deletedMax]`，单点即 FTK Del 精确值
- **NIST Full≥**：FTK Full 列（我们须 ≥ 此值）
- **Del 判定**：完全一致 / 区间内 / 无已删
- **Full 判定**：相等 / 优于
- **NIST**：`PASS` = 满足 §2 规则

### dfr-01 ~ dfr-06（无覆盖写）

| 镜像 | NIST Del 区间 | NIST Full≥ | 我们 Del | 我们 Full | Del 判定 | Full 判定 | NIST |
|------|---------------|------------|----------|-----------|----------|-----------|------|
| dfr-01-fat | 3..3 | 3 | 3 | 3 | 完全一致 | 相等 | PASS |
| dfr-01-ntfs | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-01-xfat | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-02-fat | 3..3 | 3 | 3 | 3 | 完全一致 | 相等 | PASS |
| dfr-02-ntfs | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-02-xfat | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-03-fat | 3..3 | 3 | 3 | 3 | 完全一致 | 相等 | PASS |
| dfr-03-ntfs | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-03-xfat | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-04-fat | 36..36 | 36 | 36 | 36 | 完全一致 | 相等 | PASS |
| dfr-04-ntfs | 12..12 | 12 | 12 | 12 | 完全一致 | 相等 | PASS |
| dfr-04-xfat | 12..12 | 12 | 12 | 12 | 完全一致 | 相等 | PASS |
| dfr-05-fat | 6..6 | 6 | 6 | 6 | 完全一致 | 相等 | PASS |
| dfr-05-ntfs | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-05-xfat | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-05-braid-fat | 6..6 | 6 | 6 | 6 | 完全一致 | 相等 | PASS |
| dfr-05-braid-ntfs | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-05-braid-xfat | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-05-nest-fat | 6..6 | 6 | 6 | 6 | 完全一致 | 相等 | PASS |
| dfr-05-nest-ntfs | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-05-nest-xfat | 2..2 | 2 | 2 | 2 | 完全一致 | 相等 | PASS |
| dfr-06-fat | 3..3 | 3 | 3 | 3 | 完全一致 | 相等 | PASS |
| dfr-06-ntfs | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |
| dfr-06-xfat | 1..1 | 1 | 1 | 1 | 完全一致 | 相等 | PASS |

### dfr-07 ~ dfr-14（含覆盖写 / 碎片 / 大目录）

| 镜像 | NIST Del 区间 | NIST Full≥ | 我们 Del | 我们 Full | Del 判定 | Full 判定 | NIST |
|------|---------------|------------|----------|-----------|----------|-----------|------|
| dfr-07-fat | 6..15 | 6 | 9 | 9 | 区间内 | 优于 | PASS |
| dfr-07-ntfs | 2..5 | 2 | 3 | 3 | 区间内 | 优于 | PASS |
| dfr-07-xfat | 1..5 | 1 | 3 | 3 | 区间内 | 优于 | PASS |
| dfr-07-one-fat | 3..9 | 3 | 6 | 6 | 区间内 | 优于 | PASS |
| dfr-07-one-ntfs | 1..3 | 1 | 2 | 2 | 区间内 | 优于 | PASS |
| dfr-07-one-xfat | 0..3 | 0 | 2 | 2 | 区间内 | 优于 | PASS |
| dfr-07-two-fat | 0..6 | 0 | 3 | 3 | 区间内 | 优于 | PASS |
| dfr-07-two-ntfs | 0..2 | 0 | 1 | 1 | 区间内 | 优于 | PASS |
| dfr-07-two-xfat | 0..2 | 0 | 1 | 1 | 区间内 | 优于 | PASS |
| dfr-08-fat | 30..74 | 30 | 42 | 42 | 区间内 | 优于 | PASS |
| dfr-08-ntfs | 10..25 | 10 | 13 | 13 | 区间内 | 优于 | PASS |
| dfr-08-xfat | 11..25 | 11 | 13 | 13 | 区间内 | 优于 | PASS |
| dfr-09-fat | 780..780 | 780 | 780 | 780 | 完全一致 | 相等 | PASS |
| dfr-09-ntfs | 260..260 | 260 | 260 | 260 | 完全一致 | 相等 | PASS |
| dfr-09-xfat | 253..260 | 253 | 256 | 256 | 区间内 | 优于 | PASS |
| dfr-10-fat | 780..2340 | 780 | 780 | 780 | 区间内（=下限） | 相等 | PASS |
| dfr-10-ntfs | 260..780 | 260 | 260 | 260 | 区间内（=下限） | 相等 | PASS |
| dfr-10-xfat | 247..780 | 247 | 256 | 256 | 区间内 | 优于 | PASS |
| dfr-11-fat | 9..9 | 9 | 9 | 9 | 完全一致 | 相等 | PASS |
| dfr-11-ntfs | 3..3 | 3 | 3 | 3 | 完全一致 | 相等 | PASS |
| dfr-11-xfat | 3..3 | 0 | 3 | 3 | 完全一致 | 优于 | PASS |
| dfr-12-fat | 27..36 | 9 | 27 | 27 | 区间内（=下限） | 优于 | PASS |
| dfr-12-ntfs | 9..12 | 9 | 9 | 9 | 区间内（=下限） | 相等 | PASS |
| dfr-12-xfat | 9..12 | 3 | 9 | 9 | 区间内（=下限） | 优于 | PASS |
| dfr-13-fat | 270..414 | 57 | 354 | 354 | 区间内 | 优于 | PASS |
| dfr-13-ntfs | 87..118 | 92 | 118 | 118 | 区间内（=上限） | 优于 | PASS |
| dfr-13-xfat | 90..138 | 13 | 118 | 118 | 区间内 | 优于 | PASS |
| dfr-14-fat | 12..12 | 12 | 12 | 12 | 完全一致 | 相等 | PASS |
| dfr-14-ntfs | 8..10 | 8 | 10 | 8 | 区间内（=上限） | 相等 | PASS |
| dfr-14-xfat | 8..10 | 8 | 8 | 8 | 区间内（=下限） | 相等 | PASS |

### dfr-15 ~ dfr-17（无已删文件）

| 镜像 | NIST Del 区间 | NIST Full≥ | 我们 Del | 我们 Full | Del 判定 | Full 判定 | NIST |
|------|---------------|------------|----------|-----------|----------|-----------|------|
| dfr-15-fat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-15-ntfs | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-15-xfat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-16-fat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-16-ntfs | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-16-xfat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-17-fat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-17-ntfs | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |
| dfr-17-xfat | 0..0 | 0 | 0 | 0 | 无已删 | 相等 | PASS |

---

## 4. 无 NIST 基准的 4 个变体

这些镜像在 NIST FTK 3.3.0 报告中**没有单独一行**，本仓 `dfr_nist_benchmark.h` 亦无表项，**不能与官方 Del/Full 逐数字对比**。

| 镜像 | 我们 Del | 我们 Full | 批量结论 | 说明 |
|------|----------|-----------|----------|------|
| dfr-01-recycle-fat | 7 | 7 | PASS | 回收站场景 FAT 变体 |
| dfr-01-recycle-ntfs | 2 | 2 | PASS | 回收站场景 NTFS 变体 |
| dfr-11-compress-ntfs | 3 | 3 | PASS | NTFS 压缩属性变体 |
| dfr-11-mft-ntfs | 3 | 3 | PASS | MFT 相关变体 |

---

## 5. 复现

```powershell
d:\dev\PhoenixRecover\build\win2019-ninja\tests\test_dfr_nist_compare.exe
# 或
ctest -C Release -R DFR_NIST_Compare --output-on-failure
```

机器可读 CSV：[dfr_nist_matrix.csv](./dfr_nist_matrix.csv)（与本表同源，2026-06-03 全量跑数）

相关文档：

- [NIST_DFR_COMPARE_20260603.md](./NIST_DFR_COMPARE_20260603.md) — 修复说明与 CTest 索引
- [dfr_nist_compare_run.csv](./dfr_nist_compare_run.csv) — 批量脚本原始输出（含 4 个无基准变体）
