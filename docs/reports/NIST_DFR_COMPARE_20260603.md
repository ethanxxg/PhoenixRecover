# NIST CFReDS DFR 对比结论（dfr-test-images）

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-03 |
| 官方基准 | NIST CFTT **FTK v3.3.0.33124** 测试报告（Deleted / Intact/Meta / Full） |
| 验证方式 | 引擎全量恢复（无 16 文件上限）+ CTest `DFR_NIST_SpotCheck` |

---

## 1. 结论摘要

> **逐镜像数字对照** → [DFR_TEST_IMAGES_NIST_MATRIX_20260603.md](./DFR_TEST_IMAGES_NIST_MATRIX_20260603.md)（66 镜像：62 有 FTK 基准全部 PASS，4 变体无官方表项）

| 指标 | 结果 |
|------|------|
| dfr-test-images 合计 | **66** 个 `.dd` |
| **有 NIST 基准** | **62/62 PASS**（`DFR_NIST_Compare`，全量恢复） |
| **无 NIST 基准** | **4**（recycle×2、compress、mft）— 仅冒烟，无法逐数字对比 |
| 66 镜像快速批量 | **57 PASS / 9 WARN**（每分区最多 16 个恢复，≠ 全量 NIST） |
| exFAT 关键场景 | Del / Full **一致或优于** FTK |
| 引擎 Bug | 已修复（见 §2） |

**WARN（9 个）** 均为 dfr-15/16/17（官方亦无已删文件），与 NIST 一致，**不是缺陷**。

**校验含义**：已删数落在 FTK `[Intact/Meta .. Del]` 区间内，完整恢复数 **≥** FTK Full — **不是**要求每个数字与 FTK 报告逐字相等（详见对照表 §2）。

---

## 2. 本次修复的引擎问题（exFAT）

1. **`AllocationPossible` 清零**：已删流扩展项常无 `0x02` 位，此前不建簇链 → 恢复 0 字节。现对已删条目仍按 `firstCluster` + 大小建链。
2. **`NoFatChain` 误用**：已删/碎片文件（dfr-05 braid/nest）不再盲目顺序扩展，改为 FAT 链 + DFR `.txt` 碎片重组。
3. **已删目录内文件**：父目录已删（`0x05`）时，子项即使为 `0x85` 也标记为 Deleted（dfr-12）。
4. **Mac 元数据过滤**：跳过 `.Trashes` / `.fseventsd` 等，避免虚增 deleted 计数。
5. **DFR `.txt` 评分**：`FormatPayloadScore` 识别 `\nDFR\n` 结构，启用 exFAT 碎片重组。

---

## 3. 与 FTK 对齐的关键 exFAT 镜像

| 镜像 | NIST Del | NIST Full | 我们 deleted | 我们 full 恢复 |
|------|----------|-----------|--------------|----------------|
| dfr-02-xfat | 1 | 1 | 1 | 1 |
| dfr-03-xfat | 1 | 1 | 1 | 1 |
| dfr-05-xfat | 2 | 2 | 2 | 2 |
| dfr-05-braid-xfat | 2 | 2 | 2 | 2 |
| dfr-05-nest-xfat | 2 | 2 | 2 | 2 |
| dfr-06-xfat | 1 | 1 | 1 | 1 |
| dfr-12-xfat | 12（Intact/Meta **9**） | **3** | 9 | **9**（≥3） |

> dfr-12：官方 Del=12 含 3 个 Intact/None 项；可恢复元数据 **9** 个，我们全部完整恢复（优于 FTK Full=3）。

---

## 4. 批量 CSV 与官方的差异说明

| 现象 | 原因 |
|------|------|
| ~~FAT `deleted=260` vs 官方 Del=780~~ | **已修复**：FAT/exFAT 多分区改为 **求和** |
| ~~`rec_ok=16/48` on fat-09~~ | **已修复**：`PHOENIX_DFR_MAX_RECOVER=0` 全量恢复 |
| exFAT dfr-15/16/17 WARN | 镜像无 DFR 已删文件，与官方一致 |

### 环境变量（`test_dfr_windows_batch.exe`）

| 变量 | 默认 | 说明 |
|------|------|------|
| `PHOENIX_DFR_MAX_RECOVER` | `16`（严格模式自动 `0`） | 每分区最多尝试恢复的已删文件数；`0`= unlimited |
| `PHOENIX_DFR_NIST_STRICT` | 未设置 | `1` 时按 `dfr_nist_benchmark.h` 校验 Del/Full |
| `PHOENIX_DFR_ONLY_SUBDIR` | 未设置 | 例如 `dfr-test-images` 只测该目录 |
| `PHOENIX_DFR_WINDOWS_MAX_IMAGES` | `0` | 限制镜像数量（CTest 快速批量为 `12`） |
| `PHOENIX_DFR_REPORT_CSV` | 临时目录 | CSV 输出路径 |

### CTest 目标

| 测试名 | 说明 |
|--------|------|
| `DFR_Windows_Batch` | 12 镜像快速冒烟（16 文件/分区） |
| `DFR_NIST_SpotCheck` | 6 个关键 exFAT 镜像 |
| `DFR_NIST_Compare` | **dfr-test-images 全量** NIST 对比（timeout 2h） |
| `DFR_NIST_BatchStrict` | 批量脚本 + 严格模式 + dfr-test-images |

全量 NIST 对比：

```powershell
d:\dev\PhoenixRecover\build\win2019-ninja\_build_release.bat
ctest -C Release -R DFR_NIST_Compare --output-on-failure

# 或批量严格模式（写 CSV）
$env:PHOENIX_DFR_NIST_STRICT = "1"
$env:PHOENIX_DFR_MAX_RECOVER = "0"
$env:PHOENIX_DFR_ONLY_SUBDIR = "dfr-test-images"
$env:PHOENIX_DFR_REPORT_CSV = "docs\reports\dfr_nist_strict.csv"
.\build\win2019-ninja\tests\test_dfr_windows_batch.exe
```

---

## 5. 复现

```powershell
d:\dev\PhoenixRecover\build\win2019-ninja\_build_release.bat
ctest -C Release -R DFR_NIST_SpotCheck --output-on-failure
```

原始批量 CSV：[dfr_nist_compare_run.csv](./dfr_nist_compare_run.csv)
