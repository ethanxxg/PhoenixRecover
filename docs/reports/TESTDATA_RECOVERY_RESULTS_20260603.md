# testdata 逐镜像恢复结论

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-03 |
| 工具 | `test_dfr_windows_batch.exe`、CFReDS/DFTT 专用 CTest |
| 原始数据 | [dfr_windows_recovery_20260603.csv](./dfr_windows_recovery_20260603.csv)（66 行） |

> 与 [WINDOWS_TEST_REPORT_20260603.md](./WINDOWS_TEST_REPORT_20260603.md)（47 项 CTest）互补。

---

## 0. 如何读这些数字

| 术语 | 含义 |
|------|------|
| **总文件数** | 扫描到的全部节点（正常 + 已删 + 目录） |
| **已删除** | 标记为 Deleted 的普通文件数（FAT/exFAT **多分区求和**） |
| **成功恢复** | 写出后**磁盘大小与目录项一致**的次数（`PHOENIX_DFR_MAX_RECOVER=0` 为全量） |
| **PASS** | 扫描 OK 且至少恢复 1 个已删（无已删 → WARN） |

**FAT 多分区**：「已删除」与「成功恢复」均为各分区**之和**（与 NIST Del/Rec 统计一致）。  
快速 CTest 批量默认 `PHOENIX_DFR_MAX_RECOVER=16`；全量 NIST 对比见 `DFR_NIST_Compare`。

---

## 1. `testdata/cfreds/`（7 个镜像）

| 镜像 | FS | 已删除 | 成功恢复 | 结论 | 说明 |
|------|-----|--------|----------|------|------|
| `dfr-01-fat.dd` | FAT | 1* | 3 | PASS | 三分区各 1 删 |
| `dfr-01-ntfs.dd` | NTFS | 1 | 1 | PASS | `Bunda.txt` |
| `dfr-01-xfat.dd` | exFAT | 2 | 2 | PASS | |
| `dfr-05-fat.dd` | FAT | 2* | 6 | PASS | 多分区 |
| `dfr-05-ntfs.dd` | NTFS | 2 | 2 | PASS | |
| `dfr-05-braid-fat.dd` | FAT | 2* | 6 | PASS | braid FAT |
| `dfr-05-braid-ntfs.dd` | NTFS | 2 | 2 | PASS | braid NTFS |

\* FAT「已删除」为批量列值（分区最大值）。

### `dfr-01-fat.dd` 明细

MBR 三分区（FAT16 + FAT16 + FAT32），专用测试 `CFReDS_FAT_DFR01` 与批量测试均为 PASS：**共 3 个已删，3 个恢复成功**。

---

## 2. `testdata/dfr-test-images/`（66 镜像）

| 结论 | 数量 | 说明 |
|------|------|------|
| PASS | 60 | 至少恢复 1 个已删 |
| WARN | 6 | 无已删（dfr-15/16/17 等） |
| FAIL | 0 | |

### 典型镜像

| 镜像 | 已删除 | 成功恢复 | 说明 |
|------|--------|----------|------|
| `dfr-01-recycle-ntfs.dd` | 2 | 2 | 回收站 NTFS |
| `dfr-04-fat.dd` | 12 | 36 | 多分区累加 |
| `dfr-06-fat.dd` | 1 | 3 | 大文件，三分区 |
| `dfr-09-fat.dd` | 260 | 48 | 大量已删，仅试 16 个 |
| `dfr-15-fat.dd` | 0 | 0 | WARN，无已删 |
| `dfr-05-braid-xfat.dd` | 3 | 1 | MBR exFAT |

完整 66 行见 CSV。

---

## 3. DFTT 金标（比批量更严）

| 镜像 | CTest | 验证 |
|------|-------|------|
| `6-fat-undel.dd` | DFTT_FAT_RecoverBytes | 6 文件 MD5 |
| `7-ntfs-undel.dd` | DFTT_NTFS_RecoverBytes | 8 文件 MD5 |
| `11-carve-fat.dd` | DFTT_11_CARVE_FAT | `paul.jpg` MD5 |

---

## 4. 其它 testdata

