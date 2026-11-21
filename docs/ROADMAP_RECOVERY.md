# 恢复能力路线图

## 已完成（P0）

| 能力 | 模块 | 备注 |
|------|------|------|
| NTFS / FAT / exFAT / ext4 元数据 | `FileSystemModuleService` | ext4 已删 dentry/htree/孤儿 inode 打标已实现 |
| 签名雕刻 | `CarveModuleService` | 含 SQLite 魔数发现 |
| 分区 / 丢失分区 | `PartitionRecoveryService` | `ProbeMode` Full / Table / Lost |
| FAT/NTFS 碎片（贪心） | `FragmentStitchService` | 格式感知打分 |
| **SQLite 页级恢复** | `SqliteRecoverService` + `SqliteRepair` | 截断尾坏页、雕刻偏移；WAL/指针重映射 P1 |

## 计划中

| 优先级 | 项 | 目标 |
|--------|-----|------|
| **P1** | SQLite WAL/journal | 合并 `-wal` 提高可打开率 |
| ~~P1~~ | ext4 已删文件 | 已实现：`Ext4Parser` htree + Deleted 打标；`DFR_Linux_Batch` PASS |
| **已完成** | **Graph 碎片拼接** | `GraphCarveFragmentStitcher` + `FragmentStitchStrategy::GraphCarve` |
| **P2（部分落地）** | exFAT 碎片 | 已复用 `FatFragmentStitcher::TryImproveExfatNode`（经 `FragmentStitchService` 按 `fsType` 分发）；独立 `ExfatFragmentStitcher` 与 exFAT 质量门禁 CTest 仍待办 |
| **P3** | 恢复后自动 SQLite 修复 | `RecoverEngine` 导出 `.db` 后可选调用 `SqliteRecoverService` |

## Graph 策略（已实现）

见 `docs/FRAGMENT_STITCH_MODULE.md` 与 `recover/GraphCarveFragmentStitcher.h`。

**阶段 3**：`CarveGraphMergeService` 已接入 `UnifiedVolumeScanner`（见 `FRAGMENT_STITCH_MODULE.md`）。

---

# DiskGenius 对齐路线（A0–A5）

> **决策日期**：2026-11-07  
> **定位变更**：由「只读数据恢复工具（对标 EaseUS）」扩展为「数据恢复 + 分区管理 + 备份还原 + 磁盘工具一体化工具（全面对齐 DiskGenius）」。  
> **对标基线**：DiskGenius `6.2.0.1829`（2026-06-08）。  
> **配套文档**：[PRODUCT_SCOPE_SCAN_RECOVER_REPAIR.md](./PRODUCT_SCOPE_SCAN_RECOVER_REPAIR.md) §2 / §11（定位措辞已同步）、[ARCHITECTURE.md](./ARCHITECTURE.md)（分层约束待更新）、[PERFORMANCE_BASELINE.md](./PERFORMANCE_BASELINE.md) §6.2（竞品对比数据）、§8（并行 RAW 扫描吞吐基线）。  
> **本文与上文关系**：上文「恢复能力路线图」的 P1/P2/P3 编号继续有效，作为**恢复算法子轨**并入本路线的「并行轨」。

## 1. 优先级重排原则

原计划以「扫描 / 恢复 / 修复」三条主线为唯一 KPI，因此广度类能力被冻结。定位变更后：

| 变化 | 说明 |
|------|------|
| **广度优先** | A1–A4 的功能补齐成为主线，其优先级高于引擎深度优化 |
| **引擎深度改为并行轨** | 扫描并行化、签名匹配、大结果集 UI、FS 特性、雕刻/修复覆盖持续推进，但**不阻塞** A1–A3 |
| **三主线降级为质量基线** | `PRODUCT_SCOPE §5` 场景门禁、公开数据集金标（NIST / DFTT / Digital Corpora）仍是发版硬门槛，不再作为阻止范围扩展的理由 |
| **安全底座先于一切写功能** | A0 未验收前，不得合入任何会打开设备写句柄的代码 |

## 2. 阶段总表

| 阶段 | 目标 | 主要能力 | 依赖 | 风险 | 退出门禁（须全部满足） |
|------|------|----------|------|------|------------------------|
| **A0** | 定位 + 写路径安全底座 | 定位文档；写目标模型、授权白名单、事务/回滚、设备写句柄封装、隔离测试环境 | — | — | 五条安全不变量全部有自动化用例；默认拒绝策略生效；CTest 全绿 |
| **A1** | 只读广度（可与 A0 并行，不需写权限） | 虚拟盘/镜像只读挂载（VMDK/VHD/VHDX/VDI/QCOW/IMG）、BitLocker 解锁、RAID 只读虚拟重组（0/1/4/5/6/10/JBOD）、ReFS 解析、智能加载损坏 NTFS、按类型恢复参数化 | 无 | 低 | 每能力配公开数据集金标用例；扫描源保持只读；`UiTypeCoverage` 不回退 |
| **A2** | 写**目标盘** | 镜像还原、磁盘/分区克隆（按文件/按扇区/按布局）、系统迁移、镜像内文件提取 | A0 | 中 | 目标盘不得为任何已注册源盘；操作前后全盘哈希比对；中断可重放 |
| **A3** | 写**源盘元数据** | 分区创建/删除/格式化、无损调整/拆分/合并/隐藏、MBR↔GPT 无损转换、分区表备份还原 | A0、A2 | 高 | 事务日志完整；断电/中断模拟测试通过；回滚可验证 |
| **A4** | 高危 / 不可逆 | 丢失分区写回分区表、重建 MBR、Windows 引导修复、永久擦除/空闲区擦除、扇区编辑器写入、坏道检测与修复 | A0、A3 | 极高 | 一次性确认令牌 + 强制回滚镜像；独立物理测试机验收；双人复核清单 |
| **A5** | 生态与硬件兼容 | WinPE 启动盘、多语言（简中/繁中/英起步）、只读 CLI、Excel 报表、4Kn / RAID 控制器 / NVMe / 存储池 / 动态磁盘兼容矩阵 | A1–A4 | 中 | 兼容矩阵有实测记录；`Regress.ps1` Release 全绿；安装包与签名产物就绪 |
| **并行轨** | 恢复算法深度 | 扫描并行化、签名多模式匹配（AC）、10 万条 UI、NTFS `ATTRIBUTE_LIST`/`$I30`/EFS、ext4 `inline_data`/JBD2、exFAT nameHash、雕刻与修复覆盖、误报率量化 | 无 | 低 | 见 `PRODUCT_SCOPE §5.5` 与 `PERFORMANCE_BASELINE §4/§5.2` |

## 3. A0 细化：写路径安全底座

### 3.1 五条安全不变量（不可协商）

| # | 不变量 | 自动化验证方式 |
|---|--------|----------------|
| S1 | **已注册为扫描源的设备，会话内禁止任何写打开** | 源盘注册后对同设备发起任意写授权，必须返回拒绝 |
| S2 | **默认拒绝**：目标未显式登记，或操作类别未显式授权，一律拒绝 | 空白名单下所有 `WriteOperation` 均拒绝 |
| S3 | **不可逆操作必须携带一次性确认令牌，且必须存在回滚镜像** | 无令牌 / 无回滚镜像时拒绝；令牌不可复用 |
| S4 | **写前必须有可验证基线**：源盘只读镜像或全盘哈希 | 缺基线时拒绝 A3/A4 类操作 |
| S5 | **非 Windows 平台在写路径完备前编译期禁用** | 平台宏守卫 + 编译期断言 |

### 3.2 组件清单与状态

| 组件 | 职责 | 状态 |
|------|------|------|
| `disk/WriteGuard.h` → `WriteTarget` | 目标类型（普通文件 / 镜像文件 / 目标磁盘 / 源磁盘）、物理磁盘号、卷标识、基线、回滚镜像标记 | **已落地** |
| `disk/WriteGuard.h` → `WriteGuard` | 白名单登记 + 授权决策（纯逻辑、零 I/O、默认拒绝、一次性确认令牌） | **已落地**，CTest `WriteGuard` / `WriteGuardDefaultDeny` |
| `disk/WriteGuard.h` → `WriteGrant` | 不可伪造、单次消费的授权凭证；`Grant()` / `GrantNewFile()` 是进程内唯一来源（私有构造 + friend） | **已落地**（A0-3 升级，原有 `WriteDecision` 接口保留） |
| `disk/WriteTransaction.h` | 事务日志：前/后镜像摘要、重叠区间拒绝、FNV-1a 校验和、LIFO 回滚计划、基线校验、断电恢复（未提交一律转 `Aborted`，只回滚不前滚） | **已落地**，纯逻辑零 I/O，CTest `A0Transaction` / `A0TransactionDefaultDeny` |
| `disk/DeviceWriter.h` | 唯一写句柄封装**骨架**：无默认构造、不可拷贝，仅接受 `WriteTransaction&`；能力链 `WriteGuard → WriteGrant → WriteTransaction → DeviceWriter` 用 `static_assert` 固化；`WriteSectors()` 只登记写意图并返回 receipt，不打开句柄 | **已落地（骨架）**，真实 `WriteFile` 待 A0 验收后仅替换该函数体 |
| 隔离测试环境 | 物理测试机 + 可回滚镜像回放 + 写前后全盘哈希比对 | **待建设，与 A2 同步（A0-3 唯一剩余项）** |

> A0-3 交付的代码**仍然零 I/O**：三个头文件均不含 `windows.h`、不调用 `CreateFile`/`WriteFile`、不持有句柄。事务层把「写前基线 → 前镜像留痕 → 逆序回滚 → 后镜像校验 → 日志校验和」做成可测试的契约，真实写入接入时只需替换 `DeviceWriter::WriteSectors()` 函数体，授权链与回滚契约不变。
>
> `WriteGuard` 只做决策，不打开任何句柄、不接 UI、不执行写入。`WriteGrant` 的构造函数私有且仅 `WriteGuard` 为友元，`WriteTransaction` 无默认构造，因此**没有任何调用方能在绕过 `Authorize()` 的前提下构造出 `DeviceWriter`**。
>
> **编译期总开关**：`PHOENIX_ENABLE_DEVICE_WRITES` 默认 `0`，此时所有设备级写操作一律返回 `PlatformNotSupported`，只有隔离测试构建（`-DPHOENIX_ENABLE_DEVICE_WRITES=1`）能跑完整授权逻辑。A0 验收前生产构建不得置 `1`。
>
> **验证记录（2026-11-07，A0-2）**：`WriteGuard`（逻辑全分支）与 `WriteGuardDefaultDeny`（生产默认拒绝）两个 CTest 用例在 `PHOENIX_BUILD_GUI=OFF` 配置下编译运行通过；`tests/CMakeLists.txt` 以二进制追加方式接入，未改动既有行。
>
> **验证记录（2026-11-07，A0-3）**：新增 `A0Transaction`（宏=1）与 `A0TransactionDefaultDeny`（宏=0）两个 CTest 用例。覆盖：单次消费授权凭证、意图范围/摘要/分隔符/目标/操作校验、重叠区间拒绝、Prepare→Commit 封存、LIFO 回滚计划、基线不符拒绝关闭、日志截断/篡改的校验和检出、断电恢复（Committed→Committed，其余→Aborted）、目标/操作不匹配的恢复拒绝、S5 生产默认拒绝、S1 源盘禁写、S3 一次性令牌复用检出。`g++ -std=c++17 -Wall -Wextra` 两个宏分支均无警告；`WriteGuard` 两分支回归通过。

## 4. 各阶段条目明细

### A1 — 只读广度

