# custom-carving-images 雕刻测试报告

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-05 |
| 数据集 | `testdata/custom-carving-images/` |
| 镜像 | `carving-00.dd` … `carving-19.dd`（20 张） |
| 基准 | `manifest.json`（1100 个文件偏移/大小） |
| 布局 CSV | [custom_carving_layout_results.csv](./custom_carving_layout_results.csv) |
| 雕刻 CSV | [custom_carving_results.csv](./custom_carving_results.csv) |

---

## 1. 结论摘要

| 阶段 | 工具 | 结果 |
|------|------|------|
| **布局 / embed** | `Validate-CustomCarvingManifest.py` | **1100/1100 PASS** |
| **镜像 smoke** | 扇区 0 可读 | **20/20 PASS** |
| **金标 + RawScan** | CTest `CustomCarving_ManifestGolden` | **PASS**（carving-03，5 条目 + found=430） |
| **全量 RawScan 批测** | `test_custom_carving_batch.exe` + `Run-CustomCarvingBatch.ps1` | **20/20 完成**（~75 s 全量） |

**embed 门槛**：manifest 中每个 `(offset, size)` 均可从 `.dd` 读出且未越界。  
**carve 指标**：RawScan 节点是否覆盖 manifest 偏移（非全部文件类型均有签名，carve 率为参考值，不作为硬 FAIL）。

---

## 2. 全量批测（20/20 镜像完成）

| 指标 | 结果 |
|------|------|
| 镜像 | **20/20** 全部完成 |
| embed | **1100/1100 PASS** |
| carve 命中 | **1059/1100（96.3%）** |
| 总耗时 | ~75 s（修复后；此前 carving-10 单张 ~19 min 挂起） |

| 镜像 | embed | carve 命中 | 备注 |
|------|-------|------------|------|
| carving-00 … 19 | 各 55/55 | 48–55/55 | 含 carving-10（53/55） |

carve 未命中多为无 magic 的 txt/json/css/ics，或个别 GIF/SVG 节点边界与 manifest 不完全一致。

---

## 3. 引擎问题与修复（本轮）

| 问题 | 现象 | 修复 |
|------|------|------|
| **镜像尾扇区** | 非 512 对齐的 `.dd` 末尾文件 embed 失败；`BAD SECTOR` 日志 | `SectorReader::OpenImage` 记录 `m_imageFileSize`；同步读支持末扇区部分字节；`GetTotalBytes()` 返回精确文件长度 |
| **RawScan 扫描上界** | 分区 `sizeBytes` 用扇区数×512，丢失尾部 | `Run()` 优先 `partition.sizeBytes` / `GetTotalBytes()`；扫描扇区数按精确字节数计算 |
| **RawScan 性能（carving-10）** | ~80 MB 镜像单张 ~19 min 挂起 | `FindNextStrongCarveHeader` 等步进搜索加步数上限；弱文本签名无边界时跳过而非创建 50 MB 节点；`RescanNestedGraphicHeaders` 宿主内扫描上限 16 MB |
| **无签名类型** | txt/json/css 等 carve 未命中 | 预期行为（无 magic）；不作为 FAIL |
| **GIF/SVG 部分未命中** | 结构雕刻失败或节点偏移/大小与 manifest 不完全一致 | 记录于 CSV；后续可加强 `CarveGif`/SVG 头变体 |

关键文件：`disk/SectorReader_win.cpp`、`disk/SectorReader.h`、`scan/RawScan.cpp`

---

## 4. 说明

- 镜像末尾文件可能超出「扇区对齐」长度但仍在文件物理长度内；批测使用 `file_size` 作为 embed 边界（与 Python 布局验证一致）。
- `carving-10.dd` 含 ~36 MB RTF 等大文件；修复前无 footer 签名误匹配会触发 50 MB 全量步进搜索导致挂起，修复后 ~12 s/张。
- **不会删除或修改** `testdata/custom-carving-images/` 下任何源文件。

---

## 5. 复现命令

```powershell
# 布局验证（~10s）
python testdata\tools\Validate-CustomCarvingManifest.py --repo-root d:\dev\PhoenixRecover `
  --csv docs\reports\custom_carving_layout_results.csv

# CTest 金标（carving-03）
d:\dev\PhoenixRecover\build\win2019-ninja\tests\test_custom_carving_manifest.exe

# 全量 RawScan 批测（20 张，~75 s）
powershell -File scripts\Run-CustomCarvingBatch.ps1 -From 0 -To 19
python scripts\Merge-CustomCarvingCsv.py --repo-root d:\dev\PhoenixRecover
```

---

## 6. 相关 CTest

| 用例 | 说明 |
|------|------|
| `CustomCarving_ManifestLayout` | Python 布局 1100 项 |
| `CustomCarving_ManifestGolden` | carving-03 金标 + RawScan smoke |
| `CustomCarving_Batch_All` | 批测（默认 `PHOENIX_CUSTOM_CARVE_MAX_IMAGES=1` 快速门禁） |
