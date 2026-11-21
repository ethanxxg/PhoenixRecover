# TestDisk 7.2 对齐报告：文件系统广度 · 分区写回 · boot 修复

> **日期**：2026-11-21
> **对齐基线**：`D:\dev\testdisk-7.2`（TestDisk / PhotoRec 7.2，GPL v2+）
> **参考实现**：`src/parti386.c`（`write_mbr_i386` / `write_all_log_i386` / `write_MBR_code_i386`）、
> `src/partgptw.c`（`write_part_gpt`）、`src/ntfs_fix.c`（`repair_MFT`）、
> `src/ntfs.c`（`recover_NTFS`，backup 分支）、`src/fat32.c`（"Backup BS" / "Org. BS"）、
> `src/ext2_sb.c`（`interface_superblock`，仅定位备份超块并提示 fsck）
> **原则**：只读参考行为，不复制代码；所有写入一律走本项目 A0 能力链
> `WriteGuard → WriteGrant → WriteTransaction → DeviceWriter`。

---

## 1. 本轮已落地（P0）

| TestDisk 能力 | 参考位置 | 本项目实现 | 状态 |
|---|---|---|---|
| FAT32 用备份引导扇区覆盖主引导扇区 | `fat32.c` "Backup BS" | `repair/BootSectorRepair.h::PlanFat32BootFromBackup`（sector 6..8 → 0..2，仅当备份通过 BPB 校验且几何不超分区） | ✅ 规划器 |
| FAT32 用主引导扇区回写备份 | `fat32.c` "Org. BS" | `PlanFatTableMirror` 的 FAT#1→FAT#2 分支（表级，非扇区级） | 🟡 表级 |
| FAT 表镜像修复 | `fat1x.c` / `fat32.c` `repair_FAT_table` | `PlanFatTableMirror`：仅当一份头字节自洽时给出计划；两份都合理但不同则**拒绝猜测** | ✅ 规划器 |
| NTFS 从卷末备份重建引导扇区 | `ntfs.c` `recover_NTFS(backup=1)` | `PlanNtfsBootFromBackup`（末扇区校验 OEM/BPS/SPC/$MFT LCN 后才用） | ✅ 规划器 |
| NTFS `$MFT` 与 `$MFTMirr` 择优 | `ntfs_fix.c` `repair_MFT` | `PlanNtfsMftMirror`：`FILE` 魔数判定存活方；**mirror→MFT 方向默认关闭**，需 `exposeMirrorToMft` 专家门 | ✅ 规划器 |
| MFT 记录长度/簇大小语义 | `ntfs_fix.c`（`clusters_per_mft_record` 正负两种解释） | 同规则：`want = max(clusterSize, 4*recordSize)` | ✅ |
| ext2/3/4 定位备份超块 | `ext2_sb.c`（仅提示 `fsck -b`） | `PlanExtSuperblockFromBackup`：按 `3^i·5^j·7^k` 组号搜索，兼容 block 起点与 +1024 两种落位，校验 magic/log/inode size 后才用 | ✅ 规划器（比 TestDisk 多做一步：可给出可写镜像） |
| MBR 写回（保留引导代码 + 4 主分区项） | `parti386.c` `write_mbr_i386` | `disk/PartitionTableWrite.h::BuildMbr` | ✅ 构建器 |
| EBR 链写回（逻辑分区 + 0x0F 链接） | `parti386.c` `write_all_log_i386` | `BuildEbr`（调用方指定各 EBR 的 LBA 与链接类型） | ✅ 构建器 |
| GPT 写回（保护性 MBR + 主/备头 + 条目 CRC32） | `partgptw.c` `write_part_gpt` | `BuildGpt`（128×128 条目、first/last usable、备份位于末尾 33 扇区） | ✅ 构建器 |
| 写 MBR 引导代码 | `parti386.c` `write_MBR_code_i386_aux` | **故意不做**：调用方必须显式传入原有 440 字节引导区 | ⛔ 拒绝合成 |

**门禁**：`tests/test_repair_align.cpp` → CTest `RepairAlign`，**81 checks / 0 failure**。
覆盖正向（损坏→计划→应用→校验器重新通过→字节与损坏前完全一致）与
拒绝路径（无有效备份不出计划、两份 FAT 都合理则拒绝、MBR 重叠/零长/>2 TiB 拒绝、
GPT 越界/磁盘过小拒绝、mirror→MFT 需专家门）。
源码级负向两轮均按预期失败：
① 禁用 FAT32 备份检测 → 6 FAIL / exit 8；
② 跳过 MBR 冲突校验 → 3 FAIL / exit 8。

---

## 2. 文件系统广度：与 TestDisk 的差距清单

TestDisk 的 `src/*.c` 解析器覆盖 29 类，我们目前 6 类。按"对 Windows 数据恢复的真实需求"排序分阶段：

