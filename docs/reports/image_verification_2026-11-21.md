# 镜像验证报告（2026-11-21，最终版）

| 字段 | 值 |
|------|-----|
| 输入 | `C:\PhoenixImage\v0.img`（100.01 GiB，完整）、`v1.img`（**208.01 GiB，完整 100%**） |
| 引擎 | `phoenix_headless_flat` / `phoenix_headless_verify`（源码 `tools/phoenix_headless.cpp`），生产 `NtfsParser`/`RecoverEngine`/`SectorReader` 未改 |
| 判定方式 | **引擎逐字节**：生产 `SectorReader` 回读镜像 + 引擎 runlist 重建字节 + `memcmp`，无 Python/无 oracle 参与判定 |
| 独立交叉验证 | `tools/mft_oracle.py` + `tools/verify_all_image_recovery.py`（仅供旁证，不参与任何 verdict） |
| 最终结论 | 卷 0 **逐字节全量通过**；卷 1 完整镜像 **872,559 / 872,559 文件逐字节 100% 通过（114.073 GiB）** |

---

## 1. 镜像完整性

| 卷 | 扇区 | 容量 | COVERAGE | 坏扇区 | 跳过 | 源盘写入 |
|----|------|------|----------|--------|------|----------|
| v0.img | 209,728,511 | 100.01 GiB | 100.000% | 0 | 0 | 0 |
| **v1.img** | **436,228,943** | **208.01 GiB** | **100.000%** | **0** | **0** | **0** |

- 盘回插后确认为 `PhysicalDrive2` / ST1000DM003（SN `0AD132601218`），LBA0 `33 C0 8E D0` / `55 AA`，卷 1 boot LBA 209,728,638。
- `v1.img` 从 `nextLba=412,103,806` 续传 11 分钟完成；前 96 GiB 与旧 `v1_data.img` 抽 200 个 1 MiB 块逐字节一致。
- 环境坑：`image_region.exe` / `phoenix_headless*.exe` 从 Git-Bash 直接启动会 segfault，必须用
  `env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows MSYS2_ARG_CONV_EXCL='*'` 包装。

## 2. 卷 1 发现（完整镜像，引擎 vs 独立 oracle）

| 指标 | 引擎 | oracle | 一致情况 |
|------|------|--------|----------|
| 节点 | **940,662** | 940,635 记录 | 比稀疏镜像多发现 64 个节点，其余 94 万条 name/size/runs 零差异 |
| 大小差异 | — | — | 123 处全为 oracle=0（`$ATTRIBUTE_LIST` / 命名流），引擎正确 |
| 已删除 | 36,251 | 36,251 | 100% 一致 |

## 3. 卷 1 全量恢复 + 引擎逐字节校验（本次核心）

### 3.1 流水线

```
完整 v1.img (208 GiB)
  → 引擎扫描：940,662 节点
  → 872,548 个 id，24 分片并行恢复（flat 输出 f<seq>_<id>_<name>，无预读）
  → recover_v1_full_result.csv：872,561 条记录
  → 24 分片并行引擎校验（phoenix_headless verify）
  → engine_verify_v1_full.csv + 统计汇总
```

- 恢复性能：稳态 ~12,900 文件/min，瓶颈是 87 万小文件写盘（非扫描/读镜像）。
  关键优化：① 每文件一个 id 目录 ≈80ms/文件 → flat 平铺；② 64 MiB 预读在随机小文件上反慢 3 倍 → 关闭。
  预读不改变字节（100 文件 flat 有/无预读 SHA-256 全一致）。

### 3.2 恢复结果

| 项 | 值 |
|----|-----|
| 请求 id | 872,548 |
| 成功（status=0） | **872,559**（含同 id 双节点 NTFS 流） |
| status=3 Directory/noPayload | 1（id 529，目录，无载荷） |
| status=2 No recoverable data | 1（id 269279，删除残留无簇无驻留，引擎正确判定） |
| 坏扇区 | 0 |

### 3.3 引擎逐字节校验（FINAL）