| 项 | 对标 DG 能力 | 落点 |
|----|--------------|------|
| A1-1a | 虚拟磁盘**只读扇区映射** | ✅ `disk/VirtualDiskSource.h`：RAW/IMG、VHD fixed、VHD dynamic（BAT + 位图 + 稀疏零填充）；VHDX/VMDK/QCOW2/VDI/差分 VHD 明确检出并拒绝。CTest `VirtualDiskSource` |
| A1-1b | 扩展格式只读映射 | ✅ VHDX（双头/区域表/元数据项/交错 BAT，fixed+dynamic）、VMDK monolithicSparse（KDMV→GD→GT→grain，含零化 grain GTE）、QCOW2 v2/v3（L1/L2 簇表）、VDI（平面块映射）；CTest `VirtualDiskExtended`。不支持的子型一律 `UnsupportedVariant` + 原因，不静默当裸盘 |
| A1-1c | 真实镜像金标验证 | ✅ 10 个由原生工具生成的真实镜像（Hyper-V / Virtual PC / VMware / Disk2vhd / GRUB）+ 独立 Python 参考实现逐扇区交叉验证；CTest #68 `VirtualDiskGolden`。样本与工具见 `testdata/virtual-disk-golden/` |
| A1-1d | 真实镜像覆盖补缺 | QCOW2 / VDI / 普通 monolithicSparse VMDK 的成功读取路径仍无真实样本（当前仅合成 fixture 或拒绝路径）；见下文「A1-1c 覆盖边界」 |
| A1-2 | 虚拟磁盘作为**只读扫描源**接入扫描主路径与 UI 镜像入口 | ✅ `disk/VirtualDiskSectorSource.h`（只读字节源桥接）+ `SectorReader::OpenVirtualImage()` / `ReadVirtualSectors()` + `RecoveryScanWorker` 按扩展名路由 + `RecoveryLocationPage` 文件过滤器与提示；CTest #69 `VirtualDiskSectorSource`。**镜像内文件浏览/提取**属 A2-4，仍待办 |
| A1-3a | BitLocker 分区**只读解锁** | ✅ `disk/BitLockerCrypto.h`（纯 C++17 AES-128/192/256、SHA-256、AES-CCM、AES-XTS/CBC 扇区解密）+ `disk/BitLockerVolume.h`（Win7+ FVE 卷头 / 3 份元数据副本优选 / VMK 保护器 / FVEK 解封 / 卷头重定位 / 实现 `IVirtualDiskReader`）；CTest #71 `BitLockerCrypto`、#72 `BitLockerVolume`。**纯 `disk/` 层，未接 `SectorReader`、未接 UI** |
| A1-3b | BitLocker 真实卷兼容 | **待办**：真实 Windows 加密卷金标样本、TPM / TPM+PIN 保护器、Vista v1、BitLocker To Go 混合卷、Elephant Diffuser（0x8000/0x8001）、4Kn 卷、XTS 密钥布局在真实卷上的确认、配置驱动的非 GUI 扫描入口。见下文「A1-3a 覆盖边界」 |
| A1-4a | 虚拟 RAID **只读重组引擎** | ✅ `disk/VirtualRaid.h`（header-only）：JBOD/0/1/4/5/6/10、6 种 parity 布局、缺盘与坏扇区重建（RAID6 双盘 GF(2⁸) P+Q）、嵌套 RAID50/60、parity 一致性自检；CTest #70 `VirtualRaid`。**纯 `disk/` 层，未接扫描主路径、未接 UI**（本阶段按用户要求不改 GUI） |
| A1-4b-a | 自动布局探测 + 配置驱动的非 GUI 组合只读源 | ✅ `disk/RaidProbe.h`（mdadm 0.90 / 1.0 / 1.1 / 1.2 超块解析与校验和、盘序 / chunk / layout / data_offset / 降级槽取自元数据、几何经 `VerifyStripe()` 与卷内已知明文锚点复核、受限盲探测且歧义/预算超限一律拒绝）+ `disk/ScanSourceChain.h`（严格 `key=value` 配置 → 容器解包 → RAID/探测 → BitLocker 解锁 → ReFS 桥接，统一 `IVirtualDiskReader` 输出）；CTest #75 `RaidProbe`、#76 `ScanSourceChain`。**零 GUI 改动、零设备句柄、零写方法**；接线为注入式字节源，尚未接 `SectorReader` 设备主路径。见下文「A1-4b-a 实装说明」 |
| A1-4b-b | 真实阵列兼容与设备主路径接线 | **待办**：SNIA DDF / Intel RIS / 厂商专有布局（HP 延迟 parity、Promise wide-pace Q、RAIDZ、far/offset RAID10）、mdadm reshape / bitmap / PPL / journal / bblog / 坏块日志、4Kn、真实阵列金标样本、事件数不一致时的副本仲裁；以及把 `SourceChain` 接到 `SectorReader` 的**非 GUI** 设备入口。真实阵列金标完成前不得在界面暴露 RAID 入口，也不得宣称「已支持真实阵列恢复」 |
| A1-5a | ReFS **只读解析引擎** | ✅ `fs/refs/RefsCrypto.h`（CRC-32C / CRC-64/NVME / SHA-256 / VBR ROR1+ADD）+ `fs/refs/RefsVolume.h`（header-only 纯 C++17、零 Qt/Win32/写）：VBR 几何与自校验、`-FVE-FS-` 加密卷识别、SUPB 三副本 / CHKP 双副本择优、13 根页引用 Merkle 校验、容器表 VLCN→PLCN、MSB+ B+ 树（根描述/内部节点/叶行/不变量）、对象表 / 父子表、目录项两种布局、inline 与 24 字节 extent 的文件数据重组；CTest #73 `RefsCrypto`、#74 `RefsVolume`。**合成卷金标，`fs/` 层独立实现，未复用 `FileSystem` 接口、未接 `SectorReader`、未接 UI**。见下文「A1-5a 覆盖边界」 |
| A1-5b | ReFS 真实卷兼容与接口接线 | **接口接线已完成（2026-11-21，零 GUI 改动）**：新增 `fs/refs/RefsParser`（`FileSystem` 适配层：`SectorReader`+`Partition` → `ReaderImage : IRefsImage` 只读桥，VLCN→PLCN 折算成绝对簇号填入 `FileNode::clusters`，inline `$DATA` 填 `residentData`），探测链在 **FAT 之前**接入 ReFS（ReFS VBR 是 NTFS 式外观，晚于 FAT 会被误判），`RecoverEngine::FsVolumeHints` 增 `isRefs` 并复用 `IsClusterRelative()`；CTest `RefsRecoverBytes`（80 项，`refs-golden` 6 条目含嵌套 `subdir/inside.txt`，逐文件 SHA-256 + 整镜像 FNV-1a）。**本阶段查出并修复两个真实缺陷**（见下文）。**仍待办**：真实 Windows ReFS 卷金标样本（3.4–3.14 / 4Kn / 64K 簇 / SHA-256 / 升级卷）、legacy v3.4–v3.9 对象表与 104 字节页引用、embedded `$DATA` 完整 inner-header + 嵌入式 B+ 树节点 framing、split（kf=0x02）文件的 type-0x40 backing record、ADS / 快照 / 压缩 / integrity streams / 硬链接、SUPB/CHKP 自校验精确清零范围复核。真实卷金标完成前不得宣称「已支持真实 ReFS 卷恢复」，也不得在 UI 暴露 ReFS 入口 |
| A1-6 | 智能加载损坏 NTFS、按类型恢复参数化（范围/对齐/最大尺寸/类型筛选） | **签名多模式匹配（AC）引擎底座已完成**（`scan/SignatureMatcher.h` header-only 流式 Aho-Corasick + CTest #77 `SignatureMatcher`）；**已接 `RawScan` 主路径**：`SignatureDB::ScanBatch` 流式 AC 批扫描替换线性 `memcmp`（A1-6，CTest #77 + 接线验证）；**已接 `DeepScan` 智能加载损坏 NTFS**：识别 NTFS/FAT boot 但结构化解析 < 8 文件时回退 `RawScan` RAW 雕刻（A1-7，CTest `DeepScanFallback` 全绿）；**并行轨 RAW 扫描（A1-8）**：跨 N 线程分块 + 512 重叠 + 按 rawOffset 合并（`RawScan::Run(reader,partition,numThreads)`，每线程独立 `SectorReader`+`SignatureDB`，CTest `RawScanParallel` 对 N=2/4/8 与串行逐偏移一致）；**A1-9 已完成 `SignatureDB::LoadFromFile` 的 Qt 解耦**：改用 `std::ifstream`(`Utils::WideToUtf8`) + 新增零依赖 `scan/SignatureJson.h`，headless/非 GUI 与 GUI 构建行为一致（CTest `SignatureDBLoad` 90 项全绿），并补齐文档引用但缺失的 `testdata/custom_signatures.json` 样例 |

> **A1-1 拆分说明**：原条目的「虚拟磁盘加载与格式互转」中，**只读加载**归 A1-1a/1b，**格式互转**会产出新文件，属写能力，移至 A2（与镜像还原同轨），并受 `WriteGuard` 的 `WriteTargetDisk` / `WriteNewFile` 授权约束。
>
> `VirtualDiskSource` 只做「虚拟 LBA → 文件偏移」的翻译，持有 `const IVirtualDiskReader&`、不含任何写方法；`FileVirtualDiskReader` 以 `std::ios::in` 打开。接入扫描主路径时须复用 `SectorReader` 的只读语义，不得把镜像文件登记为可写目标（S1）。已检出但未支持的格式一律返回 `UnsupportedFormat` / `UnsupportedVariant` + 原因，避免把容器头当裸盘扫描产生误报。
>
> **A1-1b 明确不支持的子型**（返回 `UnsupportedVariant`，`Info().detail` 给出原因）：VHD 差分盘、VHDX 差分盘 / 4Kn、VMDK flat / split / streamOptimized / 压缩 grain、QCOW2 带 backing file / 加密 / 压缩簇 / incompatible feature 位、VDI 差分盘 / 非 512 逻辑扇区。VHDX 活动日志、VMDK 非正常关闭、QCOW2 快照仅产生 `Info().warnings` 告警，不阻断读取。
>
> **格式布局依据**（2026-11-07 核实）：MS-VHDX 2.2.3.2 区域表 / 2.5.1 BAT 条目状态与 chunk 交错规则；VMware Virtual Disk Format 5.0 `SparseExtentHeader`（`KDMV`，字段全为 512 字节扇区单位）；QEMU qcow2 规范（头字段 0–103、L2 标准簇描述符 `0x00FFFFFFFFFFFE00` 偏移掩码 / bit63 压缩）；VirtualBox VDI 头（0x154 块映射、0x158 数据区、0x170 磁盘大小、0x178 块大小）与块映射特殊值 `0xFFFFFFFF`/`0xFFFFFFFE`。
>
> **A1-1b 验证记录**：`VirtualDiskExtended` CTest 覆盖四种格式的正常读取、稀疏/未分配零填充、越界拒绝，以及 16 种异常子型（父盘、4Kn、日志告警、压缩、streamOptimized、flat 描述符、backing file、加密、incompatible 位、压缩簇、快照告警、零化块、差分盘、坏区域表、非正常关闭）。`g++ -std=c++17 -Wall -Wextra` 零警告。
>
> **A1-1c 真实镜像金标（2026-11-07）**
>
> 样本来源：`qemu/qemu` 的 `tests/qemu-iotests/sample_images`（固定 commit `71f25c75417d31c214ba2cd26b273c114fc99987`）。该目录 README 明确要求「由原生工具创建样本镜像」——VHDX 由 Hyper-V、VHD 由 Virtual PC / Disk2vhd、VMDK 由 VMware 生成，**不是 QEMU 自造，也不是本项目的合成 fixture**。共 10 个真实镜像、644 MiB；SHA-256 固化于 `testdata/virtual-disk-golden/manifest.json`，用 `testdata/tools/Fetch-VirtualDiskGolden.py` 可一键复现，`--verify-only` 离线校验。
>
> 交叉验证方式：`testdata/tools/Verify-VirtualDiskGolden.py` 用 Python `struct` 依规范**独立**重算 VHD dynamic（footer → dynamic header → BAT → 位图）、VHDX（双头 → 区域表 → 元数据项 → 交错 BAT）与 RAW 的扇区内容，产出 `expectations.tsv`；`tests/test_virtual_disk_golden.cpp` 用产品 C++ `VirtualDiskSource` 打开同一批真实镜像，逐扇区比对 FNV-1a 64 指纹，并校验格式、虚拟大小、扇区/块大小、打开结果码与越界拒绝。两套实现独立编写，可互相暴露字节序、字段偏移、BAT 交错、位图索引错误。FNV-1a 仅作内容指纹；样本完整性由 manifest 的 SHA-256 保证。
>
> | 容器 | 真实样本 | 本轮验证范围 |
> |---|---|---|
> | VHD dynamic | Virtual PC、Hyper-V 2012 R2、Disk2vhd（3 个） | 127 GiB / 251 MiB 虚拟几何、BAT 稀疏零填充、越界拒绝 |
> | VHDX dynamic | Hyper-V 1 GiB、Disk2vhd 系统盘、Hyper-V 10 GiB（3 个） | 真实 MBR 前导 `33 C0 8E`、qemu `0xA5` 填充、交错 BAT、活动脏日志告警、4 MiB / 32 MiB 块 |
> | VMDK | VMware version3、AFL fuzz 损坏样本（2 个） | `createType="streamOptimized"` 压缩子型正确拒绝、损坏头鲁棒性 |
> | RAW | GRUB MBR、AFL fuzz 残片（2 个） | 真实 MBR 前导 `EB 63 90`、残尾扇区、越界拒绝 |
>
> 结果：CTest #68 `VirtualDiskGolden` 覆盖 **10 个真实镜像 / 69 个扇区指纹 / 8 个越界检查**，与独立参考实现全部一致；另做负向验证（篡改一枚指纹后测试如期 FAIL），确认金标不是恒真断言。A0/A1 共 8 个 CTest 全绿，`g++ -std=c++17 -Wall -Wextra` 零警告。样本缺失时测试明确输出 `SKIPPED` 并给出获取命令，不会静默通过。
>
> **A1-1c 覆盖边界（不得夸大）**：QCOW2、VDI、普通 monolithicSparse VMDK 三种**成功读取**路径仍只有合成 fixture，没有真实样本；三个真实 VHD dynamic 镜像的数据区全为零，只验证了稀疏/几何/BAT，未验证「含数据的 VHD dynamic 读取」。曾尝试从 `log2timeline/dfvfs` 获取同一 ext2 文件系统的 `.raw` + `.vmdk` + `.qcow2` + `.vhd` + `.vhdx` 多容器组（可用真实 RAW 做逐扇区 oracle），但沙箱网络对大文件下载与 GitHub 未认证 API 配额受限，记为 **A1-1d**。A1-2 已完成接线，但**在 A1-1d 完成前不得宣称「已支持 VMDK/QCOW2/VDI 真实镜像恢复」**；UI 过滤器只列 VHD/VHDX/VMDK/QCOW2/VDI，其余子型一律走明确拒绝。

