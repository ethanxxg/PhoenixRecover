# testdata 测试计划

> 验证报告索引：[README.md](./README.md)

## 1. 范围

`testdata/` 共 **15 个顶层目录**（约 2100 文件、~119 GB）。

| 阶段 | 工具 | 验证深度 |
|------|------|----------|
| L1 | CTest 47 项 | 算法、金标、指定镜像集成 |
| L2 | `Testdata_ImageSmoke` | 128 个 `.dd`/`.raw` 可 OpenImage |
| L3 | 脚本 / 手工 | E01 转换、素材库 repair、GUI |

**PASS 含义因阶段而异**（见 §4）。

---

## 2. 目录清单

### P0 — 已有 CTest 覆盖

| 目录 | 规模 | CTest | 状态 |
|------|------|-------|------|
| `cfreds/` | 6.1 GB | CFReDS_*、DFR_Windows_Batch | READY |
| `dftt/` | 341 MB | DFTT_* | READY |
| `dfr-test-images/` | 71.4 GB | DFR_Windows_Batch、DFR_Linux_Batch | READY（90 dd 已解压） |
| `custom-carving-images/` | 1.4 GB | CustomCarving_ManifestGolden、**CustomCarving_Batch_All** | READY（布局 1100/1100 PASS） |
| `file-carving-images/` | 480 MB | FileCarving_L0_Graphic | READY（30 dd 已解压） |
| `tools/` | 脚本 | GoldenBundle、RepairSamples | READY |

### P1 — 待扩展

| 目录 | 状态 | 说明 |
|------|------|------|
| `file-carving-cftt-images/` | READY | 8 个 graphic `.dd` 已解压（`images/images/`）；归档 `graphic-images.tbz2` 保留 |
| `testdisk-test-data/` | NEEDS_EXTRACT | 部分已解压到 dftt/ |

### P2 — 需转换或手工

| 目录 | 状态 | 说明 |
|------|------|------|
| `digital-corpora-disk-images/` | E01: NEEDS_EWF | 25 E01；`ubnist1.gen3.raw` 已 smoke；**批测 9/9 PASS**（2026-06-08） |
| `digital-corpora-disk-images.larkcache/` | 重复缓存 | 可忽略 |
| `sleuthkit_test_data/` | READY（部分 EWF） | 12 E01 + 5 zip 直出 `.dd` 已就绪；E01→`.dd` 用 `Expand-EwfToRaw.ps1 -Roots sleuthkit_test_data` |
| `napierone-*`、`govdocs1`、`filetypes1` | MANUAL | 单文件素材 |

### 运行时生成

| 路径 | 脚本 |
|------|------|
| `media/` | `testdata/tools/Fetch-MediaSamples.ps1` |
| `repair_samples/` | `scripts/Generate-RepairSamples.ps1` |

---

## 3. CTest 与 testdata 映射（47 项）

| CTest 组 | testdata |
|----------|----------|
| DFTT_* (6) | `dftt/` |
| CFReDS_* / DFR_* (12+) | `cfreds/`, `dfr-test-images/` |
| Fragment* / GraphCarve* (6) | fixture + `cfreds/dfr-05-braid-*` |
| Testdata_ImageSmoke | dftt、cfreds、dfr-test-images、custom-carving、digital-corpora raw、**file-carving-cftt-images**、**sleuthkit_test_data** |
| CustomCarving_ManifestGolden | `custom-carving-images/manifest.json` |
| CustomCarving_ManifestLayout / CustomCarving_Batch_All | 同上（1100 项布局 + RawScan 批测） |
| **DigitalCorpora_Batch_All** | `digital-corpora-disk-images` SectorReader + FS 扫描（9 可读镜像） |
| FileCarving_L0_Graphic | `file-carving-images/Images/L0_Graphic.dd` |
| Testdata_EwfExportReady | E01 转换（无 ewfexport → SKIP） |
| RepairSamples* | `repair_samples/` |

---

## 4. 结果含义

| 标记 | 含义 |
|------|------|
| CTest PASS | 该用例断言满足 |
| SMOKE PASS | 镜像可 OpenImage + 读扇区 0 |
| NEEDS_EWF | E01 需转 raw |
| NEEDS_EXTRACT | 压缩包未解压 |
| MANUAL | 非卷镜像素材 |

逐镜像恢复数字：[TESTDATA_RECOVERY_RESULTS_20260603.md](./TESTDATA_RECOVERY_RESULTS_20260603.md)

---

## 5. 执行命令

```powershell
# 一键：清单 + CTest + smoke
powershell -File scripts/Run-TestdataValidation.ps1

# 仅 CTest
cd build/win2019-ninja
ctest -C Release --output-on-failure

# 全量 DFR Windows 批量（66 镜像）
$env:PHOENIX_DFR_REPORT_CSV = "docs\reports\dfr_windows_recovery_20260603.csv"
$env:PHOENIX_DFR_WINDOWS_MAX_IMAGES = "0"
.\tests\test_dfr_windows_batch.exe
```

### 辅助脚本（`testdata/tools/`）

| 脚本 | 作用 |
|------|------|
| `Expand-FileCarvingImages.ps1` | 解压 file-carving `.dd.bz2` |
| `Download-CfttImages.ps1` / `Expand-CfttImages.ps1` | 下载 / 解压 CFTT `graphic-images.tbz2` |
| `Download-SleuthkitImages.ps1` | 下载 Sleuth Kit 测试镜像（E01 + zip） |
| `Expand-EwfToRaw.ps1` | E01 → dd（pyewf 或 ewfexport） |
| `Fetch-MediaSamples.ps1` | 生成 media 样本 |
| `Expand-DfrBz2.ps1` | 解压 DFR `.dd.bz2` |

---

## 6. 当前自动化覆盖（2026-06-05）

| 指标 | 结果 |
|------|------|
| CTest | 47/47 PASS |
| raw 镜像 smoke | 128/128 PASS（不含本次新增根；见下） |
| **file-carving-cftt-images** | 8 `.dd` 可读（Python 扇区 0 抽检 PASS） |
| **sleuthkit_test_data** | 6 `.dd` 可读（zip 直出 + `6-fat-undel`）；12 E01 源文件完整 |
| Windows DFR 批量 | 66 镜像，60 PASS / 6 WARN / 0 FAIL |
| E01 扫描/恢复 | sleuthkit 待 `Expand-EwfToRaw.ps1` 补全其余 E01→dd |

Windows 平台报告：[WINDOWS_TEST_REPORT_20260603.md](./WINDOWS_TEST_REPORT_20260603.md)
