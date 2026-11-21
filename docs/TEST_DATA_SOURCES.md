# PhoenixRecover 测试数据源清单（在线可获取）

> 目标：沉淀一批可长期复用的数据恢复测试样本来源，覆盖“删除恢复 / 文件系统解析 / 签名雕刻 / 预览与修复”。

## 1) 优先收集（建议先落地）

| 优先级 | 来源 | 主要用途 | 入口 |
|---|---|---|---|
| P0 | NIST CFReDS | 标准化取证/恢复测试镜像（含删文件、FS 场景） | [NIST CFReDS 介绍页](https://www.nist.gov/itl/ssd/software-quality-group/computer-forensics-tool-testing-program-cftt/cfreds)、[CFReDS 门户](https://cfreds.nist.gov/) |
| P0 | DFTT（Brian Carrier） | 经典 FAT/NTFS/EXT、关键字检索、Undelete、Carving 测试镜像 | [DFTT 主页](https://www.dftt.org/)、[SourceForge 文件列表](https://sourceforge.net/projects/dftt/files/) |
| P0 | Digital Corpora（NPS Test Disk Images） | 可公开教学/研究镜像，便于回归与工具对比 | [Disk Images](https://digitalcorpora.org/corpora/disk-images/) |
| P1 | Sleuth Kit Test Data | 与 TSK/Autopsy 生态一致的测试数据（若后续引入 TSK 尤其有用） | [sleuthkit_test_data](https://github.com/sleuthkit/sleuthkit_test_data) |
| P1 | DFRWS Forensic Challenges | 高难场景、挑战类样本（多源关联、IoT、carving 等） | [DFRWS Challenges](https://dfrws.org/forensic-challenges/) |

## 2) 扩展收集（类型覆盖/压力测试）

| 类别 | 来源 | 适用模块 | 入口 |
|---|---|---|---|
| 文件类型大样本 | Govdocs1 | 文件类型识别、扩展名分类、carving 误报控制 | [Govdocs1 / Digital Corpora](https://digitalcorpora.org/corpora/files/) |
| 媒体样本（音视频） | Xiph Test Media | 缩略图、解码、容器健壮性、修复前后对比 | [Xiph Test Media](https://media.xiph.org/) |
| 编解码回归样本 | FFmpeg FATE 样本集 | FFmpeg 路径可用性、边缘解码场景回归 | [FFmpeg FATE 文档](https://www.ffmpeg.org/fate.html) |

## 3) 合规与使用注意

- **CFReDS / DFTT / NPS Test Images**：通常用于公开测试与教学，优先用于 CI 回归与团队共享样本库。  
- **Digital Corpora Real Data Corpus**：属于真实二手设备数据，通常有研究资格/IRB 要求，默认不纳入常规开发回归。  
- **DFRWS 挑战集**：不同年份分发方式不同（官网、GitHub、Digital Corpora），拉取时请记录具体来源与哈希。  
- **版权与许可证**：下载后在内部样本仓保存原始 LICENSE/说明链接，避免样本流转时丢失合规信息。

## 4) 建议的首批样本包（MVP）

建议先凑一套“可快速回归”的最小样本包：

1. **DFTT**：NTFS Undelete、FAT Undelete、Basic Carving（至少 3 个）  
2. **CFReDS**：删除恢复/文件系统相关样本（至少 2 个）  
3. **Digital Corpora NPS**：`nps-2009-domexusers`（或同级别 1 套）  
4. **媒体样本**：Xiph 中 1 组视频 + 1 组音频，用于预览和修复链路冒烟

> 以上首批样本就能覆盖：扫描、树/列表渲染、勾选恢复、预览、恢复导出、失败明细等关键路径。

## 5) 入库规范（建议）

建议在本仓或外部样本盘维护如下结构（避免污染源码仓）：

```text
testdata/
  manifests/
    datasets.csv          # 来源、下载时间、版本、哈希、许可证、备注
  dftt/
  cfreds/
  digitalcorpora/
  dfrws/
  media/
```

`datasets.csv` 最少字段建议：

- `dataset_id`
- `source_url`
- `downloaded_at`
- `sha256`
- `size_bytes`
- `license_or_terms`
- `intended_tests`
- `owner`

## 5b) 仓内合成碎片样本（无需下载）

| dataset_id | 路径/生成方式 | 用途 |
|---|---|---|
| `SYNTH-FAT-STRUCTURED-FRAGMENT` | `tests/fixtures/fragment_fat_structured_fixture.*`（运行时临时镜像） | `FragmentFatStructured_QualityGate`：已删 jpg/png/pdf/zip，完整链路与断链重组 |
| `SYNTH-NTFS-STRUCTURED-FRAGMENT` | `tests/fixtures/fragment_ntfs_structured_fixture.*` | `FragmentNtfsStructured_QualityGate`：已删 JPEG，完整 data run 与 braid 断 run |

## 5c) `testdata/DFR`（NIST CFReDS 本地镜像树）

| 说明 | 路径 |
|------|------|
| 压缩包 | `testdata/DFR/**.dd.bz2`（可用 `testdata/tools/Expand-DfrBz2.ps1` 解压） |
| 已解压（与 `testdata/cfreds` 互补） | 同名 `dfr-XX-{fat,ntfs,xfat}.dd` |
| **Windows 批量验证** | `testdata/tools/Run-DfrWindowsValidation.ps1` |
| **最新报告** | `docs/reports/WINDOWS_TEST_REPORT_20260603.md` + `docs/reports/dfr_windows_recovery_20260603.csv` |

验证范围：FAT / NTFS / exFAT / braid（不含 ext/osx）。`dfr-02`～`dfr-17` 等空目录需自行放入对应 `.dd` 后再跑。

| ext4 冒烟 | `dfr-01-ext.dd`（`testdata/dfr-test-images`、`testdata/DFR` 或 `testdata/cfreds`） | CTest `CFReDS_EXT4_DFR01`；缺失时 SKIP |
| **Linux ext2/3/4 批量验证** | `testdata/dfr-test-images/**/dfr-*-ext.dd` | `Run-DfrLinuxValidation.ps1` / CTest `DFR_Linux_Batch` |
| **ext4 变体样本** | `testdata/ext4-variants/*-ext.dd` | `Generate-Ext4VariantImages.sh`；见 `docs/EXT4_VARIANTS.md` |

下载：`testdata/dfr-test-images/download-all.ps1`（NIST CFReDS 归档）；解压：`testdata/tools/Expand-DfrBz2.ps1`（同时处理 `testdata/DFR` 与 `dfr-test-images`）。

验证范围：Linux 仅 **ext2/3/4**（`*-ext.dd`，CFReDS 样本为 ext4）；不含 `*-osx.dd`。Windows 文件系统（FAT/NTFS/exFAT）见 5c 节。

模块说明见 [RECOVERY_MODULES.md](RECOVERY_MODULES.md)。Linux 构建与平台差异见 [PLATFORM_LINUX.md](PLATFORM_LINUX.md)。

## 6) 后续执行建议

- 每次版本发布前，固定跑一套“P0 + 部分 P1”回归；  
- 新增恢复能力（例如新格式修复器）时，把对应样本加入清单并登记哈希；  
- 对外宣称能力前，先在该清单中有可复现实验记录。

