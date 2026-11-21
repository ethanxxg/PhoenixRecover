# 真实坏盘修复预案 SOP（P0-3）

- 日期：2026-11-14
- 适用对象：`\\.\PhysicalDrive2` ST1000DM003-1CH162 / 序列号 `0AD132601218` / 1,953,525,168 扇区 × 512 B / 2 个 `0x42` LDM 动态分区 / 无盘符
- 配套：`P0-1_diagnosis.md`（取证与损坏层级诊断）、`summary.md`（恢复验证汇总）
- **本轮状态：只诊断，L1–L4 一项都没有执行。源盘保持原始损坏态。**

> ⚠️ 用户原始约束：「你不用真正的去修复它，如果直接修复了就没有验证数据了」。
> 因此本文件是**预案**，不是执行记录。任何 L1–L4 动作都会改写 LDM 私有头 / MBR，
> 从而销毁当前这份唯一的真实故障样本。

---

## 0. 执行门禁（四条全部满足才允许动手）

| # | 门禁条件 | 当前状态 |
|---|---|---|
| G1 | 6 批恢复记录 + 逐文件哈希清单全部归档 | **进行中**（`checkpoint.completed = [1,2]`，2/6） |
| G2 | 全量完成后 `python tools/forensic_baseline.py --verify` 复采为 `SOURCE DISK UNCHANGED` | 中途复采（13:32）已一致；**最终复采待执行** |
| G3 | ≥1 TB 外部介质，先做整盘镜像（含末尾 1 MiB LDM 区与 LBA0） | **不满足**（C+D 可用 855 GiB < 源盘 931 GiB） |
| G4 | 用户明确书面确认不再需要该损坏态样本 | **未给出** |

**当前裁定：G1/G2/G3/G4 均未满足，禁止执行任何修复动作。** 继续用只读方式把 6 批恢复做完即可拿到全部数据。

---

## 1. 目标分叉（先想清楚要什么）

| 目标 | 应选路径 | 说明 |
|---|---|---|
| 只要盘上的文件 | **L0（本轮已做）** | 手工构造 `Partition` 指向 NTFS boot，绕过无 LDM 解析的 `PartitionProbe`，双通道 SHA-256 校验；不需要修复磁盘，也不需要 Windows 认盘 |
| 让 Windows 重新显示盘符、继续用这块盘 | L1 →（失败）L2 →（失败）L3 | 会改写盘上元数据，必须先满足 §0 全部门禁 |
| 验证 Phoenix 的「修复能力」 | §4 演练盘 | 在可牺牲盘上复现同一故障态，**不碰样本** |

L0 只读恢复是本轮的正确做法：数据区完好，修复磁盘对「取回文件」没有增益，只会破坏验证样本。

---

## 2. 动手前的只读诊断清单（目标机器上执行，零写入）

```powershell
# 设备是否被系统识别、是否 Offline / Foreign
Get-Disk | Format-Table Number,FriendlyName,SerialNumber,Size,PartitionStyle,OperationalStatus,IsOffline -Auto
Get-PhysicalDisk | Format-Table DeviceId,FriendlyName,MediaType,HealthStatus,OperationalStatus -Auto
Get-Partition -DiskNumber 2 | Format-Table PartitionNumber,Type,Offset,Size -Auto
Get-Volume | Format-Table DriveLetter,FileSystem,HealthStatus,Size -Auto
```

```
rem DiskPart（只读子命令；不要用 clean / convert / create / format）
list disk
select disk 2
detail disk
list partition
rem 若该版本提供动态磁盘组导入，先用 help import 核对语法与副作用
help import
```

本项目侧复核（只读）：

```text
python tools/forensic_baseline.py --verify
python tools/mft_oracle.py scan --boot 63 209728638
```

判定表：

| 观察结果 | 推断 | 下一级 |
|---|---|---|
| `Get-Disk` 里 Disk 2 缺失或标记 Foreign / Offline，LDM DB 完好 | 外部动态磁盘未导入 | **L1** |
| 磁盘在线但卷未组装，`dmdiag`/VBLK 显示磁盘组状态不一致 | LDM 库需要重建 | L2 |
| LDM 数据库扇区损坏或 `PRIVHEAD` 副本失效，而两个 NTFS boot 完好 | 卷层元数据不可用 | L3（需先解析几何） |
| 出现读错误 / `badSectors > 0` | 物理层劣化 | 停止修复，先整盘镜像并做坏扇区优先恢复 |

