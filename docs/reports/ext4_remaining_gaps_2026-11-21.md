# ext4 剩余深度缺口收口（2026-11-21）

本轮承接「这 2 个文件系统先对齐深度恢复」中用户选择的 **ext4 剩余缺口**，
聚焦四个此前明确标注的边界：JBD v1、FLEX_BG 跨组 GDT、外部日志设备、
掉电窗口。全部为引擎与测试改动，未接 GUI。

## 1. JBD v1 日志回放（已实现）

- `fs/ext4/Ext4Parser.cpp`：接受 journal superblock type 3（JBD v1）与 type 4（JBD2 v2）。
  v1 descriptor tag 为 `be32 blocknr + be32 flags`，随后 16 字节 UUID
  （除非 `JBD2_FLAG_SAME_UUID`）；v2 为 `be32 blocknr + be16 csum + be16 flags`，
  UUID 只在日志超级块出现一次。两者显式分派，避免 v2 规则解析 v1 造成整条日志错位。
- v1 无 checksum / 64-bit 特性，revoke 仍按 4 字节项处理，commit 只校验 magic/type/sequence。
- 金标 `ext4-journal-v1`：与 legacy v2 完全相同的字节语义，2 事务 / 2 overlay 块，
  `rescue.bin` 与 `later.txt` 必须按字节恢复。
- 负向守卫：临时拒绝 `SB_V1` → 0 回放、`rescue.bin`/`later.txt` MISSING，5 FAIL；恢复后全绿。

## 2. FLEX_BG 跨组 GDT 回放（已由门禁证明）

- 研究表明 FLEX_BG 移动的是位图与 inode table 到 flex group 的首组，**不移动
  主 GDT**；真正的风险是日志重放修改了 GDT 之后，若不再读 GDT，仍按旧表解析 inode。
- 金标 `ext4-flex-bg-gdt`：两个 4096 块组，`s_log_groups_per_flex=1`；
  group 1 的位图与 inode table 位于 group 0；home GDT 把 group 1 inode table 指向 0，
  正确指针只存在于已提交日志块。`far.txt` 是 inode 65（group 1 本地 0 号）。
- 引擎现有 `ReloadGroupDescriptors()` 在 overlay 非空后重读 GDT，因此无需新增生产代码即可
  解析；测试把这条既有语义固化为门禁：1 事务 / 1 overlay 块，`far.txt` 字节匹配。
- 负向守卫：临时跳过 replay 后的 GDT 重载 → `far.txt` 大小 0 / SHA-256 为空，3 FAIL；恢复后全绿。
- 踩坑：日志数据块必须是整块 1024 字节。把 64 字节 GDT 直接赋给 `buf[off:off+bs]`
  会使 Python `bytearray` 缩短 960 字节并整体错位；生成器现已补齐为整块。

## 3. 外部日志设备 `s_journal_dev`（已实现，边界明确）

- `Ext4Parser::SetExternalJournalImage(path)`：调用方在 `Open()` 前显式提供外部日志镜像；
  `s_journal_dev != 0` 时优先用它，`s_journal_inum` 路径保持原行为。
- 日志超级块 `s_uuid@0x30` 必须与 ext4 超级块 `s_journal_uuid@0xD0` 的 16 字节一致，
  否则拒绝回放；日志块大小必须等于卷块大小，`maxlen` 受外部镜像容量约束。
- 金标 `ext4-journal-external`：`s_journal_inum=0`、`s_journal_dev=1`，
  独立 `ext4-journal-external.journal.img`，1 事务 / 2 overlay 块，`rescue.bin` 字节匹配。
- 两条负向守卫：① 不提供外部日志 → 0 回放、文件缺失、3 FAIL；
  ② 翻转外部日志 UUID 一字节 → UUID mismatch、0 回放、3 FAIL。
- **边界**：引擎不自动枚举/猜测外部日志设备（卷镜像本身没有该信息）；当前只有 CTest
  直接调用该 API，headless 与 GUI 未接线，不能宣称「自动识别外部日志盘」。

## 4. 掉电窗口（合成模拟，非真实掉电卷）

- 金标 `ext4-journal-torn`：事务 101 的 descriptor/data 已写入，但 commit 块被清零，
  模拟 commit 前掉电。正确行为是只回放事务 100（1 事务 / 2 overlay 块），
  `rescue.bin` 恢复、`later.txt` 必须不存在。
- 负向守卫：临时把「坏 magic 的尾部」当成提交点，错误回放 pending 事务 →
  `later.txt` PRESENT、2 FAIL；恢复后全绿。
- **诚实边界**：这是合成窗口，不是真实掉电盘样本；真实坏扇区/掉电卷仍未量化。

## 回归与交付

- `Ext4DepthFeatures`：**375 checks / 0 failure**，13 个变体（新增 v1 / torn / flex-bg / external 4 个）。
- 全量构建 `mingw32-make -k -j6` → `BUILD_EXIT=0`。
- 全量 CTest（94 项）：**91 passed / 3 failed**，失败仅为此前已记录的基线项
  `DFR_NIST_Compare`、`DFR_NIST_BatchStrict`、`DFTT_11_CARVE_FAT`；本轮新增项全部通过。
- 定向回归：`Ext4DepthFeatures`、`CFReDS_EXT4_DFR01`、`Ext2Ext3LegacyGolden`、
  `DFR_Linux_Batch`、`RefsRecoverBytes`、`RefsVolume`、`EngineScanFormatMatch`、
  `RecoverOutputCollision` → 8/8 Passed。

未覆盖：真实掉电卷、META_BG、JBD2 64-bit tag、外部日志设备的自动发现与 UI 接线。
