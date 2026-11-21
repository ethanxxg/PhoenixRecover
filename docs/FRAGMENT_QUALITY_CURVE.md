# 碎片重组质量曲线（Windows）

模块集成说明见 [FRAGMENT_STITCH_MODULE.md](./FRAGMENT_STITCH_MODULE.md)。

本项目用 **FormatPayloadScore** 对“导出的恢复结果”做结构一致性打分，并将结果输出为 CSV，作为碎片重组能力的 **可量化基线**（可回归、可对比）。

## 测试入口

- **CTest 名称**: `CFReDS_DFR05_QualityCurve`（从 CFReDS DFR-05 / braid 镜像导出并打分）
- **CTest 名称**: `MediaFormatQualityBaseline`（对 `testdata/media` 中 jpg/mp4 做 Complete/Partial 门槛校验）
- **CTest 名称**: `FatFragmentStitchSynthetic`（合成 FAT16 卷：断链 JPEG 重组为 `Complete`）
- **CTest 名称**: `FragmentFatStructured_QualityGate`（合成 FAT16 已删 **jpg/png/pdf/zip**：扫描→恢复→`Complete`；每种格式含完整链路与断链+诱饵簇）
- **CTest 名称**: `FragmentNtfsStructured_QualityGate`（合成 NTFS 已删 JPEG：完整 data run / braid 断 run + `$Bitmap`）
- **CTest 名称**: `NtfsFragmentStitch_Dispatch`（NTFS 经 `FragmentStitchService` 分发；测试用**默认构造、未打开**的 `SectorReader`/`Partition`，`LoadGeometry` 失败故安全返回 false，验证的是「不崩溃」）；**真实分发已接入 `NtfsFragmentStitcher::TryImproveNode`（`FragmentStitchService.cpp` 的 NTFS 分支），并非占位实现** —— 旧版「占位实现」表述已由 W4 校正
- **可选保留输出**: 设置环境变量 `PHOENIX_KEEP_TEST_OUTPUT=1`，测试会打印 `KEEP: <csv路径>` 并保留 CSV。
- **汇总 CSV**: `powershell -File testdata/tools/Summarize-QualityCurve.ps1 -CsvPath <path>`
- **卡住监视**: `powershell -File scripts/Watch-StuckTests.ps1`（或在 `Regress.ps1` 加 `-WatchStuck`）

CSV 字段：

- `dataset_id`: 数据集标识（例如 `CFREDS-DFR-05-BRAID-FAT`）
- `fs`: 文件系统类型（例如 `FAT16`/`FAT32`/`NTFS`）
- `ext`: 文件扩展名（小写；来自元数据或 magic probe）
- `name`: 文件名
- `size_bytes`: 期望写出大小（用来做“写出完整性”门槛）
- `score`: FormatPayloadScore 的标签

## 评分口径（FormatPayloadScore）

评分发生在 **写出成功且写出大小等于元数据大小** 之后，对导出文件的头部数据（默认最多读取 256 KiB）进行结构检查：

- **Complete**: 结构完整（例如 PNG 见到 IEND、PDF 见到 %%EOF、ZIP 见到 EOCD 等）
- **Partial**: 结构合理但不完整（例如只有头/部分结构）
- **Weak**: 无法判断或数据不足/未知格式
- **Invalid**: 明确不符合声明格式（magic/结构矛盾）

备注：

- `txt` 等纯文本通常会得到 `Weak`（这是预期，不代表恢复失败），因此在看“碎片重组能力”时更关注 `jpg/png/pdf/zip/mp4` 等结构化格式。

## 如何解读（质量曲线）

建议把 CSV 按 `(dataset_id, ext)` 聚合，计算：

- `ok_count`: 导出成功且大小匹配的数量（基础“能写出”门槛）
- `complete_ratio`: Complete 占比（“可用率”主指标）
- `invalid_ratio`: Invalid 占比（误拼接/误判风险）

对比方式：

- **同一数据集**：对比不同提交/不同策略下 `complete_ratio` 的提升
- **普通碎片 vs braid**：对比 `CFREDS-DFR-05-*` 与 `CFREDS-DFR-05-BRAID-*` 的差距

## 当前现状（已知限制）

当前 DFR-05 数据集中抽样到的删除文件以 `txt` 为主，CSV 中会出现较多 `Weak`。结构化格式的 **硬门槛** 由 `FragmentFatStructured_QualityGate` 承担（不依赖外部样本下载）；外部镜像（DFReDS/DFTT）仍用于真实场景回归。

### 结构化格式门禁（P0）

| CTest | 要求 |
|---|---|
| `FragmentFatStructured_QualityGate` | jpg/png/pdf/zip ×（完整链 / 断链+诱饵）共 8 场景，恢复后均为 **Complete** |
| `FragmentNtfsStructured_QualityGate` | NTFS jpg ×（完整 run / braid 断 run），恢复后均为 **Complete** |
| `MediaFormatQualityBaseline` | `testdata/media` 中 jpg/mp4 达到 Partial+ |
| `CFReDS_DFR05_QualityCurve` | 有样本时导出并写 CSV；不以 Complete% 作为硬失败（数据集以 txt 为主） |