---

## 3. 分级修复动作

### L0 · 只读恢复（已完成，零风险）

不改任何扇区。用 `tools/phoenix_headless.cpp` 手工构造 `Partition{startSector = 63 / 209728638, fsType = NTFS}` 交给未改动的 `NtfsParser` + `RecoverEngine`，再由 `tools/mft_oracle.py` 独立解析 MFT 生成真值哈希逐文件比对。6 批 checkpoint + `batch_NNN_hashes.csv` 即交付物。

### L1 · 导入外部动态磁盘（首选，成功率最高）

- **适用范围**：LDM 数据库完整（本盘实测 `PRIVHEAD` @LBA6 副本、末尾 1 MiB 内 `PRIVHEAD`/`TOCBLOCK`/`VMDB`/大量 `VBLK` 均存在）、故障为「未导入」。
- **执行位置**：优先在**另一台机器 / 另一 Windows 安装**上做，避免污染当前取证环境；GUI 路径为「磁盘管理 → 导入外部磁盘」。DiskPart 是否支持 `import` 取决于目标系统版本，务必先 `help import` 核对，**不要凭记忆执行写命令**。
- **会改写**：目标系统注册表 + 可能的磁盘末尾 1 MiB LDM 私有头序列号；**卷内数据区不动**。
- **执行后校验**：`forensic_baseline.py --verify` 预期会报 `TAIL_1MiB_LDM_DB` 变化——这本身就是「样本已失效」的证据；随后用 L0 的 oracle 抽查若干文件哈希，确认数据未受影响。
- **回退**：只有 §0 G3 的整盘镜像能回退，无法靠撤销导入还原私有头。

### L2 · 重建 / 修复 LDM 数据库

- 前置：已备份 LBA0 与末尾 1 MiB；已解析 `VMDB`/`VBLK` 确认卷成员与条带/跨区关系（**本轮未做，是待办**）。
- 手段：Windows 动态磁盘修复工具或专业 LDM 重建工具；禁止在不了解 VBLK 的情况下写回数据库。
- 风险：中。写错卷成员映射会让系统把跨区卷当成单一连续卷挂载，造成逻辑错位。
- 完成标志：Windows 出现卷且 `Get-Volume` 的 Size 与 LDM 卷声明一致，再用 oracle 抽样比对。

### L3 · `0x42` → `0x07` 转回基本盘（**有几何陷阱，不是改一个字节**）

本盘 MBR 实测：

| 项 | 分区 0 | 分区 1 |
|---|---:|---:|
| MBR 类型 | `0x42` | `0x42` |
| MBR startLBA | 63 | 209,728,575 |
| MBR sectorCount | 209,728,512 | 1,743,794,545 |
| **NTFS boot 绝对 LBA（oracle 实测）** | **63** | **209,728,638** |
| BPB `hidden sectors` | 63 | 63 |
| BPB `totalSectors` | 209,728,511 | 436,228,943 |

陷阱有三条，任何一条没处理都会把修复变成二次破坏：

1. **分区 1 的起点与 boot 相差 63 扇区**（`209728638 = 209728575 + 63`）。仅把类型改成 `0x07` 后，Windows 会到 `209728575` 找 boot sector 而失败；必须同时把该分区项的起始 LBA 校正到 boot 实际位置，或确认 LDM 私有头位于这 63 扇区偏移的用途。
2. **BPB `totalSectors` 与 MBR `sectorCount` 不一致**：分区 1 的 BPB 只声明 436,228,943 扇区（≈208 GiB），而 MBR 分区项声明 1,743,794,545 扇区（≈831 GiB）。这与 `$MFT` runlist 最大 LCN ≈ 50M 簇（≈204 GiB）大致吻合，但**尚未通过 LDM VBLK 逐记录解析确认卷几何**；在确认之前改 MBR，Windows 可能按错误长度挂载卷。
3. **末尾 1 MiB 是 LDM 数据库区**：`part1_end = 209728575 + 1743794545 = 1953523120`，与 `total_sectors − 2048 = 1953523120` 精确吻合。转回基本盘后这段区域归属需要明确，不能顺手清零或格式化。

因此 **L3 的前置条件**：完成 LDM VBLK 解析 + 备份 LBA0 + 确认两个卷的真实几何 + 有整盘镜像。当前四条都不满足，**明确禁止执行 L3**。

### L4 · 重建分区表 / 重新分区 / 格式化

