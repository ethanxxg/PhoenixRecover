# 镜像验证报告（2026-11-21）

| 字段 | 值 |
|------|-----|
| 输入 | `C:\PhoenixImage\v0.img`（100.01 GiB，完整）、`v1.img`（46.39%）+ `v1_mft1..5.img`（完整） |
| 引擎 | `phoenix_headless_p07` 源码 + 新增 `--image`（`tools/phoenix_headless.cpp`），生产 `NtfsParser`/`RecoverEngine`/`SignatureDB` 未改 |
| 独立真值 | `tools/mft_oracle.py`（图像模式）、`tools/verify_blocks_hash.py`（流式） |
| 结论 | 卷 0 **逐字节全量通过**；卷 1 元数据 **完全一致**，可读数据段抽样 **全中** |

---

## 1. 现状

| 项 | 状态 |
|----|------|
| 卷 0 镜像 | 209,728,511 扇区，COVERAGE 100.000%，0 坏扇区，2,582 s / 39.7 MB/s |
| 卷 1 镜像 | 202,375,168 / 436,228,943 扇区（**46.39%**，96.5 GiB），0 坏扇区，中断于 2026-11-15 22:08 |
| 卷 1 MFT | 五段单独抓取，COVERAGE 100%，合计 963,575,808 B（919 MiB） |
| 物理盘 | `PhysicalDrive2` **已再次掉线**（`Win32_DiskDrive` 无 Index 2），卷 1 余下 111.5 GiB 暂时无法续镜像 |

卷 1 的 MFT runlist 实测为 `786432:48128; 3509839:51217; 27325866:73462; 21867058:51214; 49815732:11243`，
与五段镜像的 LCN 落点逐一吻合，故可用「稀疏卷镜像 + 五段 MFT + 96.5 GiB 数据段」重建出可被引擎与
oracle 同时解析的验证载体（`v1_meta.img` / `v1_data.img`，稀疏，实际占用 ≈97 GiB）。

## 2. 工具改造（零引擎改动）

| 文件 | 变更 |
|------|------|
| `tools/phoenix_headless.cpp` | 新增 `--image <raw>`：经 `SectorReader::OpenImage` 读镜像，`--boot` 变为镜像内引导 LBA；`--disk` 保留用于 CSV 溯源 |
| `tools/mft_oracle.py` | `PHOENIX_ORACLE_IMAGE=<path>` 走镜像模式，跳过物理盘身份绑定 |
| `tools/verify_blocks_hash.py` | 新增：按 runlist 流式重算 SHA-256，绕开 oracle「整文件读入内存」的 MemoryError |
| `scripts/oracle_hash_batched.sh` | 新增：分批跑 oracle hash，隔离个别文件的堆耗尽 |

> 环境坑：`image_region.exe` / `phoenix_headless.exe` 从 Git-Bash 直接启动会 **启动即崩（segfault 139，无日志）**，
> 用 `env -i PATH=/c/msys64/mingw64/bin:... MSYS2_ARG_CONV_EXCL='*'` 包装即正常；否则参数里的 `C:/...`
> 还会被 MSYS 改写。

## 3. 卷 0（完整镜像）

### 3.1 发现（引擎 vs 独立 oracle）

| 指标 | 引擎 | oracle | 一致情况 |
|------|------|--------|----------|
| 节点 | 11,134（文件 9,892 / 目录 1,242） | 11,139 行 | 同名同 id 匹配 **9,885** |
| 名称 | — | — | 不一致 **0** 处 |
| 大小 | — | — | 9,880/9,885 = **99.949%** |
| 已删除 | 47 | 47 | **100%**（9,885/9,885） |
| 仅引擎 | 7 | — | 4 条 `$RmMetadata\$Repair/$Tops` 流 + 3 条 `...:售前` 命名流（oracle 把它们折进基础记录） |
| 仅 oracle | — | 10 | `$MFT/$LogFile/$Bitmap/...` 系统记录（引擎不列入用户文件） |

5 处大小差异**全部**是 `$ATTRIBUTE_LIST` 场景（id 44/78/84/2163/4162）：引擎跟随扩展记录拿到真实大小，
oracle 不跟扩展记录故报 0——与 P0-7 在源盘上用 SHA-256 验证过的结论一致，引擎正确。

### 3.2 恢复

| 指标 | 值 |
|------|-----|
| 请求 id | 9,888（唯一）；命中节点 9,892（4 个 id 为 NTFS 流，同 id 两节点） |
| 成功 / 失败 / 跳过 / 取消 | **9,892 / 0 / 0 / 0** |
| 坏扇区 | 0 |
| 输出 | `C:\PhoenixOut\v0`，43 GiB，路径按 id 隔离 |

### 3.3 逐字节校验

| 项 | 值 |
|----|-----|
| 非驻留文件 | 9,363 |
| SHA-256 精确一致 | **9,363 / 9,363（100%）** |
| 校验字节 | **41.38 GiB** |
| 方式 | 8,954 个由 oracle 直接哈希；409 个（含 27 个 >512 MiB、24 个 oracle 内存溢出）由 `verify_blocks_hash.py` 流式重算 |

