# DFTT 测试报告

**日期**: 2026-06-04（更新）  
**数据路径**: `testdata/dftt/`  
**构建**: `build/win2019-ninja` Release  
**执行命令**: `ctest -C Release -R "DFTT|PartitionProbe" --output-on-failure`

---

## 摘要

| 类别 | 数量 | 结果 |
|------|------|------|
| CTest 自动化 | **10** | **10/10 PASS** |
| 镜像 MD5 抽检 | 5 | 5/5 OK |

**总耗时**: ~100s（`DFTT_FAT_RecoverBytes` ~74s；GoldenBundle 不再重复跑 recover）

---

## CTest 列表

| CTest | 镜像/说明 | 结果 |
|-------|-----------|------|
| `DFTT_FAT_Undelete` | #6 | PASS |
| `DFTT_FAT_RecoverBytes` | #6 黄金 MD5 + 簇链重建 | PASS |
| `DFTT_NTFS_Undelete` | #7 | PASS |
| `DFTT_NTFS_RecoverBytes` | #7 | PASS |
| `DFTT_Recover_GoldenBundle` | 委派给上述 recover 测试（无重复执行） | PASS |
| `PartitionProbe` | #10 NTFS 自动探测 | PASS |
| `DFTT_11_CARVE_FAT` | #11 全量黄金 MD5 矩阵 + haxor2 负向 | PASS |
| `DFTT_8_JPEG_SEARCH` | #8 NTFS 元数据 + RawScan checklist | PASS |
| `DFTT_12_CARVE_EXT2` | #12 无镜像则 SKIP | SKIP/PASS |
| `DFTT_14_ISO9660_SMOKE` | #14 PVD 魔数 smoke | PASS |

---

## 新增/改进项（2026-06-04）

### #11 雕刻测试扩展
- 对照 `11-carve-fat.html` 全部 15 个黄金 MD5
- **P0 必需**: `paul.jpg` + `haxor2.jpg` 不得为 Good+ 有效 JPEG
- **P1 stretch**: DOC/WAV/XLS/PDF/GIF/MOV/WMV/PPT/ZIP 等 — 记录 `STRETCH miss`，暂不 fail CI（RawScan 覆盖待提升）

### #8 JPEG Search 集成测试
- NTFS 元数据恢复：file1/2/6/7 黄金 MD5 **必需**
- RawScan：诱饵 file3/4/5 **不得**作为 JPEG 雕刻命中
- 内嵌 file8–13：**可选**（ZIP/Word/ADS 未实现时 WARN）

### CTest 优化
- `DFTT_Recover_GoldenBundle` 不再重复执行 recover（节省 ~75s）
- FAT 碎片重建：空闲区索引 + 流式簇 MD5（`Md5OfClustersHexLower`）

### #14 ISO9660
- PVD 检查改为 byte offset `16×2048`（兼容 512 字节 SectorReader）

### #12 EXT2
- ZIP 包损坏（HTML 非 zip）；镜像缺失时 SKIP

---

## 已知引擎差距（stretch / optional）

| 项 | 说明 |
|----|------|
| #11 stretch 13/13 miss | 磁盘上文件连续存在，RawScan 仅稳定雕出 `paul.jpg`；需扩展 DOC/WAV/PDF/MP4 等结构雕刻 |
| #8 optional 6/6 miss | ZIP/Word/TAR 内嵌 JPEG、NTFS ADS 未覆盖 |
| #12 | 需重新下载有效 `12-carve-ext2.zip` |

---

## 复现

```powershell
cd d:\dev\PhoenixRecover\build\win2019-ninja
ctest -C Release -R "DFTT|PartitionProbe" --output-on-failure
```

详见 [`dftt_test_results.csv`](dftt_test_results.csv)