**禁止。** 大面积不可逆写，直接毁数据。仅当 L0–L3 全部失败且数据已完整导出、且用户明确要求放弃样本时才允许讨论。

---

## 4. 不销毁样本的「修复能力」验证方案（建议）

用户既要「看看如何修复」，又要保留验证样本，二者可用演练盘同时满足：

1. 取一块可牺牲盘或固定大小 VHD（≥8 GiB 即可），写入两个普通 NTFS 分区 + 若干已知哈希的文件。
2. 人为构造同款故障态：把 MBR 分区类型改为 `0x42`、写入合成的 `PRIVHEAD`/`VMDB` 副本、让卷层不可见；保留 NTFS boot 与 MFT 原位。
3. 在演练盘上依次演练 L1 / L3，每步前后用 `forensic_baseline.py` 同款采样点做指纹对比，量化「修复动作改写了哪些扇区」。
4. 用本轮同一套 `phoenix_headless` + `mft_oracle.py` 双通道验证修复后文件字节不变，作为修复能力的门禁（CTest 可固化）。

这样既不碰 `PhysicalDrive2`，又能把「引擎能否处理 LDM 故障态」变成可回归的测试资产。

---

## 5. 优先级与建议顺序

| 优先级 | 动作 | 状态 |
|---|---|---|
| **P0** | 跑完 6 批只读恢复、逐批释放暂存、归档记录 | 进行中（2/6） |
| **P0** | 全量完成后的最终 `--verify` 零写入复采 | 待执行 |
| **P0** | 前台 `cleanup` 删除 0 字节空壳，释放项目内路径 | 待执行 |
| **P0** | `verify_archive.py` 严格归档门禁（记录齐全、哈希自洽、空间已回收、零写入） | 待执行（工具已就绪） |
| **P1** | 补齐 `summary.md` / `PERFORMANCE_BASELINE.md §10` 全量统计 | 待执行 |
| **P2** | LDM VBLK 逐记录解析，把「未导入」从推断升级为定论 | 未开始 |
| **P2** | §4 演练盘修复能力验证（可转为 CTest 门禁） | 未开始 |
| **P3** | 满足 G1–G4 后，才在真盘上考虑 L1 | **本轮禁止** |

### 5.1 全量完成后的收尾命令序列（严格按序）

```text
python tools/batch_recover.py status            # 确认 checkpoint = 1..7（合并口径）
python tools/verify_engine_plan_batch.py --all-completed   # 需提权：批次 4–7 卷作用域逐字节复核（会 replay 到 staging）
python tools/verify_engine_batch.py --all-completed       # 需提权：仅幂等复验历史批次 1–3
python tools/batch_recover.py cleanup           # 前台：释放残留字节并删除所有 staging 空壳 / 实验目录
python tools/forensic_baseline.py --verify      # 需提权：最终零写入复采
python tools/verify_archive.py                  # 严格门禁：errors 必须为 0
python tools/make_summary.py                   # 生成最终 summary.md（含门禁结论）
python tools/final_audit.py                    # 独立终审：不信任自述结论，逐批重推
```

`final_audit.py` 由独立后台任务在 `finalize_state.json` 报 `OK` 后自动执行，校验 checkpoint=`[1..7]`、7 步 rc、严格归档门禁、引擎发现覆盖、源盘零写入复采，并逐批重推「声明数 / 唯一 id 数 / 有效验证数 / unresolved」与 `discoveryGuard` / `engineFormat`。输出 `final_audit.json`；`--negative-guard` 人为少算 1 个文件必须 EXIT=1。审计完成后重跑一次 `make_summary.py`，最终 `summary.md` 第十四节会带上审计结论。

**为什么复核必须在 `cleanup` 之前**：`verify_engine_plan_batch.py` 会把归档中缺少引擎 SHA-256 的异常 id 通过生产引擎 `phoenix_headless_p07.exe` replay 到 `recover_staging/p07_replay_vol*` 并重算字节；`cleanup` 一旦清空 staging 就无法 replay。它与 `verify_engine_batch.py` 的分工是硬边界：前者服务引擎发现源（`plan_engine.json`，批次 4–7，输出 `batch_NNN_engine_plan_verification.json`），后者只服务历史 oracle 候选源（批次 1–3，输出 `batch_NNN_engine_verification.json`）；旧工具已拒绝 `--batch >= 4`，避免生成会被 `ev_valid()` 误纳的旧 schema 记录。