| # | 文件系统 | TestDisk | 本项目 | 阶段 |
|---|---|---|---|---|
| 1 | NTFS | 列目录/复制/删除 | ✅ 解析 + 删除 + 逐字节验证 | 已有 |
| 2 | FAT12/16/32 | 列目录/复制/删除/boot 修复 | ✅ 解析 + 删除 | 已有 |
| 3 | exFAT | 列目录/复制/删除 | ✅ 解析 + 删除（NoFatChain） | 已有 |
| 4 | ext2/3/4 | 列目录/复制/删除(仅 ext2) | ✅ 解析 + 删除 + JBD2 回放 | 已有（更深） |
| 5 | ReFS | ❌ | ✅ 解析 + 删除残留页 | 已有（TestDisk 没有） |
| 6 | XFS | 分区发现 + 列目录 | ❌ | **P1** |
| 7 | btrfs | 分区发现 + 列目录 | ❌ | **P1** |
| 8 | HFS/HFS+/HFSX | 分区发现 + 列目录 | ❌ | **P1** |
| 9 | APFS | 分区发现 + 列目录 | ❌ | **P1** |
| 10 | ISO9660 | 列目录 | ❌ | P2 |
| 11 | UFS1/UFS2 | 分区发现 + 列目录 | 🟡 仅 `DetectUfs1` 探测 | P2 |
| 12 | JFS | 分区发现 + 列目录 | ❌ | P2 |
| 13 | ReiserFS 3.5/3.6/4 | 分区发现 + 列目录 | ❌ | P2 |
| 14 | Linux swap | 分区发现 | ❌ | P2 |
| 15 | LVM / LVM2 | 识别 + 只读 | ❌ | P2 |
| 16 | LUKS | 识别 + 解锁 | ❌ | P2 |
| 17 | Linux md RAID | 识别 1/4/5/6 | ✅ 探测 + 只读重组（更强） | 已有 |
| 18 | VMFS | 分区发现 | ❌ | P3 |
| 19 | ZFS / Btrfs 池 | 识别 | ❌ | P3 |
| 20 | BeFS / CramFS / GFS2 / HPFS / FATX / NSS / NextFS / SysV / WBFS / Sun | 分区发现（多数不可列文件） | ❌ | P3（按需） |

**P1 的口径**：只做**只读挂载 + 目录枚举 + 文件簇映射**（能进现有 `FileSystem` 接口、能被 `RecoverEngine` 恢复、能被 `phoenix_headless verify` 逐字节校验），不做写、不做格式化。每个 FS 必须配：① 独立 oracle 金标；② 逐字节恢复门禁；③ 源码级负向守卫。缺一项不算完成。

---

## 3. 分区表类型：与 TestDisk 的差距

| 分区表 | TestDisk | 本项目 | 阶段 |
|---|---|---|---|
| Intel MBR（含 EBR 链） | ✅ 读写 | ✅ 只读探测；🟡 写构建器（本轮） | 构建器已完成，接线待 A3 |
| EFI GPT（主/备头 + CRC） | ✅ 读写 | ✅ 只读探测；🟡 写构建器（本轮） | 同上 |
| Apple / Sun / BSD / Xbox / Humax / None | ✅ 识别（多数可写） | ❌ | 不做（Windows 主线无关） |

---

## 4. 阶段计划

| 阶段 | 内容 | 门禁 | 依赖 |
|---|---|---|---|
| **P0（本轮）** | boot/元数据修复规划器 + MBR/EBR/GPT 构建器 | `RepairAlign` 81 项 + 两轮负向 | 无（纯计算） |
| P1 | XFS / btrfs / HFS+ / APFS 只读解析（按此顺序） | 每 FS 独立金标 + 字节恢复 + 负向 | 无 |
| P2 | ISO9660 / UFS1-2 / JFS / ReiserFS / swap / LVM2 / LUKS 只读 | 同上（小 FS 可共享一套镜像生成器） | P1 的接口抽象 |
| P3 | VMFS / ZFS / BeFS 等长尾 | 按需 | — |
| **W（写路径）** | 把构建器接到 `WriteGuard(WritePartitionTable / WriteBootRecord)` → UI「保存分区表 / 修复引导扇区」 | A0 隔离测试环境验收 + 断电回滚演练 | A0 最后一项「隔离测试环境」 |

> **为什么现在不直接写盘**：`PHOENIX_ENABLE_DEVICE_WRITES` 仍为 0，A0 的隔离测试环境未建成。
> 本轮交付的是"可审计的 after-image + 校验 + 拒绝路径"，这是 TestDisk 那种
> `pwrite` 直写所没有的一层；接线后同一个 plan 只是多了"落到设备"的最后一步。

---

## 5. 明确边界（不得夸大）

1. 规划器**只读**，不打开任何写句柄；生产构建下即使接线也会返回 `WritesDisabled`。
2. FAT32 恢复只覆盖"主引导扇区损坏、备份完好"；主/备同时损坏时不猜测几何（TestDisk 允许人工输入）。
3. NTFS `$MFT` 从镜像恢复是**有损**的（会丢掉镜像未覆盖的新记录），置信度 35，必须专家门。
4. ext 超块恢复后 GDT / 位图可能仍旧陈旧，报告里固定提示 `e2fsck -p -b` 复核；与 TestDisk 一致，不假装"一键修好"。
5. GPT 构建器假设逻辑扇区 ≤ 512 字节的 MBR/保护性 MBR 布局；4Kn 保护性 MBR 未覆盖。
6. 全部验证均为**合成镜像**，未在真实损坏盘上执行过写回。