> **A1-2 实装说明（2026-11-07）**
>
> 桥接层 `disk/VirtualDiskSectorSource.h` 把 `VirtualDiskSource` 的「虚拟 LBA → 文件偏移」能力接入 `SectorReader`：Windows 侧用 `CreateFileW(GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING)` 打开（保留 Unicode 路径、无 `GENERIC_WRITE`、不允许其他进程改写镜像，符合 S1），Unix 侧沿用 `FileVirtualDiskReader` 的 `std::ios::in`。`SectorReader` 新增 `OpenVirtualImage()` / `IsVirtualImage()` / `GetVirtualDiskInfo()`，`ReadSectors()`/`ReadBytes()` 在虚拟模式下分流到 `ReadVirtualSectors()`（保留取消、批量快速路径、逐扇区重试、坏扇区零填充 + `BadSectorMap`、进度回调语义），`Close()` 释放源。`PartitionProbe`、`UnifiedVolumeScanner`、`RecoverEngine`、预览等全部下游仍拿 `SectorReader&`，**零改动**。
>
> 路由规则：`RecoveryScanWorker` 仅对 `.vhd/.vhdx/.vmdk/.qcow2/.vdi` 等容器后缀调用 `OpenVirtualImage()`；`.img/.dd/.raw` 继续走 `OpenImage()`，以保留裸镜像的精确字节长度（含残尾扇区）。`OpenVirtualImage()` 若识别为无容器签名的 RAW 会**明确拒绝**并提示改用 `OpenImage()`，避免把改了后缀的裸盘静默截断。差分盘 / 压缩 grain / 加密卷等不支持子型把解析器的原因透传到 UI 错误框，不会退化为裸盘扫描产生误报。
>
> **验证记录**：CTest #69 `VirtualDiskSectorSource` 覆盖后缀路由、合成 VHD dynamic 的几何/格式/扇区读取/稀疏零填充/`ReadBytes` 跨扇区/进度回调/越界（宽松与严格两种策略）/取消标志/关闭后状态、**打开前后镜像文件逐字节不变**（S1 实证）、RAW 拒绝、裸镜像 `OpenImage` 回归、差分盘原因透传；并通过 `SectorReader` 真实扫描路径对 **6 个真实镜像 / 66 个扇区指纹 / 6 个越界检查** 与独立 Python oracle 逐项比对。篡改一枚金标指纹后测试如期 FAIL（负向验证）。`g++ -std=c++17 -Wall -Wextra` 零警告；三处 Qt 侧改动用 Qt5.14.2 头文件 `-fsyntax-only` 通过；A0/A1 共 9 个 CTest 全绿。
>
> **尚未验收**：GUI 端到端手工用例（在界面选 `.vhdx` → 扫出文件 → 预览 → 恢复写出）未执行；A1-1d 的真实样本缺口未补；VMDK flat/split 多文件挂载仍需扩展 `IVirtualDiskReader`。因此本阶段结论是「**扫描主路径已可读取 VHD/VHDX 真实镜像**」，不是「全格式虚拟盘恢复已可用」。

> **A1-4a 实装说明（2026-11-07）**
>
> `disk/VirtualRaid.h` 实现 `IVirtualDiskReader`，因此组件盘可以是裸镜像、`VirtualDiskSource` 支撑的容器镜像（VHD/VHDX/VMDK/...）、内存 fixture，或另一个 `VirtualRaid`——后者直接给出嵌套 RAID50/60 能力，无需额外代码。全类只持有 `const IVirtualDiskReader*`，**无写方法、无文件/设备句柄、无 Qt / Win32**，构建上不新增 `.cpp`、不改根 `CMakeLists.txt`。
>
> **布局依据**（动手前联网核实）：Linux md `raid5.c::raid5_compute_sector` 的四种布局——left `pd_idx=(N-1)-(stripe mod N)`、right `pd_idx=stripe mod N`、symmetric `disk=(pd_idx+1+dd_idx) mod N`、asymmetric `disk=dd_idx>=pd_idx?dd_idx+1:dd_idx`，外加 `parity-first`/`parity-last` 伪 RAID4 固定校验盘。RAID6 采用 `lib/raid6` 的 GF(2⁸) 本原多项式 `0x11D`、生成元 `g=0x02`，`P=⊕D_i`、`Q=⊕g^i·D_i`，**系数按 stripe 内逻辑数据块序号 `i`**（与物理盘号无关），P/Q 作为相邻对一起轮转；RAID6 仅实现 symmetric 与固定校验盘布局，asymmetric 双校验轮转明确返回 `UnsupportedLayout`。
>
> **恢复语义**：RAID4/5 单盘失效、RAID6 任意双盘失效（含 P 或 Q 同时丢失）均从 P/Q 方程重建；目标成员**读失败**（坏扇区、不稳定盘）按「该 chunk 临时不可用」处理，计入不可用数并尝试 parity 重建，而不是返回错误字节或静默零填充；RAID0/JBOD 无冗余时读失败原样向上抛，绝不伪造数据。`VerifyStripe()` 逐扇区重算 P/Q，供上层校验猜测的几何参数；降级阵列返回 `Partial` 而非误报 `Ok`。
>
> **验证记录**：CTest #70 `VirtualRaid` 用**独立的正向布局生成器**（闭式公式映射，与产品的角色枚举不同路径）+ **独立的 GF 实现**（位移多项式乘，不复用产品的 exp/log 表）造组件盘，再逐扇区反向重组比对；GF 表另做 256×256 全组合乘法与除法交叉校验。覆盖 7 个级别 / 10 种布局组合、每个成员的单盘失效、RAID6 双盘（两数据盘 / 数据+校验 / 双校验）、非对齐读、严格越界、`dataOffset`、坏扇区回退、parity 篡改检出、嵌套 RAID50/60（含降级内层），以及 12 类配置拒绝。负向验证两重：错误 layout / chunk 必须**不**复现原卷；**源码级**把 `GfPow(k)` 篡改为 `k+1` 后测试如期 7 处 FAIL，恢复后全绿。A0/A1 共 10 个 CTest 全通过，`g++ -std=c++17 -Wall -Wextra` 零警告。
>
> **A1-4a 覆盖边界（不得夸大）**：所有验证均基于**内存合成阵列**，没有任何真实硬件/mdadm 阵列镜像；**没有自动布局探测**（盘序、chunk、轮转、起始偏移全部由调用方给 `RaidConfig`）；**没有** `mdadm` 超块 / DDF / RIS 解析、厂商专有布局、4Kn 成员、far/offset RAID10。配置错误会产出**错误字节且不会自动报错**——`VerifyStripe()` 与首扇区已知锚点（MBR/GPT/NTFS BPB）是必须的前置守卫。因此当前只能说「重组算术与降级恢复已验证自洽」，**不能说「已支持真实阵列恢复」**。
>
> **后续更新（A1-4b-a）**：上段描述的是 A1-4a 完成时的状态。`VirtualRaid` 本身仍不做自动探测，但 `disk/RaidProbe.h` 已提供 mdadm 元数据驱动的几何判定与 parity 复核，配置错误不再会「无人察觉地」进入扫描；仍需注意 **P/Q 校验无法确定盘序、parity 轮转与 chunk**，这些必须来自元数据或跨条带已知明文锚点。真实阵列兼容性仍属 A1-4b-b 待办，结论「不能说已支持真实阵列恢复」不变。
>
> **本阶段 GUI 边界**：按用户「不要改 GUI，先对齐功能」的要求，A1-4a 全部落在 `disk/` 与 `tests/`，未改 `pages/`、`controllers/`、`.ui`，也**未接入 `SectorReader` 扫描主路径**。后续接入时先走配置驱动的非 GUI 入口（会话/CLI 参数），任何 UI 接线须另行批准；在 A1-4b 的真实阵列金标完成前不得在界面暴露 RAID 入口。

> **A1-3a 实装说明（2026-11-07）**
>
> 两个 header-only 组件，全部落在 `disk/` 与 `tests/`，**零 GUI 改动、零设备句柄、零写方法**：`BitLockerCrypto.h` 实现 AES（S-box 由 GF(2⁸) 逆元推导，不用粘贴表）、SHA-256、AES-CCM（nonce 12 / tag 16 / 无 AAD / L=3）、AES-XTS 与 AES-CBC 扇区解密；`BitLockerVolume.h` 实现 `IVirtualDiskReader`，因此解密后的卷可直接作为虚拟盘源、RAID 成员或未来扫描源，无需改动任何下游。持有的是 `const IVirtualDiskReader*`，`Open()` 只读 `-FVE-FS-` 卷头与 FVE 元数据，`Read()` 只返回明文字节。
>
> **格式依据**（动手前联网核实）：libbde《BitLocker Drive Encryption (BDE) format》——Win7+ 卷头 `-FVE-FS-` @3、GUID @160、三份元数据偏移 @176/184/192；FVE 块头 v2（加密卷大小 @16、卷头扇区数 @28、三份偏移 @32/40/48、卷头偏移 @56）；FVE 元数据头（metadata_size @0、版本 @4、头长 @8、卷 GUID @16、nonce 计数 @32、加密方法 @36）；条目头 8 字节（size/type/value/version）；VMK 结构（保护类型 @26、属性 @28）；`datum_key_t` 密钥从 @12 开始；`datum_aes_ccm_t` 为「8 字节条目头 + 12 字节 nonce + 16 字节 tag + 密文」，密文从条目偏移 36 开始。密钥拉伸来自 `libbde_password.c` 的 88 字节结构 `{last[32], initial[32], salt[16], count LE64}`、0x100000 次 SHA-256；恢复密钥 48 位数字按 8 组 ÷11 转 8 个 u16 LE 后取 SHA-256；用户密码为 `SHA256(SHA256(UTF-16LE))`。扇区路径依据 `libbde_sector_data.c`：逻辑偏移 < 卷头大小时改读 `volume_header_offset + logical`，**解密用的物理偏移也随之为重定向后的值**；三份元数据区与重定位卷头物理区在解密视图中返回零；`physical >= encrypted_volume_size` 的区段按明文直通。XTS tweak = 物理扇区号 `physical / sector_size`，CBC IV = `E_FVEK(LE128(physical))`（dislocker `decrypt.c`）。AES-CCM 的 `A_0 = 02‖nonce‖000000`、`A_i = 02‖nonce‖i`（i 从 1）、`B0 = 3A‖nonce‖len_be24`、tag = CBC-MAC(明文) ⊕ S0，与 dislocker `decrypt.c` 的微软兼容实现一致。
>
> **支持范围**：Win7/8/10/11 固定盘（块头 v2）；加密方法 0x8002 / 0x8003 / 0x8004 / 0x8005；保护器 clear key、恢复密钥、用户密码、外部/启动密钥（32 字节 BEK 密钥材料）、以及直接提供裸 VMK 的取证旁路。3 份元数据副本按 nonce 计数择优，副本损坏自动回退并给出 warning。**明确拒绝**：Vista v1（卷头重建规则不同）、BitLocker To Go 混合卷（密文在独立 `COV *.ER` 文件）、Elephant Diffuser 0x8000/0x8001、TPM / TPM+PIN（仍会枚举报告）、非 512/4096 扇区、非 BitLocker 卷、三份元数据全毁——一律带原因返回，绝不退化成裸盘扫描产生误报。
>
> **验证记录**：CTest #71 `BitLockerCrypto` 用 FIPS-197 AES-128/192/256 加解密向量、FIPS-180-4 SHA-256 向量校验自研密码学实现，再用**独立 Python oracle**（`testdata/tools/Gen-BitLockerGolden.py`：纯 Python AES，S-box 由 GF 逆元计算；SHA-256 用 `hashlib`）产出的 `primitives.tsv` / `stretch.tsv` 逐项比对 XTS-128/256、CBC-128/256 扇区、CCM-128/256 × 三种负载长度（含 44/76 字节非整块，覆盖 CCM 尾部部分块）与 0x100000 次拉伸；每个正向向量都带密文/tag/nonce/sector 号篡改的负向断言。CTest #72 `BitLockerVolume` 对 10 个合成加密卷跑完整链路：6 个可解密卷 × 16 个逻辑读取 = **96 个 FNV-1a 指纹**，覆盖重定位卷头（NTFS `EB 52 90` / `NTFS    ` / `55 AA` 锚点）、跨扇区与非对齐读、三份元数据区零填充、尾部扇区、越界拒绝、错误凭据、CCM tag 篡改、三份元数据全毁、源文件逐字节不变（S1），并验证解密卷可直接作为 `VirtualDiskSource` 的只读源（组合性）。金标由 `manifest.json` 的 SHA-256 固化，`--verify-only` 可离线复核。两组合计 **455 项检查全通过**；负向验证两重：把 GF 归约多项式 `0x87` 改为 `0x89` 后 #71 如期 3 处 FAIL，把卷头重定位改成直读后 #72 如期 36 处 FAIL，恢复源码后全绿。A0/A1 共 **12 个 CTest 全通过**（Release，4.29 s），`g++ -std=c++17 -Wall -Wextra -Wpedantic` 零警告。
>
> **A1-3a 覆盖边界（不得夸大）**：全部验证基于**合成卷**，没有任何真实 Windows BitLocker 加密卷样本；`0x8004` 的 XTS 双密钥按「FVEK 密钥数据前 32 字节对半拆分」实现，真实卷是否存在「32 字节 FVEK 区 + 32 字节 TWEAK 区」的另一种排布尚未确认（A1-3b 必须用真实卷 + 首扇区锚点复核）；用户密码同时尝试「带/不带 NUL 终止」、恢复密钥同时尝试 u16 LE/BE 两种字节序，由 CCM tag 裁决，属于容错而非已验证的规范分支；BEK 只接受已提取出的 32 字节原始密钥材料，**不解析 `.BEK` 文件结构**；不支持 TPM、Vista、To Go、Diffuser、4Kn、部分解密/EOW 位图范围表；元数据区域大小按 `RoundUp(metadata_size, sector_size)` 推导并可用 `SetMetadataAreaSize()` 覆盖，真实卷若保留更大区域需人工指定。因此当前只能说「**只读解锁链路与解密算术已验证自洽**」，**不能说「已支持真实 BitLocker 卷恢复」**。
>
> **本阶段 GUI 边界**：按用户「不要改 GUI，先对齐功能」的要求，A1-3a 未改 `pages/`、`controllers/`、`.ui`，也**未接入 `SectorReader` 扫描主路径**。后续接入须先走配置驱动的非 GUI 入口（会话/CLI 参数），并在 A1-3b 真实卷金标完成前不得在界面暴露 BitLocker 解锁入口；凭据输入与密钥材料的生命周期（不落盘、不写日志、用完清零）须另行设计评审。

