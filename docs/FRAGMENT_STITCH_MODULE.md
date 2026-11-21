# 碎片重组模块（低耦合集成）

## 分层

| 层 | 文件 | 职责 |
|----|------|------|
| 策略 | `recover/FragmentStitchPolicy.h` | 开关 `enabled`，扫描/恢复共用 |
| 门面 | `recover/FragmentStitchService.h` | **唯一对外集成点** |
| 实现 | `recover/FatFragmentStitcher.cpp` | FAT 簇链搜索 + `FormatPayloadScore` |
| 实现 | `recover/NtfsFragmentStitcher.cpp` | NTFS LCN 重排 + `$Bitmap` 空闲簇搜索 |
| 评分 | `recover/FormatPayloadScore.cpp` | 容器结构打分（无黄金 MD5） |

## 如何接入

```cpp
#include "recover/FragmentStitchService.h"

FragmentStitchPolicy policy;
policy.enabled = true;

// FAT 元数据建链后
FragmentStitchService::TryImproveFatClusterChain(
    reader, geo, &fatTable, startCluster, fileSize, ext, absSectorChain, policy);

// 恢复写出前
FragmentStitchService::TryImproveForRecovery(reader, partition, node, policy);

// 打开 FS 解析器后（当前仅 FatParser）
FragmentStitchService::ApplyPolicyToFileSystem(*fs, policy);
```

## 产品路径

- **扫描**：`UnifiedScanConfig::fragmentStitch` → `VolumeSectorScanner` → `FatParser`
- **恢复**：`RecoverOptions::formatAwareFatFragments` → `SetOptions` 同步到 `fragmentStitch` → `RecoverEngine`
- **UI**：`QSettings` 键 `recover/formatAwareFragments`（扫描 Worker 与恢复页共用）

## NTFS

- `recover/NtfsFragmentStitcher.cpp`：`TryImproveNode` 已接入 `FragmentStitchService::TryImproveForRecovery`（按 `fsType` 含 `NTFS` 分发）。
- 实现：`$Bitmap` 判定空闲 LCN + `FormatPayloadScore` 搜索（与 FAT 同预算/窗口）；跳过 NTFS 压缩与 resident。
- 扩展名缺失/截断时按首簇魔数 `ProbeExtension`；MFT 仅 1 个 data run 时若相邻空闲 LCN 已能 `Complete`，会写回簇链（braid 夹具路径）。
- CTest：`FragmentNtfsStructured_QualityGate`（合成卷，无需下载）。
- CTest：`CFReDS_BraidNtfs_FragmentStitch`（需 `dfr-05-braid-ntfs.dd`；对比 stitch 开/关得分）。
- CTest：`NtfsFragmentStitch_Dispatch` 验证分发不崩溃。

## 扩展 exFAT（W4 校正现状）

**已落地**：独立的 `ExfatFragmentStitcher` 仍不存在，但 exFAT 已由 `FragmentStitchService::TryImproveForRecovery` 在 `fsType` 含 `exFAT` 时复用 `FatFragmentStitcher::TryImproveExfatNode` —— 先读 exFAT boot 构造 `FatVolumeGeometry`，再走 FAT 的簇链打分。因此旧表述「exFAT 无碎片拼接」不准确。

**仍待办**：

1. 为 exFAT 补独立质量门禁 CTest（现有 `FragmentFatStructured_QualityGate` / `FragmentNtfsStructured_QualityGate` 无 exFAT 对应项）
2. 评估 `noFatChain` 连续簇卷的专用打分（当前复用 FAT 外推）
3. 若打分策略分化明显，再拆独立 `ExfatFragmentStitcher`；调用方 `RecoverEngine` / `FatParser` 无需改动

## Graph 碎片拼接（`GraphCarve`）

| 策略 | 枚举 | 场景 |
|------|------|------|
| 贪心（默认） | `FragmentStitchStrategy::HeuristicGreedy` | FAT/NTFS 簇链 |
| 图搜索 | `FragmentStitchStrategy::GraphCarve` | `FileNode::rawFragments` ≥ 2（雕刻多 span） |

实现：`recover/GraphCarveFragmentStitcher.cpp`

- 块 = 磁盘 `[diskOffset, size)`；边 = 尾+头可拼接且 `FormatPayloadScore` 非 Invalid
- 贪心构造路径 + 小块全排列（n≤8）；预算默认 50ms
- 门面：`FragmentStitchService::TryGraphStitchCarveBlocks` / `TryImproveForRecovery`（RAW 多 span）
- 恢复：`RecoverEngine` 按 `rawFragments` 顺序写出

```cpp
FragmentStitchPolicy policy;
policy.strategy = FragmentStitchStrategy::GraphCarve;
FragmentStitchService::TryGraphStitchCarveBlocks(reader, part, L"jpg", blocks, node, policy);
```

CTest：`GraphCarveFragmentStitch`（乱序 JPEG 三 span → Complete/Partial）。

**阶段 3 后处理**：`scan/CarveGraphMergeService` — 同扩展名、间距 ≤16MiB 的 RAW 命中聚类 → Graph 合并为 `RECOV_GRAPH_*`（`graphMergeCarvedSpans` 默认开，见 `UnifiedVolumeScanner::RunPhase3`）。CTest：`CarveGraphMerge`。

**不含 ML。**