| 数据源 | CTest | 结论 |
|--------|-------|------|
| `custom-carving-images/carving-03.dd` | CustomCarving_ManifestGolden | manifest 5 偏移 OK |
| **`custom-carving-images/`**（20 镜像 × 55 文件） | Validate-CustomCarvingManifest + RawScan 批测 | **布局 1100/1100 PASS**；carve 见 [CUSTOM_CARVING_IMAGES_REPORT_20260605.md](./CUSTOM_CARVING_IMAGES_REPORT_20260605.md) |
| `file-carving-images/L0_Graphic.dd` | FileCarving_L0_Graphic | jpg 偏移金标 OK |
| 128 个 `.dd`/`.raw` | Testdata_ImageSmoke | 全部可读 |
| 55 个 `.E01` | Testdata_EwfExportReady | 无 ewfexport 时 SKIP |
| **`file-carving-cftt-images/`**（8 `.dd`） | 扇区 0 抽检 | **8/8 PASS**（2026-06-05） |
| **`sleuthkit_test_data/`**（6 `.dd`） | 扇区 0 抽检 | **6/6 PASS**；12 E01 源完整，其余 E01→dd 待转换 |
| **`digital-corpora-disk-images/`**（9 可读 dd/raw） | DigitalCorpora_Batch_All | **恢复闭环 376/379（99.2%）**；见 [DIGITAL_CORPORA_DISK_IMAGES_REPORT_20260608.md](./DIGITAL_CORPORA_DISK_IMAGES_REPORT_20260608.md) |

### `file-carving-cftt-images/`（8 镜像，2026-06-05）

| 镜像 | 大小 | 扇区 0 | 说明 |
|------|------|--------|------|
| `graphic-basic_1305121231.dd` | 317 MB | OK | CFTT 图形雕刻 |
| `graphic-braid_1305121235.dd` | 137 MB | OK | |
| `graphic-disorder_1305121235.dd` | 258 MB | OK | |
| `graphic-nofill_1305121236.dd` | 317 MB | OK | |
| `graphic-notshifted_1305311317.dd` | 317 MB | OK | |
| `graphic-partials_1305121236.dd` | 226 MB | OK | |
| `graphic-shifted_1305311317.dd` | 317 MB | OK | |
| `graphic-simple-frag_1305121236.dd` | 318 MB | OK | |

路径：`images/images/`。归档 `graphic-images.tbz2` 仍保留于 `images/`。

### `sleuthkit_test_data/`（2026-06-05）

| 镜像 | 来源 | 扇区 0 | 说明 |
|------|------|--------|------|
| `from_brian/6-fat-undel.dd` | 本地/DFTT | OK | 与 DFTT #6 同源 |
| `from_brian/2-kwsrch-fat/fat-img-kw.dd` | zip 解压 | OK | FAT 关键字搜索 |
| `from_brian/3-kwsrch-ntfs/ntfs-img-kw-1.dd` | zip 解压 | OK | NTFS 关键字搜索 |
| `from_brian/4-kwsrch-ext3/ext3-img-kw-1.dd` | zip 解压 | OK | ext3 关键字搜索 |
| `from_brian/5-fat-daylight/daylight.dd` | zip 解压 | OK | FAT daylight |
| `nps-2009-hfsjtest1/image.gen1.dmg` | zip 解压 | — | HFS+（非 `.dd`，smoke 未含） |

12 个 `.E01` 源文件均在位；其余 E01→`.dd` 使用 `testdata/tools/Expand-EwfToRaw.ps1 -Roots sleuthkit_test_data`（**不覆盖已有非空 `.dd`**，除非 `-Force`）。

---

## 5. 复现

```powershell
# 全量 66 镜像 CSV
$env:PHOENIX_DFR_REPORT_CSV = "docs\reports\dfr_windows_recovery_20260603.csv"
$env:PHOENIX_DFR_WINDOWS_MAX_IMAGES = "0"
.\build\win2019-ninja\tests\test_dfr_windows_batch.exe

# 单镜像专用测试
.\build\win2019-ninja\tests\test_cfreds_fat.exe
.\build\win2019-ninja\tests\test_cfreds_ntfs.exe
```