> **A1-5a 实装说明（2026-11-07）**
>
> 两个 header-only 组件落在 `fs/refs/`，**零 GUI 改动、零设备句柄、零写方法**。与 A1-3/A1-4 一致采用「`IRefsImage` 只读字节源 + 独立金标」的隔离路线：**没有**继承 `FileSystem`（该接口绑定 `SectorReader` 与 `Partition`，会把只读解析层拖进 Qt/Win32 链接依赖），而是暴露 `MemoryRefsImage` / `FileRefsImage` 两个只读源和 `Open()` / `ListDirectory(oid)` / `ParentChildLinks()` / `TranslateVlcn()` / `ReadFile()`。将来接扫描主路径时写一个薄适配层即可，见 A1-5b。
>
> **格式依据**（动手前联网核实）：ReFS Forensics Reference（forefst，3.4–3.14）的 VBR / Superblock / Checkpoint / Page References / Common Metadata Page Header / B+-Tree Node / Object Table / Parent-Child Table / Container Table / Directory Entries / Extent Descriptors / Checksum Architecture，以及 libfsrefs、refs_core、Microsoft ReFS B+-tree 设计说明。关键偏移：VBR `ReFS\0\0\0\0`@3、`FSRS`@0x10、几何 @0x20/@0x24、版本 @0x28/@0x29、校验选择 @0x2A、容器字节数 @0x40、ROR1+ADD 自校验 @0x16；SUPB 固定 LCN 30 + 尾部两备份、GUID @0x50、generation @0x68、CHKP LCN 表 @0xC0/@0xC8、自描述 @0xD0；CHKP 时钟 @0x60、flags @0x78（0x0200 = indirect root list）、页引用尺寸 @0x5C、root count @0x90、13 根偏移表 @0x94；页引用 48 B（CRC64）/72 B（SHA-256）/104 B（legacy，拒绝），四 LCN slot + cktype@0x22 + digest@0x28，摘要覆盖**整页所有有效 slot 的簇字节**；80 字节公共页头（volume signature = GUID 四字 XOR @0x0C、self LCN @0x20、TableIdLow @0x48）；MSB+ 根描述 @0x50、node header 在 `0x50+u32@0x50`、level @+0x0C、key-index start/count/end @+0x10/+0x14/+0x20 且 `(end-start)/4==count`、16 字节行头；容器表根 #7/#8 用**物理** LCN 引导，CPC @value+0x18、物理起始簇在 `value[len-16]`（160 B → 0x90，224 B → 0xD0），`PLCN = map[VLCN >> log2cpc] + (VLCN & (cpc-1))`；对象表 compact 记录页引用 @value+0x20；父子表 48 B 行的 ParentOID @+0x08 / ChildOID @+0x18；目录项 key type 0x30（kf 0x01 embedded / 0x02 index，UTF-16LE 名字 @key+4），embedded 值 times @0x28..0x40 / attrs @0x48 / size @0x58 / allocated @0x60 / sub-record @0xA8，index 值 size @0x38 / attrs @0x40（目录位 0x10000000）；extent 固定 24 B（VLCN / flags / record size / file VCN / padding / run length），重组前按 file VCN 排序。
>
> **校验与拒绝语义**：VBR 自校验失败 → `BadVbrChecksum`；`-FVE-FS-` → `BitLockerEncrypted`（提示先用 A1-3 解锁）；legacy v3.4–v3.9 → `UnsupportedVersion`；SUPB 三副本 / CHKP 双副本按「自校验通过 + 最高 generation / clock」择优，损坏自动回退；13 根页引用逐一做 Merkle 摘要校验，对象表（#0/#5）与容器表（#7/#8）为故障转移对；MSB+ 不变量不满足 → `BadNodeInvariant`；树深度上限 12 + 已访问 LCN 集合防环；容器表无法翻译 VLCN 或 extent 有空洞 → `HoleDetected`，**绝不静默零填充**；未知 checksum 类型 / 未知 marker / legacy 对象记录 → `UnsupportedVariant`。解析器持有 `const IRefsImage*`，无任何写方法。
>
> **验证记录**：CTest #73 `RefsCrypto` 用 CRC-32C/CRC-64/NVME 标准 check value（`0xE3069283` / `0xAE8B14860A799888`）、FIPS-180-4 SHA-256、FNV-1a 与 `primitives.tsv` 的 16 条独立 Python 向量逐项比对，含分块增量 CRC 与 VBR 篡改检测。CTest #74 `RefsVolume` 对 **13 个合成卷**跑完整链路：8 个正向 + 5 个拒绝，覆盖 CRC64 / SHA-256 页引用、direct / indirect root list、512e 4K / 64K 簇 / 4Kn 几何、两层 B+ 树、SUPB 主副本损坏回退、CHKP 高时钟副本损坏回退、根摘要篡改拒绝、节点不变量破坏拒绝、BitLocker / 非 ReFS / VBR 校验拒绝；每个正向卷比对 6 个目录项的 **48 条期望**（大小、分配、属性、三个 FILETIME、数据驻留类型、extent 列表、目录位），并对 **40 份文件内容**做全量 SHA-256 + FNV-1a 双指纹比对及跨 extent 非对齐窗口读、越界读、目录无数据拒绝、未知 OID 拒绝。内存源逐字节比对证明「解析器从不写源」，并验证容器表**单副本损坏走故障转移、双副本损坏被拒绝**。`manifest.json` 固化 SHA-256，`--verify-only` 可离线复核。两组合计 **1047 项检查全通过**；负向验证两重：把 CRC-64/NVME 多项式末位 `…B5→…B7` 后 #73 如期 2 处 FAIL / #74 如期 16 处 FAIL，把 VLCN→PLCN 翻译改成 `+1` 簇后 #74 如期 17 处 FAIL，恢复源码后全绿。A0/A1 共 **14 个 CTest 全通过**（Release，4.73 s），`g++ -std=c++17 -Wall -Wextra -Wpedantic` 零警告。
>
> **A1-5a 覆盖边界（不得夸大）**：全部验证基于**按公开偏移构造的合成卷**，没有任何真实 Windows ReFS 卷。三处必须使用**合成卷的扁平化金标布局**，不能当作真实卷规范：① SUPB/CHKP `LcnWithChecksum` 自校验按「一簇、descriptor 区清零、CRC32-C、摘要存 descriptor+0x28」实现，真实卷的精确清零范围待复核；② embedded `$DATA` sub-record 链用 0x30 字节扁平头（保留公开文档的 marker / descriptor / stream size / disk allocated / extent count 字段）后接 inline 字节或 24 字节 extent，真实卷的 inner-header + 嵌入式 B+ 树节点 framing 尚未解析；③ 目录名→子目录 OID 绑定未从公开资料确定，现通过 Parent-Child Table 暴露关系、由调用方以 OID 调 `ListDirectory()`。**未支持**：legacy v3.4–v3.9 对象表 / 104 B 页引用、split（kf=0x02）文件的 type-0x40 backing record、ADS、快照、压缩、integrity streams、硬链接、稀疏文件、删除项从 B+ 树 slack 恢复（**W16 已在合成金标上实现不可达残留目录页恢复，真实卷未验证，USN journal 仍未解析**）；4Kn 仅覆盖 VBR 几何，无 4Kn 物理盘样本。因此当前只能说「**只读结构解析、校验链与数据重组在合成语料上已验证自洽**」，**不能说「已支持真实 ReFS 卷恢复」**。
>
> **本阶段 GUI 边界**：按用户「不要改 GUI，先对齐功能」的要求，A1-5a 全部落在 `fs/refs/`、`tests/`、`testdata/`、`docs/`，未改 `pages/`、`controllers/`、`.ui`，也**未接入 `SectorReader` 扫描主路径、未复用 `FileSystem` 接口**。后续接线须先做配置驱动的非 GUI 入口，并在 A1-5b 真实卷金标完成前不得在界面暴露 ReFS 入口。

### A1-6 实装说明（流式 Aho-Corasick 多模式签名匹配器 · 按类型恢复参数化引擎底座）

**为什么要单独做一层**：`scan/SignatureDB::Match` 用线性 `memcmp` 逐条比对 514 条签名（O(patterns × bytes)），`RawScan.cpp` 是手写逐类型 `memcmp` carving；在 60+ TiB 全盘 RAW 雕刻场景下这是热点。本层把「逐类型线性扫描」换成「一次 O(bytes) 扫描的 Aho-Corasick 自动机」，把全部 header magic 编译成同一张转移表，省去每字节重复的模式外循环，并为后续「按类型恢复参数化」（对齐 / 最大尺寸 / 类型筛选 / 二级 sub-type 消歧 / footer 定界）提供统一结构。性质上属**并行轨**「签名多模式匹配（AC）」引擎项，是 A1-6「按类型恢复参数化」的底座。

- `scan/SignatureMatcher.h`（header-only 纯 C++17）：trie + BFS 失败链（fail link）+ 输出传播（output propagation）的流式 AC 自动机。`Build()` 建 trie 后 `BuildTransitions()` 预计算全转移表 `go[256]`（直接子节点 delta = child，缺失走 fail 链），首次线性匹配的真实复杂度从 O(P·N) 降到 O(N + P·D)（N 字节、P 模式、D 平均深度）。
- **流式重叠缓冲**：每窗重喂前窗末尾 `(maxDepth-1)` 字节，保证跨窗口的 magic 仍被精确命中一次（不多不少）。`maxDepth` 在建 trie 时按各模式 header 长度更新（初版漏算导致重叠长度恒 0，已修）。
- **延迟解析（deferred resolution）**：header 候选先登记为 pending，secondary sub-type（如 RIFF→WAVE 消歧）与 footer（如 PDF `%%EOF`、在 header 之后数 KB）在后续窗口字节到达时再校验，避免缓冲整流；`Finish()` 冲刷仍未决候选。footer 搜索用「可见窗口 ∩ 搜索区间」交集，跨窗口 OR 累积 `footOk`，只有整段搜索区间已可见或 `Finish()` 时才定稿（`covered || finalize`），解决了 footer 跨窗口定位与原版 `Finish()` 用空缓冲重算覆盖 `true` 的 bug。
- 结构：`MatchPattern`（extension / header / headerOffset / secondary / secondaryOffset / footer / maxSize / priority）、`SignatureHit`（patternId / extension / fileStart / magicOffset / headerLength / secondaryOk / footerOk / priority）、`MatchOptions`（alignment / maxSize / allowedExtensions / includeFooterSearch）。`#ifdef SIGNATUREMATCHER_WITH_SIGDB` 提供从 `SignatureDB` 读取的适配器 `BuildMatcherFromSignatureDB`（扩展名透传；DB 侧 `GetAll()` 已含字段）。
- **零依赖**：无 Qt、无 Win32、无 I/O；不接 `RawScan` 主路径、不接 `SectorReader` 设备入口、不碰 `pages/` `controllers/` `.ui`，符合「先对齐功能、零 GUI 改动」。

**验证记录**：独立 Python oracle `testdata/tools/Gen-SignatureMatcherGolden.py` 用朴素逐字节逐模式扫描算期望（与 AC 实现走不同代码路径交叉验证），生成 64 KiB 确定性填充镜像 + `cases.tsv` / `negative.tsv`（由期望命中间隙 + 16 字节 margin 自动派生，规避确定性填充偶含 magic 的误报）/ `streaming.tsv` / `manifest.json`（SHA-256 固化）/ `expectations.json`，8 类 CATALOG（jpg / png / gif / zip / pdf(+EOF footer) / riff(+WAVE secondary) / mp4(headerOffset=4) / cr2）。CTest #77 `SignatureMatcher` 覆盖：AC 单元属性（重叠模式 `{ab,bc}` → 5 状态、`abcabc` → 4 命中 `[0,1,3,4]`）；金标单缓冲 vs 独立 oracle 9 命中逐项相等；负向区零误报；**流式 512 B 窗口 == 单缓冲**（含 `Finish()` 冲刷 footer）；篡改 magic 字节丢 1 命中；type-filter 仅 jpg；alignment=512 保留 png@2048 / mp4@4096 / cr2@8192 三命中。**源码级负向验证**：篡改 `m_nodes[r].go[c] = nxt` 为 `= 0` 后测试如期 6 处 FAIL，恢复源码后全绿。本轮 Release 下 **17/17 CTest 全通过**（4.73 s），`g++ -std=c++17 -Wall -Wextra -Wpedantic` 零警告；测试二进制在 `tests/CMakeLists.txt` 注册为 `#77 SignatureMatcher`。

**覆盖边界（不得夸大）**：① 引擎底座与离线金标已就绪，**并已接入 `RawScan` 主路径**（A1-6：`ScanBatch` 流式 AC 批扫描替换线性 `memcmp`；CTest `RawScanAC` / `DeepScanFallback` / `RawScanParallel` 回归通过），A1-10 已把并行路径接入生产阶段 3（默认串行、配置开启，CTest `RawScanParallelWiring`）；A1-11 已量化**合成镜像吞吐**（见 `PERFORMANCE_BASELINE.md §8`）；但**真实全盘 / 真实磁盘端到端提速尚无实测**；② `SignatureDB::LoadFromFile` 的 Qt 依赖已于 **A1-9** 解除（`std::ifstream` + 零依赖 `scan/SignatureJson.h`），非 GUI / headless 构建同样可加载自定义 JSON 签名（CTest `SignatureDBLoad` 90 项）；运行时内建签名实测 **513** 条（含 `UiTypeSignatureSupplement`），产品文档的 **514** 为 `UiTypeCoverage` 口径，勿混用；③ 金标为 64 KiB 合成镜像，未量化真实磁盘镜像（含碎片化、重叠 magic、坏扇区零填充）的误报 / 漏报率；④ secondary / footer 仅覆盖 CATALOG 内 2 类（riff / pdf），其余类型无 secondary/footer 约束；⑤ 自动机按字节构建，不支持位级 / 变长 magic 与正则式签名。因此可以说「**多模式流式匹配引擎已验证自洽并接入 RAW 扫描主路径**」，**不能说「真实全盘提速已量化 / 真实镜像误报率已量化」**。

