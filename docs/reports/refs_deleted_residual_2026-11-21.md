# ReFS 删除残留恢复对齐（2026-11-21）

## 目标与缺口

用户目标：把 ext2/3/4 与 ReFS 的**深度恢复**对齐。ext4 线已完成
JBD v1 / FLEX_BG / 外部日志 / torn commit / checksum v2/v3 / 真实卷逐字节门禁；
ReFS 线此前仅"引擎完成"，`RefsParser.h` 明确标注：

> ReFS does not keep unlinked directory entries, so only live entries are
> enumerated. There is no deleted-name recovery here.

ReFS 删除文件时会移除目录行并摘除 object-table 链接，因此不存在可直接读取的
"unlinked 链表"。但 B+-tree 页字节与文件数据簇在被复用前仍留在镜像里——这正是
DiskGenius 一类工具能恢复 ReFS 删除文件的物理基础。

## 实现

### 引擎窄接口（`fs/refs/RefsVolume.h`）

只新增、不改动任何现有方法语义：

- `ForEachResidualDirRow(visitor)`：扫描所有物理簇，找出
  *MSB+ 签名 + 页版本 2 + 卷签名匹配*、且 **self-LCN 无法从 13 个 checkpoint
  根 + object table 的全部 live 目录页到达**的叶页，逐行回调 type 0x30 目录行。
- `ParseResidualRow(...)`：复用现有 `ParseDirEntry` / `ParseSubRecord` 把残留
  行解码为与 live 完全相同的 `RefsDirEntry`（name / 时间戳 / extents / inline）。
- private `CollectReachableRefs(...)`：只读取字节，跟随 internal 节点的
  PageRef 收集可达 self-LCN 集合，不修改引擎状态。

关键设计点：孤立残留页没有父 PageRef，无法复验 Merkle 摘要；准入条件是
**结构合法 + 不可达 + self-LCN 解析回自身物理位置**，宁可漏报不可误报。

实现过程中门禁发现并修复了一个真实缺陷：object table 是**叶子页**，live 目录页
的 PageRef 嵌在行 value 里，仅从 13 个根沿 internal 节点走永远到不了。若不把
`m_objectRoots` 纳入可达集合，6 个 live 目录行会被全部误标为"删除残留"。
修复后 live/deleted 严格分离。

### 适配器（`fs/refs/RefsParser.cpp/.h`）

`ScanMetadata()` 在 live 递归枚举后追加残留行扫描；`BuildNode(..., deleted)`
复用同一套名字/扩展名/时间戳/extent→cluster 翻译/内联数据逻辑，仅把 status
置为 `FileStatus::Deleted`。残留节点不进入 `m_inUseCount`。

### 独立金标（`testdata/tools/Gen-ReFSGolden.py`）

新增 `refs_v314_deleted_residual` 变体：

- 先写含 `ghost.dat`（extent，4096 B）的旧根目录页与 orphan 数据簇；
- 再让 live object table 只指向另一组干净的 `root_dir_slots`；
- `emit_page()` / `put()` 从不清零簇，残留自然字节完整但不可达；
- 独立真值表 `deleted.tsv`（name / size / sha256 / fnv1a / extents），
  与 live `files.tsv` 分离，由同一份明文 `content_for()` 计算，非 C++ 输出。

## 门禁与验证

- `RefsRecoverBytes`：走 **生产路径** `SectorReader` → 探测链 → `RefsParser`
  → `RecoverEngine::RecoverFile` → 与 `deleted.tsv` 逐字节 sha256 比对；
  同时断言 6 live 节点保持 Normal、恰 1 个 Deleted 节点、整镜像 FNV-1a 不变。
- 正向：`RefsRecoverBytes` 96 checks / 0 failure，ghost.dat `bytes ok deleted`。
- **源码级负向**：临时禁用 `ForEachResidualDirRow` → ghost.dat MISSING、
  3 FAIL、exit 8；恢复后全绿。
- **数据级负向**：把 ghost 数据簇翻转 1 字节 → `BYTES WRONG` +
  sha256 mismatch、1 FAIL、exit 8；金标已备份恢复，`--verify-only` 18 files/0 bad。
- `RefsVolume`：cases.tsv 15 行 / files.tsv 55 行、9 正例 / 5 反例、
  54 live file / 45 content，全绿（新变体的 6 个 live 文件照常纳入）。
- 定向回归 7/7（含 `EngineScanFormatMatch`、`RecoverOutputCollision`、
  `Ext4DepthFeatures`、`FormattedVolumeRecovery`）。
- 全量构建 `BUILD_EXIT=0`；全量 CTest 91/94，3 项失败为基线既有
  （`DFR_NIST_Compare`、`DFR_NIST_BatchStrict`、`DFTT_11_CARVE_FAT`）。

## 边界（不得过度声明）

- **金标获取**：按项目既有约定 `testdata/` 整体不入库（体积原因，见
  `testdata/测试镜像说明.md`）。本次变体由 `testdata/tools/Gen-ReFSGolden.py`
  生成；干净检出后须先运行该生成器重建 `refs_v314_deleted_residual.bin` 与
  `deleted.tsv`，否则 `RefsRecoverBytes` 的删除用例无法加载真值。生成器与
  所有既有 ReFS 变体同样不入库，这是本仓库的一贯边界。
- 这是**合成金标**上的删除残留恢复，不是真实 Windows ReFS 卷样本。
- 未解析 USN / change journal；只恢复"页字节仍完整"的删除文件，
  被复用/覆盖的页不在能力范围内。
- 孤立页无法复验 Merkle 摘要，准入依赖结构合法性 + 不可达性，
  真实卷上可能存在本方案不覆盖的残留形态。
- 引擎接口尚未接入 headless 命令行删除列示，也无 GUI 控件；
  当前通过 `RefsParser` → `FileSystemModuleService` 生产探测链生效。