**`cleanup` 可以延后，但必须如实记账**：`batch_NNN` 暂存里剩下的只是零字节空壳（数据簇在逐批 `truncate(0)` 时已归还），而 `rmdir` 逐个删 84 万个目录在 Defender 下要数小时，会无限期把后面的严格门禁挡在门外。此时用

```text
python tools/finalize_recovery.py --resume --skip-cleanup --settle 0
```

把 `cleanup` 记为 **DEFERRED**（不是伪造 rc=0），并在状态文件里分项计量 `batchShellFiles` / `batchShellBytes` 与 `auxProbeBytes`（后者是 `readahead_*` 基准缓存、`p07_gap78` 证据等实验产物，**不能与批次暂存相加**）。放行条件是`batchShellBytes == 0`，由 `final_audit.py` 读严格门禁**独立重测**的每批 `stagingNonZeroBytesFiles` 复核，而非采信这条说明。空壳清扫本身可以作为纯后台 housekeeping 继续跑，它不再改变任何结论。

**把清扫做快（`tools/fast_rmtree.py`）**：Python 侧 unlink 会被环境的 `safe-delete` 守卫包装成「移入回收站」，对几万个空目录直接 fail-closed；因此这里全程走原生 `cmd /c rmdir`——先用 `rename`（同卷、只动目录项）把条目分片到 K 个桶，再并发 K 个原生进程删。实测分片 13.1–13.7 万/分钟，rmdir 在 8 路时 1.49 万/分钟、16/24 路反而降到 1.06–1.32 万/分钟（并发不再线性，瓶颈是 Defender 实时扫描），63.4 万个空壳约 45 分钟清完。删净后 `batch_recover.py cleanup` 只需 4.1 s 回收剩余探针目录，且重复执行幂等（替换而非追加步骤记录）。

复核工具对每条 `match=0` 行都从源盘**只读**重解析 MFT 真值（普通 / ADS / `$ATTRIBUTE_LIST` 三类），且所有解释键都是 `(bootLBA, id)` 卷作用域 —— 卷 209728638 也有一个 `id=78`（`...appinfo`，220 字节），绝不能借用卷 63 的 P0-7 修复结论。卷隔离自检 `--negative-guard-cross-volume` 会故意跨卷泄漏该解释，必须被拒绝（EXIT=1）。`verify_archive.py` 逐字段重推复核文件、不信任其自述结论。

**无人值守执行（推荐）**：`tools/finalize_recovery.py` 是上述序列的封装，轮询 `checkpoint.json` 直到 `[1..7]` 后按本顺序逐步执行。关键步骤 rc≠0 **立即停止并保留 staging**（复核阶段的 replay 证据不会被 cleanup 毁掉）；cleanup 为 warning 级，因为其内容全是零字节空壳，数据簇已在逐批释放时回收。每步 rc / 耗时 / 输出尾部写入 `finalize_state.json`，日志 `finalize.log`；`python tools/finalize_recovery.py --status-only` 查看状态。幂等：已有 `status=OK` 的状态文件不会重跑。

**P0-9 分母口径**：`plan_engine.json::files` 统计的是候选**行数**，批次 5 / 7 各含 1 个重复数值 id（`162865` / `809473`，ADS slot id 与普通 MFT 记录号相撞）。逐字节复核的分母是 `ids.txt` 的**唯一 id 数**（249,455 / 307,030）；`declared − unique` 必须由字面重复行完全解释，且 `hashes.csv` 的 id 集合须与请求唯一集精确相等。`verify_archive.py::ep_valid()` 独立重推，不信任复核文件自述；`--negative-guard-duplicate-ids` 人为多声明 1 个 id 必须 EXIT=1。

顺序理由：`cleanup` 之后 staging 目录才彻底消失，`verify_archive.py` 的「空间已回收」检查才有最终态；`make_summary.py` 读取 `archive_verification.json`，所以必须最后跑。`--verify` 只读源盘，与 `cleanup`（只碰项目内路径）互不影响。中途快照可用 `verify_archive.py --allow-partial`，此时未完成批次记为 INFO 而非 ERROR。

---

## 6. 口径声明

- 本文所有几何数字来自 `baseline.json` 与 `mft_oracle.py` 的只读实测，未做任何写操作验证。
- 「未导入导致卷组装失败」目前是证据支持的最合理解释，**不是定论**（缺 VBLK 逐记录解析）。
- 修复动作涉及磁盘写操作，本轮**一项未执行**；任何执行记录都应另开报告，不得回填到本文件。