> **本阶段 GUI 边界**：按用户「不要改 GUI，先对齐功能」的要求，A1-6 ~ A1-9 全部落在 `scan/` / `tests/` / `testdata/`，未改 `pages/`、`controllers/`、`.ui`；**A1-10** 为把并行 RAW 扫描接入生产链路，在 `controllers/RecoveryScanWorker.cpp` 增加了一处 `QSettings` 读取（`recover/rawScanThreads`，默认 1）。仍**零 GUI 改动**：未改 `pages/`、任何 `.ui` 或对话框，未新增 UI 控件。已接入主路径：A1-6 流式 AC 替换线性 `memcmp`、A1-7 损坏 NTFS 回退 RAW 雕刻、A1-8 并行 RAW 扫描分块+合并、A1-10 配置驱动接线。

### A1-10 实装说明（并行 RAW 扫描接入生产链路）

**问题**：A1-8 交付的 `RawScan::Run(reader, partition, numThreads)` 仅被 CTest 调用；`UnifiedVolumeScanner` 阶段 3 → `CarveModuleService` → `RawScan::Run(reader, partition)` 始终串行。接线时另发现两处正确性缺陷：① 并行 worker 硬编码 `SignatureDB localDb;`（513 条内建签名），**忽略** `CarveModuleService` 注入的自定义 / 扩展名过滤数据库；② `CarveModuleService::targetExtensions` 从未真正过滤——`filteredDb` 由默认 ctor 构造（已含内建全集）后再 `Add` 命中项。

**实现（零 GUI 改动）**：
- `SignatureDB` 新增 `explicit SignatureDB(const std::vector<FileSignature>&)`（精确签名集，不加载内建）；并行 worker 由主线程快照 `m_db->GetAll()` 后各自构造 `localDb(sigSnapshot)`，保证与串行路径签名集逐项一致（`ScanBatch` 有可变 scratch，不能共享单实例）。
- `RawScan` 新增 `SetThreadCount/GetThreadCount`（默认 **1 = 串行**，行为零变化）；2 参 `Run` 在 `>1` 时委派 3 参并行重载；并行分支补 `cancelled` 传播；公开 3 参 API 线程数硬上限 64。
- `CarveConfig::numThreads`（默认 1；`0`=auto，`N`=显式，夹紧到 `[1, min(32, 硬件并发)]`）、`UnifiedScanConfig::rawScanThreads`（默认 1）、可测的 inline `MakeCarveConfig()` 映射；`RecoveryScanWorker` 读 `QSettings recover/rawScanThreads`（默认 1，无 UI 控件）。
- 修复 `targetExtensions`：改用精确集构造器，只保留命中扩展名的签名；文件 / 进度回调始终在调用线程发射（worker 不触碰回调）。

**验证**：CTest `RawScanParallelWiring` 26 项全绿（配置默认 / 映射、精确集构造器、自定义库下 parallel==serial、`targetExtensions={png}` 真实过滤、回调零跨线程、取消传播、源码级接线守卫）。**两轮源码级负向验证**：① 把 `SetThreadCount(ResolveThreadCount(config.numThreads))` 改成 `SetThreadCount(1)` → 接线守卫 FAIL（EXIT=1）；② 把 worker 改回 `SignatureDB localDb;` → 自定义库用例 serial=5 / parallel=63，2 处 FAIL（EXIT=1）；恢复源码后全绿。定向回归 **10/10**。三个未进入 headless 构建的生产文件另做 `g++ -fsyntax-only` 校验：`UnifiedVolumeScanner.cpp`、`RecoveryScanWorker.cpp`（含 Qt 头）0 错误，`CarveModuleService.cpp` 由新 CTest 直接编译链接。

**边界（不得夸大）**：默认仍是**串行**，并行须显式配置才生效；`pages/` 与 `.ui` 未改，设置项无 UI 控件；非镜像源（设备 / 虚拟盘）无法克隆 reader，自动回退单线程；**合成镜像吞吐已于 A1-11 量化**，但真实磁盘 / 真实卷端到端提速与误报率仍未量化；唯一已知串行 / 并行分叉点（嵌套 jpeg 跨 chunk 的 cursor 跳过）未消除。

### A1-11 实装说明（并行 RAW 扫描吞吐基线）

**交付**：`tools/bench_rawscan_parallel.cpp` + CMake 选项 `PHOENIX_BUILD_BENCH`（默认 **OFF**，**不注册 CTest**，避免计时断言抖动）。支持 `--size-mib / --repeat / --threads / --csv / --image / --keep / --via-service`；`--via-service` 走 A1-10 的生产配置链，默认直连 `RawScan::Run`。

**方法学**：256 MiB 合成镜像（常量 `0x5A` 填充 + 每 256 KiB 一个 png/jpg/gif header-only magic，共 1023 个）；写盘后 `_commit` 强制回写 + 整文件预读 + 2 次未计时串行 warm-up；每线程数重复 3 次取中位数，且**每轮旋转线程数顺序**消除先后偏差；每次结果与 warm-up 串行基线逐偏移比对。首版曾因刚写入的 256 MiB 后台回写使前几个配置偏冷，得到 1T=2246 ms / 8T=266 ms 的 **8.44x 虚高**结果，已修正并复测稳定。

**实测（MinGW g++ 7.3.0 / Release `-O3` / 32 逻辑核）**：直连 1T 1352.6 ms → 2T 1.88x → 4T 3.25x → 8T **4.98x**（943 MB/s）；生产配置链 1T 1357.1 ms → 8T **4.94x**（932 MB/s），两入口差异 < 2%，全配置文件集一致。详见 `PERFORMANCE_BASELINE.md §8` 与 `docs/reports/rawscan_parallel_bench_2026-11-08*.{txt,csv}`。

**边界**：合成低熵填充（无随机假阳性）、OS 页缓存普通文件（非真实物理盘 I/O）、magic 严格递增（规避嵌套 jpeg 分叉）、仅 RAW 雕刻流水线（非 GUI 端到端）、CPU/RAM/磁盘型号未采集故不可跨机比较、不作发版门禁。真实 10–50 GB 卷 / 碎片化 / 坏扇区 / 峰值 RSS 仍未量化。

### A1-4b-a 实装说明（自动布局探测 + 配置驱动组合源）

**为什么要单独做一层**：A1-4a 的 `VirtualRaid` 忠实按给定 `RaidConfig` 出字节，配置错了会安静地产出错误数据——这是恢复场景最危险的失效模式。本层把「猜配置」变成「有证据才接受」。

- `disk/RaidProbe.h`（header-only 纯 C++17）：解析 mdadm **0.90**（4 KiB 超块，位于 `(设备大小 & ~64KiB) - 64KiB`，host-order 字累加校验和）与 **1.0 / 1.1 / 1.2**（256 B 固定头 + `dev_roles[]`，LE 字累加校验和；1.1 在 0、1.2 在 4 KiB、1.0 在末尾 8–12 KiB 区间）。字段按 `md_p.h` / `super1.c` 公开布局取：level / layout / chunk / raid_disks / data_offset / `dev_roles[dev_number]` / set_uuid / events。
- 元数据路径要求**所有成员**超块校验和有效、set_uuid 与几何一致、盘角色不重复且覆盖完整；随后用 `VirtualRaid::VerifyStripe()` 抽样核对磁盘上的 parity，**元数据与实际 parity 不符即 `VerificationFailed`**，绝不静默信任元数据。降级阵列按缺失角色填 `nullptr` 并登记 `cfg.missing`。
- **受限盲探测**只覆盖有 parity 的级别，带候选预算与排列预算；候选打分 = 已验证条带 + 卷起始通用锚点 + 卷内已知明文锚点。全部同分返回 `Ambiguous`、超预算返回 `BudgetExceeded`、无证据返回 `NoLayoutFound`。
- 两个由金标实测逼出来的正确性结论（已写入代码注释）：① **P/Q 逐扇区成立，与 chunk 大小无关**，所以 chunk 必须由配置给出候选集 + 卷内明文锚点判定；② **单奇偶 XOR 是对称的**（`A⊕B=P` 蕴含 `A⊕P=B`），任意指定一个成员当 P 都能通过 P 校验，因此 parity 校验既不能确定盘序、也不能确定 parity 轮转，**必须**有跨条带的已知明文锚点。少于 2 个锚点时盲探测直接拒绝。
- `disk/ScanSourceChain.h`：严格的行式 `key=value` 配置（未知键 / 重复键 / 坏值 / 缺 `members` 都是硬错误），注入式 `IVirtualDiskReader` 字节源按名字组装为「容器解包 → RAID 探测/重组 → BitLocker 只读解锁 → ReFS `IRefsImage` 桥接」，输出统一 `IVirtualDiskReader`。探测失败**不会**降级为「只扫第一块盘」。`Report()` / `Describe()` 中凭据一律脱敏。

**验证记录**：CTest #75 `RaidProbe`（228 项）、#76 `ScanSourceChain`（185 项）；独立 Python oracle `testdata/tools/Gen-RaidProbeGolden.py` 生成 16 个用例 / 20 条逐层 FNV-1a + SHA-256 期望 / 6 条原语向量 / 58 文件 SHA-256 manifest（`--verify-only`）。覆盖 0.90/1.0/1.1/1.2、RAID1/5/6、左右对称/非对称布局、盘序打乱、降级缺盘、VHD fixed 容器包装、盲探测成功与歧义/无证据/超预算拒绝、UUID 冲突、部分校验和损坏、parity 与元数据矛盾、reshape 特性拒绝、盘角色冲突。BitLocker 与 ReFS 两段的期望分别复用 `bitlocker-golden/sectors.tsv` 与 `refs-golden/files.tsv`（既有独立金标），验证组合链确实把解密层与文件系统层串通。**两重源码级负向验证**：关闭 mdadm-1.x 校验和 → #75 如期 2 处 FAIL（`md12_partial_csum` 被错误接受）；关闭链路的探测失败守卫 → #76 如期 1 处 FAIL；恢复后全绿。Release 下 A0/A1 子集 **16/16 通过**，`g++ -std=c++17 -Wall -Wextra -Wpedantic` 零警告。

**覆盖边界**：全部为**合成**语料，没有任何真实 mdadm / 硬件 RAID 阵列；未做 DDF、RIS、厂商布局、reshape、bitmap、PPL、journal、bblog、4Kn、事件数冲突仲裁；0.90 与 1.x 均按小端目标解析；盲探测不做无 parity 级别（RAID0/1/JBOD/10）的猜测，也不对全零条带计分。`SourceChain` 目前只接受**注入式字节源**，尚未接 `SectorReader` 的设备主路径，凭据生命周期（不落盘、不写日志、用完清零）在接入真实设备前须另行评审。

> **本阶段 GUI 边界**：按用户「不要改 GUI，先对齐功能」的要求，A1-4b-a 全部落在 `disk/`、`tests/`、`testdata/`、`docs/`，未改 `pages/`、`controllers/`、`.ui`。A1-2 已产生的 UI 接线保持原样未回退。真实阵列金标（A1-4b-b）完成前不得在界面暴露 RAID 入口。

### A2 — 写目标盘

| 项 | 说明 |
|----|------|
| A2-1 | 分区/磁盘镜像还原（含单分区还原），需 `WriteGuard` 授权 `WriteTargetDisk` |
| A2-2 | 磁盘/分区克隆三种模式（按文件 / 按扇区 / 按文件系统布局） |
| A2-3 | 系统迁移到 HDD/SSD/USB |
| A2-4 | 镜像文件浏览与文件提取（只读部分可提前到 A1） |

### A3 — 写源盘元数据

| 项 | 说明 |
|----|------|
| A3-1 | 分区 CRUD、格式化（NTFS/FAT32/exFAT/EXT4）、快速分区 |
| A3-2 | 无损调整 / 扩展 / 拆分 / 合并 / 隐藏 / 盘符分配 |
| A3-3 | MBR↔GPT 无损转换、4K 对齐检测 |
| A3-4 | 分区表备份与还原（事务化） |

### A4 — 高危不可逆

| 项 | 说明 |
|----|------|
| A4-1 | 丢失分区搜索结果**写回分区表**（现 `PartitionRecoveryService` 仅只读列出候选） |
| A4-2 | 重建 MBR、分区表错误修复、Windows 引导修复与转换 |
| A4-3 | 永久删除文件 / 擦除扇区 / 擦除空闲空间（含擦除标准） |
| A4-4 | 扇区编辑器写入（现仅前 100 字节只读 hex dump） |
| A4-5 | 坏道主动检测与修复（现仅读失败零填充记录） |

### A5 — 生态

| 项 | 说明 |
|----|------|
| A5-1 | WinPE 启动盘制作、UEFI 启动项管理 |
| A5-2 | 多语言（当前界面文本全部硬编码中文，无 `QTranslator`） |
| A5-3 | 只读 CLI（`main.cpp` 现无参数解析） |
| A5-4 | Excel 报表、目录结构导出 HTML |
| A5-5 | 硬件兼容矩阵：4Kn、RAID 控制器、NVMe、USB 桥、存储池、动态磁盘 |

## 5. 与既有编号的映射

| 既有编号 | 去向 |
|----------|------|
| `PRODUCT_SCOPE §8` P0/P1/P2（三主线） | 并入**并行轨**，仍受 `§5.5` 发版门禁约束 |
| 本文上文 P1 SQLite WAL/journal | 并行轨 / A1 |
| 本文上文 P2 exFAT 碎片、P3 恢复后自动 SQLite 修复 | 并行轨 |
| `PERFORMANCE_BASELINE §6.1` Case 01–05 | 各阶段验收统一使用；竞品列固定为 DiskGenius `6.2.0.1829` |
| `ROADMAP_RECOVERY.md` 原「计划中」表 | 保留，作为恢复算法子轨 |

## 6. 风险红线