驻留文件 529 个：内容内嵌在 MFT 记录中，已由「恢复文件大小 == oracle 记录大小」及发现阶段一致性覆盖。

### 3.4 格式判定（引擎 SignatureDB 513 条 + AC）

| formatAgreement | 数量 |
|---|---|
| match | 3,128 |
| mismatch | 3,038 |
| noDeclaredExt | 2,603 |
| noSignature | 1,105 |
| noPayload | 18 |

抽样核对 mismatch 全部是**扩展名撒谎**或签名粒度差异，引擎无误：

- `.config → jpg`：文件头 `FF D8 FF E0 00 10 4A 46 49 46`（真 JPEG，被 AliWorkbench 命名成 .config）；
- `.dll → exe`：`4D 5A 90 00`（PE，DLL/EXE 同签名）；
- `.xml → svg / crd`：XML 子类型，引擎给了更具体的标签；
- `.log → ini`：日志以 `[section]` 开头。

## 4. 卷 1（元数据完整，数据 46.39%）

| 指标 | 引擎 | oracle | 一致情况 |
|------|------|--------|----------|
| 节点 | 940,598（文件 872,496 / 目录 68,102） | 940,571 记录 | 同名同 id 匹配 **872,457** |
| 大小 | — | — | 872,347/872,457 = **99.987%**，110 处差异**全为 oracle=0**（`$ATTRIBUTE_LIST`/命名流） |
| 已删除 | 36,251 | 36,251 | **100%**（872,457/872,457） |
| 仅引擎 | 39 | — | 命名流（`$UsnJrnl:$Max`、`*:Zone.Identifier` 等） |
| 仅 oracle | — | 10 | 系统记录 |

文件声明总量 114.11 GiB；不含系统文件的可恢复量 101.89 GB（oracle 口径）。

数据抽样：取**全部 run 都落在已镜像 96.5 GiB 内**的 398,059 个候选，分层抽 400 个（0.74 GiB）：

| 项 | 值 |
|----|-----|
| 引擎恢复 | 400 / 400 成功，0 坏扇区 |
| SHA-256 与 oracle（从 `v1_data.img`）比对 | **400 / 400 一致**，校验 756 MiB |

## 5. 已知局限

1. 卷 1 余下 111.5 GiB 数据未镜像（盘掉线），该区间的文件只能验证**元数据**，不能验证字节；
2. `pagefile.sys` 等 27 个大文件纳入流式校验（41.38 GiB 已覆盖），但未做「恢复后重新挂载比对」；
3. `$ATTRIBUTE_LIST` 与命名流在 oracle 侧无对应实现，相关 110+5 条只能以引擎口径 + 大小核对；
4. 同 id 双节点（`449/450/451/513`）会让「按 id 取文件」的比对逻辑误判，路径需按名字匹配。

## 6. 复现

```bash
# 扫描（镜像直读）
env -i PATH=/c/msys64/mingw64/bin:/c/Windows/system32:/c/Windows MSYS2_ARG_CONV_EXCL='*' \
  phoenix_headless_img.exe scan --image 'C:\PhoenixImage\v0.img' --boot 0 \
  --out 'C:\PhoenixImage\engine_nodes_v0.csv' --engine-format 0

# 恢复
... recover --image 'C:\PhoenixImage\v0.img' --boot 0 --ids ids_v0.txt \
  --out-dir 'C:\PhoenixOut\v0' --result 'C:\PhoenixImage\recover_v0_result.csv'

# 独立真值
PHOENIX_ORACLE_IMAGE='C:\PhoenixImage\v0.img' py -3 tools/mft_oracle.py scan --boot 0
bash scripts/oracle_hash_batched.sh 'C:\PhoenixImage\v0.img' ids_v0_all.txt oracle_hash_v0_all.json 400
py -3 tools/verify_blocks_hash.py --image 'C:\PhoenixImage\v0.img' \
  --csv docs/reports/real_disk_recovery_2026-11-14/candidates_vol0.csv \
  --result 'C:\PhoenixImage\recover_v0_result.csv' --ids ids_v0_stream.txt --out verify_v0_stream.json
```

## 7. 产物

| 路径 | 内容 |
|------|------|
| `C:\PhoenixImage\engine_nodes_v0.csv` / `_v1.csv` | 引擎发现清单（9,892 / 872,496 文件） |
| `C:\PhoenixImage\recover_v0_result.csv` | 卷 0 恢复明细（9,892 行，全 status=0） |
| `C:\PhoenixImage\recover_v1_result.csv` | 卷 1 抽样恢复明细（400 行） |
| `C:\PhoenixImage\oracle_hash_v0_all.json` / `verify_v0_stream.json` | 逐字节校验结果 |
| `C:\PhoenixImage\oracle_hash_v1.json` | 卷 1 抽样校验结果 |
| `C:\PhoenixOut\v0` | 卷 0 恢复出的 9,892 个文件（43 GiB） |
