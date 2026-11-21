# ext4 变体测试矩阵

## 目标

在 NIST CFReDS `*-ext.dd`（偏保守 ext4 配置）之外，补齐**现实环境常见**的 ext4 特性组合，并纳入 Linux 回归（`DFR_Linux_Batch`）。

## 支持矩阵（验收维度）

| 特性 | E4-A | E4-B | E4-C | E4-D | E4-E |
|------|:----:|:----:|:----:|:----:|:----:|
| extents | ✓ | ✓ | ✓ | ✗ | ✓ |
| has_journal | ✓ | ✓ | ✗ | ✓ | ✓ |
| 64bit | — | ✓ | — | — | — |
| metadata_csum | — | ✓ | — | — | — |
| dir_index | ✓ | ✓ | ✓ | ✓ | ✓ |
| 大目录压力 | — | — | — | — | ✓ |

## 生成与校验

```bash
bash testdata/tools/Generate-Ext4VariantImages.sh
bash testdata/tools/Verify-Ext4VariantFeatures.sh
```

- 生成脚本：`testdata/tools/Generate-Ext4VariantImages.sh`
- 特性校验：`testdata/tools/Verify-Ext4VariantFeatures.sh`（对照 `testdata/manifests/ext4_variants.csv`）
- 恢复回归：`DFR_Linux_Batch`（扫描 + 写出 + size match）

> **W4 资源状态核验（2026-11-08）**：上述两个生成/校验脚本、`testdata/manifests/ext4_variants.csv`、`testdata/ext4-variants/`、`testdata/DFR/` 与 `testdata/manifests/` **当前均不存在于仓库**，即本节描述的是**规划中的变体矩阵，尚未落地**。当前 ext4 删除恢复的实际证据只有 `CFReDS_EXT4_DFR01`、`DFR_Linux_Batch`（DFR 官方 ext 镜像）与 `DFTT_12_CARVE_EXT2`；ext2/3 无独立金标。落地变体矩阵前不得据本节宣称已覆盖 E4-A~E 特性组合。

## PASS 口径

与 CFReDS ext 批测一致：

1. 有 Deleted：恢复并校验**全部可恢复** Deleted（`Recoverable()` 过滤后）。
2. 无 Deleted（如仅 deep path）：恢复并校验**全部可恢复** Allocated。
3. 不崩溃；大目录样本（E4-E）应在合理时间内完成扫描。

## 与 CFReDS 的关系

| 来源 | 作用 |
|------|------|
| `testdata/dfr-test-images/**/**-ext.dd` | 删除恢复场景、官方布局 |
| `testdata/ext4-variants/*.dd` | ext4 **特性组合** 覆盖 |

两者均由 `DFR_Linux_Batch` 统一扫描，无需单独 CTest 目标。

## 后续（可选）

- E4-F：`uninit_bg` + `metadata_csum_seed` 强化组合
- `encrypt` / `verity`：列为风险项（能列出/导出原始数据即可）
- 将 `*.features.txt` 的 SHA256 写入 manifest，防止生成参数漂移