1. A0 未验收，不合入任何打开设备写句柄的代码；A1 全部能力必须保持扫描源只读。
2. A3/A4 的任何操作不得在含真实数据的盘上开发自测，只能在隔离测试机 + 可回滚镜像上进行。
3. 每次新增写能力，必须同时提交对应的回滚用例与前后哈希比对结果。
4. 对齐的是**功能清单**；恢复算法效果与硬件兼容性属于长期打磨项，不得以「功能已列」对外宣称等效。

## 7. 推进顺序（当前执行位置）

```
[完成] A0-1 定位文档 → [完成] A0-2 写授权骨架 → [完成] A0-3-a 授权凭证/事务日志/回滚/写句柄骨架
                                    ↓
                        [待建设] A0-3-b 物理隔离测试环境（与 A2 同步）
                                    ↓
                        A1（可与 A0-3 并行，只读、低风险）
                          └─ [完成] A1-1a 基础格式 → [完成] A1-1b 扩展格式 → [完成] A1-1c 真实镜像金标
                                                                    → [完成] A1-2 扫描主路径/UI 接线
                                                                    → [完成] A1-4a 虚拟 RAID 只读重组（纯 disk 层，未接 UI）
                                                                    → [完成] A1-3a BitLocker 只读解锁（纯 disk 层，未接 UI）
                                                                    → [完成] A1-5a ReFS 只读解析引擎（纯 fs/refs 层，未接 UI，合成卷金标）
                                                                    → [完成] A1-4b-a mdadm 超块探测 + 配置驱动组合只读源（零 GUI）
                                                                    → [并行轨·已完成] 流式 Aho-Corasick 多模式签名匹配器（按类型恢复参数化引擎底座，零 GUI，CTest #77）
                                                                    → [待办] A1-1d 样本补缺 / A1-3b 真实 BitLocker 卷 / A1-4b-b 真实阵列与设备主路径接线 / A1-5b 真实 ReFS 卷与接口接线 / [已完成·W4 校正] A1-6 AC 匹配器已接 `RawScan` 主路径、A1-7 损坏 NTFS 回退 RAW 雕刻（`DeepScanFallback`）；真实镜像误报/漏报率量化仍属待办
                                    ↓
                              A2 → A3 → A4 → A5
并行轨：引擎深度 + 验收基线（P0 门禁）持续推进，不设阻塞
```

### 当前批次遗留项（不阻塞 A1 开工）

| 项 | 说明 |
|----|------|
| A0-3-a | ✅ 已完成：`WriteGrant` 能力令牌、`WriteTransaction.h`（日志/回滚/断电恢复）、`DeviceWriter.h`（类型级写句柄骨架）、4 个 CTest 用例 |
| A0-3-b | 隔离测试环境：物理测试机、可回滚镜像回放、写前后全盘哈希比对；真实 `WriteSectors()` 写入实现须待本项就绪且 A0 整体验收后合入 |
| A1-1b | ✅ 已完成映射部分；**剩余**：VMDK flat/split 多文件挂载需要扩展 `IVirtualDiskReader` 为多文件源，仍属未支持子型 |
| A1-1c | ✅ 已完成：10 个真实镜像 + 独立 Python 参考实现 + CTest #68 `VirtualDiskGolden`（69 扇区指纹 / 8 越界检查全匹配，含负向验证）；**QCOW2/VDI/普通 VMDK 真实样本仍缺，转 A1-1d** |
| A1-1d | **新增待办**：补 QCOW2、VDI、普通 monolithicSparse VMDK 的真实镜像与含数据的 VHD dynamic 样本。首选 `log2timeline/dfvfs` `test_data/` 的 `ext2.{raw,vmdk,qcow2,vhd,vhdx}` 同内容多容器组（`.raw` 可直接做逐扇区 oracle）；本轮因沙箱网络/API 配额未取到，需在网络畅通环境重跑 |
| A1-2 | ✅ 已完成接线：`VirtualDiskSectorSource` 桥接 + `SectorReader::OpenVirtualImage()` + 扩展名路由 + UI 过滤器；CTest #69 端到端覆盖合成与 6 个真实镜像。**剩余**：GUI 手工端到端用例（选盘→扫描→预览→恢复）未执行；镜像内文件浏览/提取归 A2-4 |
| A1-2 遗留 | 真实 VMDK（普通 monolithicSparse）/ QCOW2 / VDI 的 UI 入口虽已可点选，但成功读取路径只有合成 fixture，须待 A1-1d 补真实样本后才能宣称支持；VMDK flat/split 多文件挂载需扩展 `IVirtualDiskReader` 为多文件源；`SectorReader_unix.cpp` 的虚拟分流与 Windows 版对称改动，但当前环境无 Linux/macOS 构建，**尚未在 Unix 上编译验证** |
| A1-1a | ✅ 完成并验证：RAW（含残尾扇区）、VHD fixed（含截断拒绝）、VHD dynamic（位图分配/稀疏块/BAT 越界/差分盘拒绝）、格式检出拒绝、边界与只读文件读取 |
| A1-1b | ✅ 完成并验证：VHDX / VMDK monolithicSparse / QCOW2 v2-v3 / VDI 四类映射 + 16 种异常子型拒绝，CTest `VirtualDiskExtended` |
| A1-4a | ✅ 已完成纯 `disk/` 层：`VirtualRaid.h` + CTest #70（独立 oracle、GF 全组合交叉校验、7 级别 / 10 布局、RAID6 双盘、坏扇区回退、嵌套 RAID50/60、源码级负向验证）。**未接 `SectorReader`、未接 UI**（按用户要求） |
| A1-4b-a | **已完成**：mdadm 0.90 / 1.0 / 1.1 / 1.2 超块解析与校验和、几何 parity 复核、配置错误显式拒绝、受限盲探测（歧义/超预算/无证据拒绝）、严格配置解析与注入式组合只读源。CTest #75 / #76，16 个合成用例 + 20 条逐层指纹 + 两重源码级负向验证；仍**未接 `SectorReader` 设备主路径**，详见「A1-4b-a 实装说明」 |
| A1-4b-b | **新增待办**（由原 A1-4b 收窄）：SNIA DDF / Intel RIS / 厂商专有布局、mdadm reshape / bitmap / PPL / journal / bblog、4Kn、真实阵列金标样本、事件数冲突仲裁；以及把 `SourceChain` 接到 `SectorReader` 的非 GUI 设备入口与凭据生命周期评审。真实阵列金标完成前不得在界面暴露 RAID 入口 |
| A1-3a | ✅ 已完成纯 `disk/` 层：`BitLockerCrypto.h` + `BitLockerVolume.h` + CTest #71/#72（FIPS 向量、独立 Python AES/hashlib oracle、10 个合成卷、96 个扇区指纹、两重源码级负向验证）。**未接 `SectorReader`、未接 UI**（按用户要求） |
| A1-3b | **新增待办**：真实 Windows BitLocker 加密卷金标样本（按 0x8002/0x8003/0x8004/0x8005 与各类保护器分组，含 4Kn、部分解密/EOW）、真实卷上 XTS 双密钥布局复核、`.BEK` 文件结构解析、TPM / TPM+PIN、Vista v1、BitLocker To Go 混合卷、Elephant Diffuser 0x8000/0x8001；以及配置驱动的**非 GUI** 扫描入口与凭据生命周期设计。真实卷金标完成前不得宣称「已支持真实 BitLocker 卷恢复」，也不得在 UI 暴露解锁入口 |
| A1-5a | ✅ 已完成纯 `fs/refs/` 层：`RefsCrypto.h` + `RefsVolume.h` + CTest #73/#74（标准 CRC / FIPS SHA-256 向量、独立纯 Python oracle、13 个合成卷、48 条文件期望、40 份内容双指纹、两重源码级负向验证、14 个 CTest 全绿）。**合成卷，非真实 Windows ReFS 卷；未复用 `FileSystem` 接口、未接 `SectorReader`、未接 UI**（按用户要求） |
| A1-5b | **新增待办**：真实 Windows ReFS 卷金标样本（v3.4–v3.14 / 升级卷 / 4Kn / 64K 簇 / SHA-256）、legacy 对象表与 104 B 页引用、embedded `$DATA` 完整 inner-header + 嵌入式 B+ 树节点 framing、split 文件 type-0x40 backing record、ADS / 快照 / 压缩 / integrity streams / 硬链接、目录名→子目录 OID 绑定、SUPB/CHKP 自校验精确清零范围复核；以及 `FileSystem` 适配层与配置驱动的非 GUI 扫描入口。真实卷金标完成前不得宣称「已支持真实 ReFS 卷恢复」，也不得在 UI 暴露 ReFS 入口 |
| A1-6 | **签名多模式匹配（AC）引擎底座已完成**：`scan/SignatureMatcher.h`（流式 Aho-Corasick，零 Qt/Win32/I/O）+ 独立 Python oracle `Gen-SignatureMatcherGolden.py` + CTest #77 `SignatureMatcher`（17/17 全绿，含源码级负向验证）。**A1-6 已接 `RawScan` 主路径**（`ScanBatch` 流式 AC 批扫描替换线性 `memcmp`）；**A1-7** `DeepScan` 智能加载损坏 NTFS 回退 RAW 雕刻（CTest `DeepScanFallback`）；**A1-8** 并行 RAW 扫描（CTest `RawScanParallel`）；**A1-9** `SignatureDB::LoadFromFile` Qt 解耦（零依赖 `scan/SignatureJson.h`，非 GUI 可用，CTest `SignatureDBLoad` 90 项）。**待办**：真实镜像误报/漏报率与真实全盘端到端提速量化；不得宣称「已量化真实 RAW 雕刻提速」 |
| 并行轨 | 签名多模式匹配（AC）：`scan/SignatureMatcher.h` **已实现并接 `RawScan` 主路径**（`ScanBatch` 流式批扫描替换线性 `memcmp`）；**A1-7** `DeepScan` 损坏 NTFS 回退 RAW 雕刻已落地（CTest `DeepScanFallback`）；**A1-8** 并行 RAW 扫描已落地（`RawScan::Run(reader,partition,numThreads)` 跨 N 线程分块+512 重叠+按 rawOffset 合并，CTest `RawScanParallel` 对 N=2/4/8 与串行逐偏移一致）；**A1-9** `SignatureDB::LoadFromFile` Qt 解耦已落地（`std::ifstream` + 零依赖 `scan/SignatureJson.h`，headless 构建可加载自定义 JSON 签名；CTest `SignatureDBLoad`）；真实全盘提速基准仍待量化 |
| 并行轨 | 运行时内建签名实测 **513** 条（`SignatureDB` 构造末尾 `m_sigs.size()`，含 `UiTypeSignatureSupplement`）；PRODUCT_SCOPE 中的 **514** 为 `UiTypeCoverage` 口径，二者统计对象不同，勿混用 |
| 并行轨 | 非 MSVC 工具链兼容：MinGW g++ 7.3 下 `test_utils_unique_path.cpp` 因 `<filesystem>` 不可用、`FormatPayloadScore` 相关目标编译失败；项目主力编译器为 MSVC 2019+，该问题为既有工具链差异，非本次改动引入 |
| 并行轨 | `ARCHITECTURE.md` 的「I/O 层只读」分层约束需随 A2 启动同步更新 |
| 并行轨 | **A1-10 已落地**：并行 RAW 扫描经 `UnifiedScanConfig.rawScanThreads` → `MakeCarveConfig` → `CarveConfig.numThreads` → `RawScan::SetThreadCount` 接入生产阶段 3；默认 1（串行、零行为变化），`QSettings recover/rawScanThreads` 可显式开启（`0`=auto）；CTest `RawScanParallelWiring` 26 项 + 两轮源码级负向验证 |
| 并行轨 | A1-10 顺带修复两处既有缺陷：`CarveModuleService::targetExtensions` 从未真正过滤；并行 worker 忽略注入签名库（现按 `m_db->GetAll()` 快照精确克隆）。A1-11 已量化合成镜像吞吐（8T 约 4.9x，见 `PERFORMANCE_BASELINE.md §8`）；真实磁盘 / 真实卷提速与误报率量化、嵌套 jpeg 跨 chunk 分叉消除仍属待办 |

## 8. P0 批次「文件恢复优先 / Windows + Linux FS」实装记录（W1–W5，2026-11-08，零 GUI 改动）