| 项 | 值 |
|----|-----|
| 校验方式 | 生产 `SectorReader::OpenImage` + 同一 `EnumerateNodes` 发现路径 + 引擎 runlist 回读镜像 + 逐字节 `memcmp` |
| 判定来源 | `phoenix_headless verify`；**无 Python / 无 oracle CSV / 无外部候选源** |
| match | **872,559 / 872,559（100%）** |
| mismatch / error / skip | **0 / 0 / 0** |
| 校验字节 | **122,485,299,985 B（114.073 GiB）** |
| 全链退出码 | 0（统计门禁 `tools/summarize_v1_verify.py` all_match=True → exit 0） |

### 3.4 校验工具可信度（正/负向证据）

| 场景 | 结果 |
|------|------|
| 正向（4 个真实文件，0/26/442/2158 B，含驻留+非驻留） | `match=4 mismatch=0 error=0`，flat 路径 `f<seq>_<id>_<name>` 解析正确 |
| 负向（篡改 `index_gen.hpp` 第 10 字节） | 该文件 `mismatch` + `firstDiffOffset=10`，其余 3 个仍 match；验证后还原 |
| 结论 | 校验非假阳性，且能精确定位差异偏移 |

## 4. 卷 0（完整镜像，历史结论）

- 发现：引擎 11,134 节点 vs oracle 11,139，同名同 id 匹配 9,885；5 处大小差异全为 `$ATTRIBUTE_LIST`，引擎正确。
- 恢复：9,892 / 9,892 成功，0 坏扇区，输出 43 GiB。
- 校验：非驻留文件 9,363 / 9,363 SHA-256 精确一致（41.38 GiB）；驻留文件 529 个由大小一致性覆盖。
- 格式判定（SignatureDB 513 条 + AC）：match 3,128 / mismatch 3,038 / noDeclaredExt 2,603 / noSignature 1,105 / noPayload 18；
  mismatch 抽样全部为扩展名撒谎或签名粒度差异，引擎无误。

## 5. 已知局限

1. 校验判定由引擎口径给出；独立 oracle 仅作旁证，`$ATTRIBUTE_LIST` 与命名流在 oracle 侧无对应实现；
2. 未做「恢复后重新挂载文件系统」比对（逐字节一致已足够覆盖数据正确性）；
3. 同 id 双节点（`449/450/451/513`）需按名字匹配，verify 已按 flat leaf 与引擎节点 SafeLeaf(name) 对齐；
4. 全量校验覆盖的是 status=0 的成功恢复项；2 条非成功项（目录 / 删除残留）按引擎判定跳过，非缺陷。

## 6. 复现

```bash
# 扫描（镜像直读）
env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows MSYS2_ARG_CONV_EXCL='*' \
  phoenix_headless_flat.exe scan --image 'C:\PhoenixImage\v1.img' --boot 0 \
  --out 'C:\PhoenixImage\engine_nodes_v1_full.csv' --engine-format 0

# 全量恢复（24 分片，脚本内部对每片 env -i 包装）
bash scripts/recover_v1_full_parallel.sh

# 引擎逐字节校验（24 分片）+ 统计门禁
bash scripts/finish_v1_pipeline.sh   # 守望恢复完成后自动拉起 verify
py -3 tools/summarize_v1_verify.py \
  --glob 'C:/PhoenixImage/engine_verify_v1_full_p*.csv' \
  --out-json 'C:/PhoenixImage/engine_verify_v1_summary.json'
```

## 7. 产物

| 路径 | 内容 |
|------|------|
| `C:\PhoenixImage\v1.img` | 卷 1 完整镜像（208.01 GiB，100%，0 坏扇区） |
| `C:\PhoenixImage\engine_nodes_v1_full.csv` | 引擎发现清单（完整镜像 940,662 节点） |
| `C:\PhoenixImage\recover_v1_full_result.csv` | 卷 1 全量恢复明细（872,561 条） |
| `C:\PhoenixImage\engine_verify_v1_full.csv` | 引擎逐字节校验明细（872,559 行，全 match） |
| `C:\PhoenixImage\engine_verify_v1_summary.json` | 校验统计（all_match=True，114.073 GiB） |
| `C:\PhoenixImage\pipeline_finish.log` | 全链日志（恢复→校验→汇总） |
| `D:\PhoenixOut\v1_full\p00..p23` | 恢复出的 872,559 个文件（扁平输出） |
| `C:\PhoenixOut\v0` | 卷 0 恢复出的 9,892 个文件（43 GiB） |
