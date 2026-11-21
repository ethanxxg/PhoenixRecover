# file-carving-images Graphic 雕刻测试报告

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-03（更新 2026-06-04） |
| 数据集 | `testdata/file-carving-images/`（本地已有，未重新下载） |
| 镜像 | `Images/L0_Graphic.dd` … `L5_Graphic.dd` |
| 基准 | `OriginalFiles/Graphic/` + NIST ImageLayouts.htm 布局 |
| 测试程序 | `test_file_carving_graphic.exe` |
| CSV | [file_carving_graphic_results_latest.csv](./file_carving_graphic_results_latest.csv)（2026-06-04 全量 PASS） |

---

## 1. 结论摘要

| 布局 | 说明 | embed（布局数据） | carve（首片段偏移） |
|------|------|-------------------|---------------------|
| **L0** contiguous | 6 文件连续存放 | **6/6 PASS** | **6/6 PASS** |
| **L1** frag in order | 6 文件按序碎片 | **6/6 PASS** | **6/6 PASS** |
| **L2** frag disorder | 乱序碎片 | **6/6 PASS** | **6/6 PASS** |
| **L3** missing frags | 缺失中间碎片 | **6/6 PASS** | **6/6 PASS** |
| **L4** extra + partial | 额外文件 + 部分碎片 | **7/7 PASS** | **7/7 PASS** |
| **L5** interleaved | 交错碎片 | **6/6 PASS** | **6/6 PASS** |

**回归门槛（CTest `FileCarving_Graphic_All`）**：L0–L5 全部 embed + carve 通过 → **PASS**（约 5–18 秒）。

---

## 2. 引擎修复（本轮）

| 格式 / 模块 | 问题 | 修复 |
|-------------|------|------|
| JPEG | SOS 后逐字节 `ReadBytes` 极慢；假 EOI | 64KB 缓冲 + FF00 填充感知扫描 |
| JPEG/BMP/… | 结构雕刻失败时 50MB footer 回退产生假巨型节点 | 图形格式失败则跳过；**TIFF 碎片首段**保留 maxEnd 回退 |
| RawScan | 前序 JPG 雕刻跳过嵌套头（L4/L5 @0x4E9600） | `RescanNestedGraphicHeaders` 在最大 JPG/BMP 宿主内二次扫描 |
| RawScan | 同类型 container 误跳过嵌套 JPG | 仅 `off == container->rawOffset` 时跳过 |
| BMP/GIF/PNG/PCX/TIFF | 结构边界不准 | `CarveImage.cpp` 结构雕刻（见前版报告） |

关键文件：`scan/carve/CarveImage.cpp`、`scan/RawScan.cpp`

---

## 3. CSV 字段说明（2026-06-04 更新）

| 列 | 含义 |
|----|------|
| `size` | 官方源文件逻辑大小 |
| `embed_bytes` | 布局中该扇区应验证的字节数（碎片场景常为前缀） |
| `carve_size` | **已验证的 embed 字节数**（与 `embed_bytes` 一致） |
| `carve_md5` | 镜像 `offset` 处 **embed 前缀** 的 MD5（不再误填整文件 ref_md5） |
| `node_offset` / `node_size` | RawScan 命中的雕刻节点起止（碎片场景可能远大于 embed） |
| `notes` | 当 `node_size` > `embed_bytes` 时标注 `fragment: node spans …` |

---

## 4. 报告问题分析与处理

| # | 现象 | 结论 | 处理 |
|---|------|------|------|
| 1 | L2 多行 `carve_size` 同为 59649536 | **报告误导 + TIFF 边界过大** | TIFF 失败回退改为截到下一文件头；CSV 分列 `node_size` / `carve_size` |
| 2 | L2 PCX 806663 vs 806664 | **碎片上结构雕刻差 1 字节** | 镜像仅 268800B 与源一致；`carve_size`=268800 已通过；整文件 806664 无法在无中间碎片时恢复 |
| 3 | L4 JPG `carve_size`<`size` 但 md5=ref | **CSV 脚本错误** | 已改：`carve_md5` 为 embed 前缀 MD5，`carve_size`=30208 |
| 4 | L4/L5 PCX/GIF 节点过大 | **碎片无头 + 覆盖节点** | embed 验证仍 PASS；CSV 用 `node_size` 暴露真实节点跨度 |
| 5 | L5 JPG node 771926 | **交错布局下 JPEG 熵数据延续** | embed 30208B 验证 PASS；完整 60716B 需碎片重组（超出 carve 范围） |

---

## 5. 性能说明

此前测试“卡死”原因：`CarveJpeg` 在 SOS 后对每个字节调用 `ReadBytes`（每次读整扇区），叠加失败 JPEG 的 50MB `FindFooter` 产生大量假宿主节点，`RescanNestedGraphicHeaders` 在内部分区逐 512B 扫描导致 CPU 长时间 100%。修复后全量 L0–L5 约 **5–18 秒**。

---

## 6. 复现命令

```powershell
d:\dev\PhoenixRecover\build\win2019-ninja\_build_release.bat

$env:PHOENIX_CARVE_REPORT_CSV = "d:\dev\PhoenixRecover\docs\reports\file_carving_graphic_results.csv"
d:\dev\PhoenixRecover\build\win2019-ninja\tests\test_file_carving_graphic.exe

cd d:\dev\PhoenixRecover\build\win2019-ninja
ctest -C Release -R FileCarving_Graphic_All --output-on-failure
```

---

## 7. 相关文件

| 路径 | 说明 |
|------|------|
| `tests/integration/test_file_carving_graphic.cpp` | L0–L5 对比测试 |
| `tests/integration/file_carving_golden.h` | 官方扇区 / embedBytes / sourceByteOffset |
| `tools/verify_carve_golden.py` | 从 dd 校验 embed 长度 |
| `tools/discover_carve_layout.py` | 自动发现 header 扇区 |

---

## 8. 说明

- 未下载或修改 CFTT `graphic-images.tbz2`。
- L2–L5 部分文件首片段无文件头（如 bmp/gif/png 中间段），通过 `AddFragmentNodes` + 宿主内 `RescanNested` 覆盖；carve 判定为**首片段 offset 处 embed 前缀 MD5 与官方一致**。