| 项 | 交付 | 验证 |
|----|------|------|
| **W1 可信回归基线** | 新增 `build-gcc14`（MSYS2 GCC 14.2.0，含 `<filesystem>`）绕开 `build-ac` 的 MinGW g++ 7.3 工具链缺口；修复 `scan/ScanSerializer.cpp`、`crash/CrashHandler.cpp` 的 `std::ofstream(std::wstring)` MSVC 扩展、`dftt_test_md5.h` RAII 化、4 个 carving 金标测试的 `goto done` 跨初始化、`RecoverEngine.cpp` 去 `<filesystem>`；headless 构建不再链接 MSVC Qt import libs | 全量 `mingw32-make -k -j6` → `BUILD_EXIT=0`；最终 GCC14 回归 **33/33（84.92 s）** + `FileCarving_Graphic_All` 单独 1/1（245.39 s，需 `--timeout 900`）+ `DFR_Windows_Batch` 12/12 PASS；GCC7 定向 3/3 + `FormattedVolumeRecovery` 22/22。`build-ac` 与 `build-gcc14` 双工具链均编译 W2/W3/W5 新测试 |
| **W2 exFAT 自动探测链** | `FileSystemModuleService.cpp` Windows 分支探测顺序 `NTFS → exFAT → FAT → ext2/3/4`；此前 exFAT 解析器虽已实现但 `fsType` 为空时永远不可达 | CTest `ExfatProbeDispatch` 23/23（合成 boot + 真实 `dfr-01-xfat.dd` @ sector 2048 + 源码级接线守卫）；GCC7 / GCC14 双绿；断开 exFAT probe 的负向验证 3 项 FAIL |
| **W3 场景矩阵与恢复字节门禁** | `testdata/manifests/expected_results.csv`；CTest `ExfatRecoverBytes`——独立手写 exFAT oracle（自解析 boot + 已删根目录项，cluster 13 / offset 1224704 / Python hashlib MD5），校验 `RecoverEngine` 写出的 4396 字节逐字节相等、MD5 `6650a82d6499770bf7eef8045a014b7d`、整镜像 FNV-1a 前后不变；CTest `FormattedVolumeRecovery`——4 MiB 合成「快速格式化」卷（全新 NTFS boot + 清零 128 KiB 元数据区 + 5 个残留载荷），校验 `DeepScan` RAW 回退的偏移命中**与雕刻字节前缀**、空白格式化卷 0 文件、整镜像哈希不变 | `ExfatRecoverBytes` 30/30、`FormattedVolumeRecovery` 22/22，双工具链全绿；两轮 exFAT + 一轮 DeepScan 源码级负向验证 FAIL 后恢复。**exFAT 门禁查出并修复 CAR-003**（见下）。`FormattedVolumeRecovery` 报告 59 文件 / 5 植入 ≈ 13.5 FP/MiB，与 W5 的 14.38 FP/MiB 互相印证 |
| **W4 文档口径校正** | `FRAGMENT_QUALITY_CURVE.md` / `FRAGMENT_STITCH_MODULE.md` / `ROADMAP` / `PRODUCT_SCOPE` / `TEST_PLAN` / `EXT4_VARIANTS.md` / `test_ntfs_fragment_stitch_dispatch.cpp`：NTFS 拼接非 stub、exFAT 复用 `TryImproveExfatNode`、`CFReDS_EXT4_DFR01` 已存在、ext4 变体矩阵属规划非交付 | 文档审阅，无代码行为变更 |
| **W5 误报率量化** | CTest `RawScanFalsePositive`（8 MiB seeded 随机噪声 + 16 植入 magic），`PERFORMANCE_BASELINE.md §9` | 8/8 全绿：Case A 115 FP = 14.38 FP/MiB；Case B TP=16 FP=109 FN=0，precision 12.80% / recall 100%；`docs/reports/rawscan_false_positive_2026-11-08.txt` |
| **W6 独立 ext2/ext3 金标** | `tests/test_ext2_ext3_legacy_golden.cpp` / CTest `Ext2Ext3LegacyGolden`；ext2 删除文件原始名经 dirent slack 扫描恢复 | 69/69 全绿；ext2 的 4 个正确识别为目录；节点总数仍 45（orphan 阶段不重复生成 `inode_XXXX`，无副作用）；源码级负向验证（关 slack 扫描 → 5 项 FAIL）恢复全绿。改 `fs/ext4/Ext4Parser.cpp` |
| **W7 多簇 / 碎片化 exFAT 恢复字节门禁** | `tests/test_exfat_multicluster_bytes.cpp` / CTest `ExfatMulticlusterBytes`；4 个真实镜像（dfr-02/03/05/12）共 7 个多簇已删文件 | 109/109 全绿；dfr-12 的 `FAT[first]` 刻意指向别处（26/31/33）被 W3 `NoFatChain` 修复正确忽略（多簇场景仍成立）；两轮源码级负向验证各按预期 FAIL（① 删除树里仍尊重 NoFatChain → 5 项；② 连续流放开 FAT 拼接器 → 3 项，`Dubhe.txt` 第 2 簇被改成 2344）。改 `fs/exfat/ExfatParser.cpp` |
| **W8 可选严格 header 校验（CAR-002，默认 OFF）** | `tests/test_rawscan_strict_headers.cpp` / CTest `RawScanStrictHeaders`；PNG/GIF/JPEG/PDF/ZIP 最小结构校验 | 11/11 全绿；默认关闭 → 行为零变化；开启后 FP 121→115（-5.0%）、precision 11.68%→12.21%、**零 FN**、recall 100%。无生产调用方开启；改 `scan/RawScan.cpp/.h`。GIF/JPEG 规则保守（噪声易巧合通过），故「减少」而非「清零」弱匹配，测试断言据实为「严格减少裸命中」 |
| **W9 DFTT FAT RecoverBytes 性能 triage** | 仅改 `tests/integration/test_dftt_fat_recover_bytes.cpp` 的测试侧暴力簇重建 oracle `TryReconstructFat16ByMd5` | 该 oracle 在 `frag2/mult2/frag3` 上**挂死**（>120 s）；根因：每候选链 `CryptAcquireContext`/`CryptReleaseContext` + 整文件 MD5，Phase C 组合搜索（remaining=3、freeRuns=1 时约 1M 候选）放大。终修：自研可克隆 `FastMd5`（RFC1321，已知向量 + clone 路径自校验通过）替换逐候选 CryptoAPI、整卷一次性读内存、预哈希固定首簇前缀、3M 候选硬上限兜底。`DFTT_FAT_RecoverBytes` **5.0 s** 通过（原挂死/28 s），6 个必过文件（含 3 个重建的碎片化已删文件）全部金标 MD5 恢复。该 oracle 仅测试侧，生产 `FatParser` 无暴力重建，**无生产行为变化** |
| **W10 全量回归与文档/工作记忆同步** | 受影响生产文件（Ext4Parser / ExfatParser / RawScan）并行回归 13/13 全绿（11.48 s） | W6–W9 四个新测试同跑全绿；W6–W9 工作写入 `.workbuddy/memory/2026-11-08.md`；§8「本批次明确未完成」已据实更新（见下） |
| **W11 ext4 深度恢复对齐（2026-11-21）** | 新增独立金标生成器 `tools/make_ext4_fixture.py`（纯 Python 自写全部磁盘结构，不依赖 `mke2fs`；本机 WSL 被安全策略拦截）+ CTest `Ext4DepthFeatures`；修复 `fs/ext4/Ext4Parser.cpp` 四处真实缺陷（见下 CAR-004） | **129 checks / 0 failure**，5 个变体：extents+间接块+slack 删除名+孤儿 inode / uninit extent / bigalloc / inline_data（文件与目录均跨 60 B 边界走 ibody xattr）/ **多级 extent 树 + 稀疏洞**（depth=1 索引节点、文件中间 3 段空洞，以及 legacy 间接块洞）。每文件 SHA-256 + 整镜像 FNV-1a 只读不变量 |
| **W12 ReFS 深度恢复接线（2026-11-21）** | 新增 `fs/refs/RefsParser.{h,cpp}`（`FileSystem` 适配层）+ 探测链接入 + `RecoverEngine` 簇语义扩展 + CTest `RefsRecoverBytes`；修复两个真实缺陷（见下 CAR-005） | **80 checks / 0 failure**（`refs-golden`：inline 2 个、extent 2 个、嵌套 `subdir/inside.txt` 1 个）。两轮源码级负向验证（VBR 读回 offset 11/13 → 4 项 FAIL；关子目录绑定 → 4 项 FAIL）后恢复全绿 |
| **W13 ext4 JBD2 journal 回放（2026-11-21）** | `fs/ext4/Ext4Parser.{h,cpp}` 新增**只读内存 overlay** 回放（`ReadFsBytes` 统一读入口 + `ReplayJournal` 两阶段：先收集 revoke、再按日志序应用）；`tools/make_ext4_fixture.py` 新增自写 JBD2 记录生成器与两个变体 | **187 checks / 0 failure**（`ext4-journal` 2 事务/2 块、`ext4-journal-clean` 0/0）。日志独有的 `rescue.bin`、`later.txt` 按字节精确恢复；被 revoke 的目录块保持 home 内容（`keep.txt` 存活、`poison.txt` 不得出现）。三轮源码级负向验证（关回放 → 2 项 MISSING；忽略 revoke → `poison.txt` 出现 + `keep.txt` 消失；强制回放 clean 日志 → 2 项误报）后恢复全绿 |
| **W14 JBD2 checksum v2/v3 校验（2026-11-21）** | `fs/ext4/Ext4Structs.h` 加 csum 特性常量、`Ext4Parser.cpp` 按内核 jbd2 格式校验 journal SB / desc+revoke tail / commit `h_chksum[0]` / tag csum（无 xorout 链式 CRC32C，复用 `RefsCrypto.h`）；`make_ext4_fixture.py` 加独立 CRC32C + 3 变体 | `Ext4DepthFeatures` 259→**279 checks / 0 failure**。仅在 `CSUM_V2/V3` 位开启时校验，无 csum 卷零回归；tag 布局 v3=16 B / v2=8 B，64bit 主动拒绝。源码级负向（禁 commit 校验）→ csum-bad 错误回放 2 事务、4 FAIL / exit 1。见 CAR-006b |
| **W15 真实 ext 卷逐字节恢复门禁（2026-11-21）** | 扩展 `tests/integration/test_cfreds_ext4.cpp`：`RecoverEngine::RecoverFile` 恢复真实 `dfr-01-ext.dd` 全部可恢复删除文件，再用引擎 runlist 从镜像直接重建字节逐字节比对；`tests/CMakeLists.txt` 给该 target 补 `RecoverEngine/RecoverOutputFile/RecoverTargetPath` | **零生产代码改动**。`CFReDS_EXT4_DFR01` 从"仅 size 匹配 / 仅发现"升级为字节门禁：3 个真实 ext 分区（p1/p2 ext2 间接块、p3 ext4 extent+flex_bg）扫描出 12 节点 / 3 删除，其中 1 个有载荷的删除文件（712 B，间接块映射）逐字节 match。源码级负向（块偏移 +512）→ `firstDiff=5` / `byteFailed=1` / exit 1。p2/p3 的删除节点 `size=0 clusters=0` 属引擎正确判"无载荷"，非缺陷 |
| **W16 ReFS 删除残留恢复（2026-11-21）** | `RefsVolume.h` 新增窄接口 `ForEachResidualDirRow` / `ParseResidualRow` + private `CollectReachableRefs`；`RefsParser` 在 live 枚举后追加残留行，`BuildNode(...,deleted)` 置 `FileStatus::Deleted`；`Gen-ReFSGolden.py` 新增 `refs_v314_deleted_residual` + 独立真值 `deleted.tsv` | **96 checks / 0 failure**：6 live 保持 Normal、恰 1 个 ghost.dat Deleted、`RecoverEngine` 逐字节 sha256 匹配、整镜像 FNV-1a 不变。门禁发现并修复真实缺陷：object table 是叶子页，live 目录页 PageRef 嵌在行 value，仅从 13 根沿 internal 走不可达 → 曾把 6 个 live 行全误标删除；纳入 `m_objectRoots` 后分离。源码级负向（禁扫描）→ ghost MISSING / 3 FAIL / exit 8；数据级负向（ghost 簇翻转 1 B）→ `BYTES WRONG` + sha mismatch / exit 8。**合成金标**，未解析 USN journal、未接 headless/GUI。见 `docs/reports/refs_deleted_residual_2026-11-21.md` |

### CAR-003：已删 exFAT 连续文件被 FAT 拼接器改写（W3 门禁发现并修复）

`ExfatRecoverBytes` 首跑即 FAIL：恢复出的 4396 字节 MD5 为 `998c2e0c7fb86669368ab9f5b9f932fe`，与独立 oracle 的 `6650a82d6499770bf7eef8045a014b7d` 不符。根因链：

1. `ExfatParser` 对已删文件强制 `chainNoFat = noFatChain && !inDeletedTree` → `false`，忽略目录项中仍然有效的 `NoFatChain`（连续分配）位；
2. 删除操作把 FAT 项清零，解析器改走「FAT 已断 → 顺序外推」路径，仍得到正确的 `[2392, 2400]` 绝对扇区；
3. 但 `BuildClusterChain()` 随后无条件调用 FAT12/16/32 向的 `FragmentStitchService::TryImproveFatClusterChain`，其把清零的 FAT 项视为空闲簇，用格式打分把第 2 簇改成不相关的 `2336`；
4. `RecoverEngine` 按簇链写出，字节错位。

**修复**（`fs/exfat/ExfatParser.cpp`）：`NoFatChain` 是 stream 的盘上属性，删除只清目录项 type 高位，故 `chainNoFat = noFatChain` 对已删条目同样成立；并在 `noFatChain` 时跳过 FAT 启发式拼接器（连续流无碎片可拼）。非连续 exFAT 流行为不变，exFAT 仍保留恢复期的专用 `FatFragmentStitcher::TryImproveExfatNode()` 路径。修复后簇链 `[2392, 2400]`、`rawOffset=1224704`、MD5 与 oracle 一致。

### CAR-004：ext4 深度恢复四处缺陷（W11 门禁发现并修复）

`Ext4DepthFeatures` 首跑暴露 3 个缺陷，追加稀疏变体后又暴露第 4 个，全部在 `fs/ext4/Ext4Parser.cpp`：

1. **uninit extent 读出陈旧字节**：`ee_len > 32768` 表示「已分配但内容未定义」，按 ext4 规范必须返回全零。原实现直接把 `ee_start` 当有效数据写出，合成镜像里填的 `0xAA` 被原样恢复——这是**静默的错误数据**，比丢文件更危险。修复：`len -= 32768` 后发 `(0, 1)` 零标记，由 `RecoverEngine` 填零。
2. **bigalloc 整卷偏移错误**：`s_log_cluster_size > s_log_block_size` 时 extent 的 `ee_start`/`ee_len` 以**簇**为单位（1 簇 = 2^n 块），原实现按块解释，整卷数据错位。修复：`Open()` 计算 `m_clusterRatio`，`ParseExtentNode()` 对 start/len 同时缩放（`ee_block` 仍是逻辑块号，不缩放）。
3. **`inline_data` 完全不可用**：`EXT4_INLINE_DATA_FL` 文件的 ≤60 B 数据在 `i_block[]`，余量在 in-inode xattr（ibody）的 `system.data` 项（末尾 magic `0xEA020000`）。原实现既不读 ibody 也不识别内联目录，导致 120 B 文件内容错误、**内联目录整棵子树丢失**。修复：新增 `ReadInodeRaw` / `ReadInlineData` / `ParseInodeXattrData`，目录侧跳过 4 B parent inode 后复用 `ParseDirBlock`，文件侧失败则标 `Lost` 而非编造数据。
4. **稀疏文件的空洞未补，后续数据整体前移（最隐蔽）**：`FileNode::clusters` 是平铺块号数组，`RecoverEngine` 靠 `entry == 0` 识别稀疏并写零；但 `ParseExtentNode` 只追加实际 extent、`GetIndirectBlocks` 还显式 `if (blocks[i] != 0)` 跳过零槽，于是「洞之后的每个字节」都落在错误偏移上——文件大小对、哈希错，**属于静默数据损坏**。修复三处：① `GetExtentBlocks` 引入 `nextLogicalBlock` 游标，`logical > cursor` 时补 `(0, gap)`（多级树共享同一游标）；② 间接块 `level==1` 的零项按洞计（`--remaining`），`level>1` 的零项按子树容量 `entriesPerBlock^(level-1)` 补洞，`i_block[12..14]` 为 NULL 但仍有未覆盖逻辑块时同样补零；③ 新增 `AppendBlockRun()` 统一展开，`start == 0` 展开为 `length` 个零而不是 `0,1,2,…`（块 0 从不分配给文件，标记无歧义）。

金标由 `tools/make_ext4_fixture.py` 独立生成（自写超块/组描述符/inode/extent 树（含 depth=1 索引节点）/xattr，不依赖 `mke2fs`），每文件 SHA-256 与整镜像 FNV-1a 双重校验。

### CAR-005：ReFS 接入生产链两处缺陷（W12 门禁发现并修复）

1. **VBR 几何按 NTFS BPB 读取（严重）**：`RecoverEngine::ParseFsVolumeHints` 的 ReFS 分支读 offset 11/13，但 ReFS VBR **不是 BPB**——`+0x03` 是 `"ReFS\0\0\0\0"`、`+0x10` 是 `"FSRS"`、`+0x14` 是固定 `0x0200` 标记，真实几何在 `+0x20`(u32 bytesPerSector) / `+0x24`(u32 sectorsPerCluster)。offset 11/13 读到全零 → 回退成「1 簇 = 1 扇区」，`photo.bin`（12288 B / 3 簇）只写出 1536 B 后 `size mismatch`。修复后按真实布局解析，并加 `"FSRS"` 双重确认。**这个缺陷只在真实 ReFS 卷上暴露,`RefsVolume` 自身的单元测试碰不到,因为引擎侧解析与 `fs/refs` 层是两条独立代码路径。**
2. **目录名无法绑定子对象（嵌套目录整棵丢失）**：ReFS 目录项（`kf=0x01`/`kf=0x02`）只带**条目序号**（该目录内 1,2,3…），**不携带子对象 OID**；`kf=0x02` 的 84 字节变体尾部 12 字节也全为零。原实现把该序号当 OID 调 `ListDirectory()`，必然落空。修复：以 **parent-child 表**（唯一权威 `parent OID → child OID`）重建树，名字按 B+ 树序绑定；当子对象数多于目录名数时，先剔除「B+ 树为空」的对象（命名目录必有行，别名/流对象必空），**仅在该剔除能让计数相等时才生效**，避免误删真实的空子目录。

`refs-golden` 的 `refs_v314_*` 系列有 `subdir/inside.txt` 两级结构，是本条的唯一字节级门禁。

### CAR-006：ext4 无 journal 回放（W13 补齐，非"缺陷修复"而是能力缺口）

语义上这不是把某段代码写错，而是**整个能力缺失**：`Ext4Parser` 只读 home location，因此「元数据已提交到 JBD2 但未 checkpoint」的崩溃窗口里，文件在盘上就是不存在。这是恢复工具最该覆盖的场景，缺它等于把最常见的一类掉电/蓝屏损失直接判死。

实现（`fs/ext4/Ext4Parser.{h,cpp}`）：

1. **只读内存 overlay**：`m_overlay`（块号 → 已提交内容）挂在 `ReadFsBytes()`。扫描源**一个字节都不写**——不是"replay 后回滚"，而是从头到尾只读，整镜像 FNV-1a 不变有门禁守着。
2. **所有元数据读取必须走同一入口**：`ReadInode` / `ReadInodeRaw` 是按 `bg_inode_table + idx * inode_size` **算出来直接读**的，从不经过 `ReadBlock`。只改 `ReadBlock` 会让 inode 表绕过 overlay，症状是"目录项回来了但 inode 还是空的"。故新增 `ReadFsBytes`，四个读取点全部归口。
3. **两阶段回放，顺序不能省**：revoke 记录可能**排在它所取消的 descriptor 之后**（`Ext4Parser` 的 fixture 里就是这样：txn 102 记了块，txn 103 才 revoke）。边扫边应用会把已被取消的块写进 overlay。必须先整趟收集 `m_revoke`（块号 → 最大 revoke 序号），再按日志序应用。
4. **revoke 判定照 `jbd2_journal_test_revoke`**：`t_seq <= revoke_seq` 才丢弃；**晚于 revoke 的重记有效**。写反会让"删目录项"这类负向事务反向生效。
5. **只应用已提交事务**：坏 magic / 缺 commit / 序号跳变即结束扫描，`pending` 直接丢弃。这正是内核崩溃后的行为，也保证"半个事务"永不进入 overlay。
6. **超级块永不 overlay**：几何是 `Open()` 的其余逻辑赖以成立的前提，中途替换等于让读它的代码重新解释整个卷。GDT 则在回放后**重读一次**（组描述符本身可能被日志化）。
7. **`s_start == 0` 必须短路**：那表示日志已 checkpoint，盘上就是最新状态。缺这道判断就会把"日志区里有内容"当成"有待回放事务"，凭空造出已删除的文件——`ext4-journal-clean` 变体专职守这条。
8. **兼容现代日志**：legacy/v2（无 64BIT）tag 为 8 B（be32 blocknr + be16 csum + be16 flags），v3 为 16 B（be32 blocknr + be32 flags + be32 high + be32 csum）；支持 `JBD2_FLAG_ESCAPE`（首 4 B 被写成 0 时还原 magic）与 `JBD2_FLAG_DELETED`（不占数据块）。64BIT 日志主动拒绝（避免误解析产生错误 overlay）。

### CAR-006b：JBD2 checksum v2/v3 校验（W14 补齐 —— 防"损坏日志→静默错误数据"）
此前回放**不校验**任何 checksum：坏盘上被位翻转/部分写入污染的日志块会把错误内容 overlay 到正确位置——比丢文件更危险（与 CAR-003 的教训同类）。依据内核 `jbd2` 的 `recovery.c`/`commit.c` 精确实现只读校验，**只在 `s_feature_incompat` 含 `CSUM_V2`(0x8)/`CSUM_V3`(0x10) 时启用**，无 checksum 的卷行为零变化。

9. **`j_csum_seed` = jbd2_chksum(~0, s_uuid@0x30, 16)**：注意内核 `jbd2_chksum()` 是**无 xorout 的链式 CRC32C**（`crc32c_le` 返回原始状态直接作下一次 seed），复用 `fs/refs/RefsCrypto.h` 的 `Crc32CUpdate`（**不加** `Crc32CFinish`）。这是踩过的坑：加 xorout 会让每个 checksum 全部 desync。
10. **superblock `s_checksum`@0xFC**：整块（该 4 B 清零后）的 raw CRC32C；`s_checksum_type`@0x50 必须是 `JBD2_CRC32C_CHKSUM`(4)。范围必须是**整块而非前 0xFC 前缀**——这是另一个踩过的坑。
11. **descriptor/revoke 尾部 `jbd2_journal_block_tail`**（块末 4 B be32）：整块（尾部清零后）raw CRC32C；失败即结束扫描。
12. **commit `h_chksum[0]`@0x10**：整块（该字段清零后）raw CRC32C；失败即**不提交该事务并结束扫描**（崩溃窗口的正确行为）。
13. **每个 data block 的 tag checksum**：`csum = crc32c(seed, be32(seq)) → crc32c(csum, data_block)`；v3 存 tag+12 的 be32，v2 存 tag+4 的 be16 低 16 位。单个 tag 失败只**跳过该数据块**（内核 `goto skip_write`），不污染其他块。
14. **tag 布局随特性切换**：v3 = 16 B（flags 在 +4、high 在 +8、csum 在 +12），v2/legacy = 8 B（csum 在 +4、flags 在 +6）。

Oracle（`tools/make_ext4_fixture.py`）自带独立表驱动 CRC32C（校验向量 `CRC32C("123456789")=0xE3069283`，与 C++ 一致），造 3 变体：`ext4-journal-csum2`/`csum3`（正确 CRC，回放结果与 legacy 逐字节一致）、`ext4-journal-csum-bad`（仅翻坏第一事务的 commit checksum，引擎必须 0 回放）。CTest `Ext4DepthFeatures` 由 259→**279 checks / 0 failure**。源码级负向守卫：把 commit 校验改成 `if(false && ...)` → csum-bad 错误回放 2 事务、rescue/later 凭空出现、**4 FAIL / exit 1**，证明门禁真实有效。

**仍未覆盖**：`metadata_csum` 的 inode/组描述符校验、外部日志设备（`s_journal_dev`）、`FLEX_BG` 跨组 GDT 重放；JBD v1 日志（旧 ext3，主动拒绝而非误解析）；真实掉电卷未测。

### 本批次已闭环（W6–W9，此前列为未完成，现据实更正）

- **独立 ext2/3 金标**：已由 W6 的 `Ext2Ext3LegacyGolden`（69/69）闭环，ext2 删除文件名经 dirent slack 恢复、目录识别正确、无 orphan 重复。
- **exFAT 多簇 / 碎片化 / FAT-chained 恢复字节门禁**：已由 W7 的 `ExfatMulticlusterBytes`（109/109，4 镜像 7 文件）闭环；dfr-12 误导 `FAT[first]` 被 W3 `NoFatChain` 修复正确忽略（多簇场景成立）。
- **`DFTT_FAT_RecoverBytes` 暴力簇重建性能**：已由 W9 修复——测试侧 oracle 原在 `frag2/mult2/frag3` 上挂死（>120 s），改为自研可克隆 `FastMd5` + 整卷读内存 + 预哈希首簇前缀 + 3M 候选上限后 **5.0 s** 通过（无生产行为变化）。**注意**：原「`DFTT_FAT`/`DFTT_NTFS` 暴力簇重建 >6 min」的说法不准确——`DFTT_NTFS_RecoverBytes` 走生产 `RecoverEngine`、无暴力 oracle，本就 0.1 s（9 个金标全过），从不慢；只有 FAT 测试侧 oracle 慢，现已修复。

### 仍明确未完成（不得宣称）

- 真实磁盘 / 真实卷（10–50 GB、碎片化、坏扇区）端到端提速与误报率仍未量化；§9 的 14.38 FP/MiB 是 8 MiB 合成随机镜像基线，不是真实卷结论，也尚未据此调参加 footer / secondary 约束。
- `WIN-FORMATTED-VOLUME` 已补**合成**金标 `FormattedVolumeRecovery`（4 MiB 快速格式化模型，22 项）；**真实快速格式化磁盘**仍未测，合成场景下 RAW 假阳性约 13.5 FP/MiB。
- A1-1d / A1-3b / A1-4b-b / A1-5b（真实 VMDK/QCOW2/VDI 镜像、真实 BitLocker 卷、真实 RAID 阵列与设备主路径接线、真实 ReFS 卷与接口接线）仍待真实样本与/或隔离硬件，完成前不得宣称对应「真实卷恢复」能力。
- 真实镜像误报/漏报率调参（footer / secondary 约束增强）、真实全盘端到端提速基准：仍待真实卷。

### 操作注记（非缺陷，回归时务必遵守）

- **全量 `ctest` 的 6 项既有失败（2026-11-21 用 `git stash` 回退全部改动后复现，与本轮 ext4/ReFS 工作无关）**：
  `DFR_NIST_Compare` / `DFR_NIST_BatchStrict`（真实 DFR 镜像的已删文件计数越出 NIST 区间，如 dfr-06-fat `del=0` 期望 3）、
  `DFTT_11_CARVE_FAT`（FAT 雕刻）；以及 3 项 `Not Run`：`DFTT_Recover_GoldenBundle` / `Testdata_EwfExportReady` / `RepairSamplesIntegrity`
  ——后 3 项 CTest 记录的 `Command: ""`，即 `find_program(POWERSHELL)` 为空，是**运行 ctest 时 PATH 被精简到不含 `WindowsPowerShell`** 所致；
  **实测把 `C:/Windows/System32/WindowsPowerShell/v1.0` 加回 PATH 后 3/3 Passed**，并非缺金标。判定标准：**先跑基线再归因，不要把这些算到新改动头上**。
- GCC14 的 `std::filesystem` 相关可执行文件在旧版 `libstdc++-6.dll`（Qt MinGW 7.3）先于 MSYS2 运行库出现在 PATH 时会以 `0xc0000139` 启动失败；**所有 GCC14 ctest / exe 运行命令须把 `/c/msys64/mingw64/bin` 置于 PATH 首位**。
- `FileCarving_Graphic_All` 本机耗时约 245 s，`--timeout 240` 会误报 Timeout，需单独用 `--timeout 900` 运行（32 项合并跑的一轮即因此出现 1 项 Timeout，非逻辑失败）。
- `testdata/tools/Compare-RecoveryResults.ps1` 并不存在；`expected_results.csv` 是 W3 场景矩阵而非「文件名 → MD5/大小」基线，`TEST_PLAN.md §6.2` 的旧描述已更正。

*本路线随工程演进更新；阶段状态变更须同步 `PRODUCT_SCOPE §10` 文档维护表。*
