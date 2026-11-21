# 产品范围：扫描 · 恢复 · 修复（EaseUS 对标）

> **版本**：1.0  
> **日期**：2026-05-27  
> **状态**：当前迭代唯一主线  
> **关联**：[TEST_EXECUTION_MATRIX.md](./TEST_EXECUTION_MATRIX.md)、[TEST_DATA_SOURCES.md](./TEST_DATA_SOURCES.md)、[PERFORMANCE_BASELINE.md](./PERFORMANCE_BASELINE.md)、[DESIGN_UnifiedScanPipeline.md](./DESIGN_UnifiedScanPipeline.md)、[DESIGN_RepairSubsystem.md](./DESIGN_RepairSubsystem.md)

---

## 1. 北极星

> **定位变更（2026-11-07）**：产品目标由「只读数据恢复工具（对标 EaseUS）」扩展为「数据恢复 + 分区管理 + 备份还原 + 磁盘工具一体化工具（全面对齐 DiskGenius `6.2.0.1829`）」。执行路线见 [ROADMAP_RECOVERY.md](./ROADMAP_RECOVERY.md) 的 **A0–A5** 专章。本节三主线**继续作为恢复赛道的质量基线**，不再是产品范围上限。

三条主线（质量基线，持续有效）：

| 主线 | 用户要感受到的价值 |
|------|-------------------|
| **扫描** | 选对盘 → 等得起 → 看得懂 → 信得过（删除/丢失/雕刻/重建有区分） |
| **恢复** | 勾得准 → 预览敢点 → 写出对 → 大批量不崩 → **扫描源绝不写入** |
| **修复** | 常见损坏能救 → 失败说清原因 → 与扫描/恢复路径顺 |

恢复赛道的护城河仍是 **扫描结果可信度 + 预览降低决策成本 + 恢复写出稳定 + 场景数据集长期打磨**。本文 **§3–§8** 的 EaseUS 对标定义、场景验收矩阵、差距对照与工程优先级全部保留，作为三主线的验收门禁；新增的广度类能力按 `ROADMAP_RECOVERY.md` 的 A0–A5 阶段门禁单独验收。

---

## 2. 范围：一体化工具（DiskGenius 对齐）

**本节已解除 2026-05-27 版的「范围冻结」。** 产品范围包含四个功能域：

| 功能域 | 范围 | 路线阶段 |
|--------|------|----------|
| 数据恢复 | 删除/格式化/分区丢失/RAW 恢复、签名雕刻、碎片重组、文件修复、预览 | 三主线（既有）+ A1 |
| 分区管理 | 分区 CRUD、格式化、无损调整、MBR↔GPT、分区表备份还原 | A3 |
| 备份还原 | 镜像备份/还原、磁盘/分区克隆、系统迁移 | A2 |
| 磁盘工具 | 坏道检测、SMART、扇区编辑、擦除、WinPE、引导修复、测速 | A4 / A5 |

### 2.1 前置门禁（硬约束）

1. **A0 未验收前，不得合入任何会打开设备写句柄的代码**。写路径必须先具备：写目标模型、默认拒绝的授权白名单、事务日志与回滚、唯一写句柄封装、隔离测试环境（详见 `ROADMAP_RECOVERY.md §3`）。
2. **扫描源只读不变量不可破坏**：任何已注册为扫描源的设备，在整个会话内禁止写打开。
3. A3/A4 类操作不得在含真实数据的盘上开发自测；每次新增写能力须同时提交回滚用例与操作前后哈希比对结果。
4. `§5.5` 七条发版门禁继续生效，不因范围扩展而放宽。

### 2.2 仍然冻结（明确不补全）

> **2026-11-21 解冻（用户指令）**：以 `D:\dev\testdisk-7.2` 为基线对齐「文件系统广度 + 分区写回/boot 修复」。
> 第一条中的 **APFS / HFS+ / Btrfs / XFS / F2FS** 自此**解除冻结**，但只按 `ROADMAP_RECOVERY.md` **W17 / P1** 阶段推进：
> **只读解析 + 目录枚举 + 簇映射 + 逐字节恢复门禁**，每个 FS 必须有独立 oracle 金标与源码级负向守卫；不做写、不做格式化、不接 UI 之前不得对外宣称支持。
> 其余冻结项（动态卷/LVM2/存储池、NAS、P2V/测速/低级格式化）仍然有效。

以下能力与「Windows 数据恢复 + 磁盘管理」主线无关，ROI 低或风险过高，**继续不做**，除非后续独立评审解禁：

- ~~APFS / HFS+ / Btrfs / XFS / F2FS 等新文件系统~~ → 见上方解冻说明，按 W17 P1 只读推进
- 动态卷 / LVM2 / 存储池、NAS / 网络位置恢复
- P2V 物理机转虚拟机、磁盘测速、批量格式化 U 盘、低级格式化

### 2.3 允许并行

- A1 全部只读能力（虚拟盘/镜像挂载、BitLocker 解锁、RAID 只读重组、ReFS 解析）**不依赖 A0**，可立即推进。
- 引擎深度（扫描并行化、签名多模式匹配、大结果集 UI、FS 特性、雕刻/修复覆盖）作为并行轨持续推进，不阻塞 A1–A3。
- 与三主线直接相关的性能、预览、去重、报告、自动化回归。

---

## 3. EaseUS 对标定义（可验收）

### 3.1 扫描

| 维度 | EaseUS 用户预期 | 我方验收口径 |
|------|----------------|--------------|
| 模式 | 快速 / 深度 / 高级（ carving ）可理解 | 统一三阶段流水线对用户 **零选手动模式**；UI 显示阶段文案（元数据 / 深度 / 雕刻） |
| 速度 | 小卷分钟级有结果；大卷可取消 | 6MB DFTT 镜像：端到端 &lt; 30s（Release）；&gt;1GB 镜像：取消 &lt; 5s 内 UI 恢复可点 |
| 结果 | 路径树、类型、搜索、删除标记 | 删除/丢失/雕刻文件在列表与树上有 **一致标记**；`carved_*` 与原名条目不误导为「完好路径」 |
| 可信 | 碎片/重建有「可恢复性」提示 | `RecoverabilityLevel` 或等价 UI 对 **非连续簇** 文件可见 |
| 稳定 | 10 万级结果不卡死 | 见 [PERFORMANCE_BASELINE.md](./PERFORMANCE_BASELINE.md) §4：1 万/10 万条滚动、搜索、全选有实测数据 |

### 3.2 恢复

| 维度 | EaseUS 用户预期 | 我方验收口径 |
|------|----------------|--------------|
| 安全 | 不能写源盘 | `RecoverEngine::ValidateTargetPath` 对同卷目标 **硬拒绝**（RCV-001） |
| 批量 | 勾选恢复、进度、可取消 | 批量恢复有成功/失败/跳过统计；取消后无半文件残留策略明确 |
| 结构 | 保留目录 / 平铺 | 「保留原目录结构」开/关行为与 UI 一致（RCV-003） |
| 冲突 | 重名不静默丢文件 | 目标已存在：**自动重命名或明确策略**（当前为 Skip，**未达 EaseUS**，见 §6） |
| 正确性 | 删而未覆盖的文件字节正确 | 对照镜像黄金文件：**MD5 一致率 ≥ 95%**（P0 场景）；碎片场景单独矩阵 |
| 预览 | 恢复前看图/看视频头 | 常见 JPG/PNG/MP4：**预览成功或明确降级**；预览不拖垮 UI（PRV-001~003） |

### 3.3 修复

| 维度 | EaseUS 用户预期 | 我方验收口径 |
|------|----------------|--------------|
| 格式 | Office / 压缩 / 图 / 视 | 已注册修复器列表与扩展名一致；不支持格式 **不假装能修** |
| 质量 | 修完能打开 | 修复后 `RepairValidate` 自检通过才标成功；报告含 `validationDetail` |
| 衔接 | 从扫描结果带去修 | 扫描结果 → staging 导出 → 修复页；staging 清空、按 id 分目录无冲突 |
| 诚实 | 结构修 vs 转码 | UI/报告区分「ZIP 重建」「FFmpeg 重封装」「RAW 仅导出 JPEG 预览」 |

---

## 4. 当前能力快照（截至 2026-05-27）

> 供差距表引用；发版前应以 CTest + 矩阵手工列为准更新「实测」列。

### 4.1 扫描引擎

| 组件 | 状态 | 说明 |
|------|------|------|
| `UnifiedVolumeScanner` 三阶段 | **已实现** | 阶段 1 `VolumeSectorScanner` → 阶段 2 `DeepScan` → 阶段 3 `RawScan` |
| FS 支持 | **部分** | NTFS、FAT、exFAT、ext4 有解析器；**无** Btrfs/XFS/HFS+ |
| 签名雕刻 | **已实现** | `SignatureDB`：**514** 条内置签名条目（`UiTypeCoverage` 口径；运行时 `m_sigs.size()` 实测 **513**，含 `UiTypeSignatureSupplement`）；约 **309** 个不重复扩展名；支持 `custom_signatures.json` 扩展（A1-9 起 `LoadFromFile` 无 Qt 依赖，headless 构建同样可用） |
| 配置跳过阶段 | **已实现** | `skipDeepScan` / `skipRawScan` |
| UI 阶段进度 | **部分** | 有进度与完成态；SCN-002 阶段文案需人工确认 |

### 4.2 恢复引擎

| 组件 | 状态 | 说明 |
|------|------|------|
| `RecoverFile` / `RecoverBatch` | **已实现** | NTFS 压缩、ext4、RAW 偏移、簇链 |
| 同卷保护 | **已实现** | `ValidateTargetPath` |
| 保留目录结构 | **已实现** | `keepDirStructureCheck_` |
| 目标已存在 | **缺口** | `RecoverStatus::Skipped`，**无自动重命名** |
| 完成页 / 明细导出 | **已实现** | 恢复完成覆盖层 + 导出明细 |
| 扫描 → 修复 staging | **已实现** | `exportFilesToRepairStaging` + 右键「尝试修复…」 |

### 4.3 修复子系统

| 修复器 | 类型 | 自检 |
|--------|------|------|
| `ZipRepair` / `OfficeZipRepair` | 结构重建 | `RepairValidate` |
| `JpegRepair` / `PngRepair` / `GifRepair` | 头部/结构 | 有 |
| `Mp4Repair` / `FfmpegVideoRepair` | moov / remux | 有 |
| `FfmpegImageRepair` / `FfmpegRawRepair` | 转码 / RAW→JPEG 预览 | 有（RAW 非「恢复原 RAW」） |

自动化：`RepairSamplesIntegrity`、`RepairSamples`（Qt）、`CrashDiagnostics`。

### 4.4 自动化回归（CTest，镜像存在时）

| 用例 | 数据源 |
|------|--------|
| `DFTT_FAT_Undelete` | DFTT #6 |
| `DFTT_NTFS_Undelete` | DFTT #7 |
| `CFReDS_FAT_DFR01` | NIST dfr-01-fat |
| `CFReDS_NTFS_DFR01` | NIST dfr-01-ntfs |
| `PartitionProbe` | CFReDS + DFTT #10 |
| `RepairSamples*` | `testdata/repair_samples/` |
| `CFReDS_DFR05_*` / braid | DFR-05 碎片 |
| `CFReDS_DFR05_QualityCurve` | 碎片导出质量 CSV |
| `MediaFormatQualityBaseline` | 媒体 jpg/mp4 评分门槛 |
| `FatFragmentStitchSynthetic` | 合成 FAT 碎片重组 |
| `DFTT_11_CARVE_FAT` | 雕刻（镜像存在时） |
| `WriteGuard*` / `A0Transaction*` | 内存 fixture，无镜像依赖（A0） |
| `VirtualDisk*` / `VirtualRaid` | 合成 fixture + `testdata/virtual-disk-golden/` 真实镜像金标（A1-1/A1-2/A1-4a） |
| `BitLockerCrypto` / `BitLockerVolume` | FIPS 向量 + `testdata/bitlocker-golden/` 合成加密卷金标（A1-3a） |
| `RefsCrypto` / `RefsVolume` | 标准 CRC / FIPS SHA-256 向量 + `testdata/refs-golden/` 合成 ReFS 卷金标（A1-5a，**非真实 Windows ReFS 卷**） |
| `RefsParser` | `testdata/refs-golden/` 端到端字节门禁 CTest `RefsRecoverBytes`（A1-5b 接口接线：inline / extent / 嵌套目录共 6 条目逐文件 SHA-256 + 整镜像 FNV-1a；**合成卷，真实 ReFS 卷未验**） |
| `Ext4Parser`（深度） | `tools/make_ext4_fixture.py` 独立生成器 7 变体金标 + CTest `Ext4DepthFeatures`（extents/间接块/slack 删除名/孤儿 inode、uninit extent、bigalloc、inline_data 文件与目录、depth=1 extent 树 + 稀疏洞、**JBD2 回放与其 clean 反例**，187 项） |
| `RaidProbe` / `ScanSourceChain` | `testdata/raid-probe-golden/` 合成 mdadm 成员盘与组合链金标 + 既有 `bitlocker-golden/` / `refs-golden/` 期望表（A1-4b-a，**非真实阵列**） |
| `SignatureMatcher` | `testdata/signature-matcher-golden/` 合成 64 KiB 镜像 + 独立 Python oracle 期望表（A1-6 / 并行轨：按类型恢复参数化引擎底座，**已接 `RawScan` 主路径**：`SignatureDB::ScanBatch` 流式 AC 批扫描替换线性 `memcmp`；A1-7 `DeepScan` 损坏 NTFS 回退 RAW 雕刻、A1-8 并行 RAW 扫描分块+合并均由对应 CTest 覆盖） |
| `DeepScan` 损坏 NTFS/FAT 回退（A1-7） | 合成损坏 NTFS 镜像（合法 boot 扇区 + 垃圾 MFT + 植入 header-only magic）；CTest `DeepScanFallback` 正向：识别 FS 但结构化解析 < 8 文件时回退 `RawScan` RAW 雕刻并恢复植入 magic；反向：无 FS 识别时零恢复（回退门控正确） |
| 并行 RAW 扫描（A1-8） | 合成 8 MiB 随机填充镜像 + 严格递增 header-only magic（含 chunk 边界扇区）；CTest `RawScanParallel` 断言 `RawScan::Run(reader,partition,N)` 对 N=2/4/8 与串行逐偏移（rawOffset/ext/size）完全一致、无重复偏移 |
| 自定义签名加载（A1-9） | `testdata/custom_signatures.json` + 零依赖 `scan/SignatureJson.h`；CTest `SignatureDBLoad` 90 项（解析器属性 / 严格拒绝 / 字段逐项 / 坏 hex 跳过 / AC 重建 / 真实样例），含源码级负向验证 |
| 并行 RAW 扫描生产接线（A1-10） | 合成 4 MiB 随机填充镜像；CTest `RawScanParallelWiring` 26 项：配置默认值与 `MakeCarveConfig` 映射、精确签名集构造器、自定义 / 过滤库下 `CarveModuleService` 的 parallel==serial、`targetExtensions` 真实生效、回调零跨线程、取消传播、源码级接线守卫；两轮源码级负向验证 |
| 并行 RAW 扫描吞吐基线（A1-11） | **非 CTest**（计时断言会抖动）：`tools/bench_rawscan_parallel.cpp` + CMake 选项 `PHOENIX_BUILD_BENCH`（默认 OFF）；256 MiB 合成镜像，直连与生产配置链两入口，1/2/4/8 线程各 3 次取中位数并校验文件集一致；实测 `PERFORMANCE_BASELINE.md §8` |

**未自动化**：CFReDS dfr-02/03、GUI 全流程、10 万行 UI 压测填数。

---

## 5. 场景验收矩阵（必须先绿）

与 `testdata/manifests/datasets.csv` 对齐。状态列请在每轮回归后更新：`⬜ 未跑` `🟡 部分` `🟢 通过` `🔴 失败`。

### 5.1 扫描 P0

| ID | 场景 | 数据集 | 通过标准 | 自动化 | 状态 |
|----|------|--------|----------|--------|------|
| SCN-P0-01 | FAT 删除文件可见 | `DFTT-6-UNDEL-FAT` | 预期路径/文件名命中 ≥ 团队基线 | `DFTT_FAT_Undelete` | 🟢 |
| SCN-P0-02 | NTFS 删除（含 resident） | `DFTT-7-UNDEL-NTFS` | 同上 | `DFTT_NTFS_Undelete` | 🟢 |
| SCN-P0-03 | NTFS 删除 1GB 卷 | `CFREDS-DFR-01-NTFS` | 扫描完成无崩溃；关键文件数 ≥ 基线 | `CFReDS_NTFS_DFR01` | 🟢 |
| SCN-P0-04 | FAT 删除 1GB 卷 | `CFREDS-DFR-01-FAT` | 同上 | `CFReDS_FAT_DFR01` | 🟢 |
| SCN-P0-05 | 三阶段进度可辨 | 任意 &gt;1GB | UI 显示阶段 1/2/3 文案 | 手工 SCN-002 | ⬜ |
| SCN-P0-06 | 扫描可取消 | 大镜像 | 5s 内停止、UI 可点 | 手工 SCN-003 | ⬜ |
| SCN-P0-07 | 基础雕刻 | `DFTT-11-CARVE-FAT` | `paul.jpg` 字节黄金 + 镜像 MD5 | `DFTT_11_CARVE_FAT`（缺镜像 SKIP） | 🟡 |

### 5.2 恢复 P0

| ID | 场景 | 数据集 | 通过标准 | 自动化 | 状态 |
|----|------|--------|----------|--------|------|
| RCV-P0-01 | 同卷写保护 | 任意 | 选源卷路径 → 阻止 + 提示 | CTest `ValidateTargetPath` + GUI 手工 RCV-001 | 🟢 |
| RCV-P0-02 | 批量恢复字节正确 | `DFTT-6` / `DFTT-7` | 黄金文件 MD5 一致 | `DFTT_Recover_GoldenBundle`（含 `Compare-RecoveryResults.ps1 -Strict`） | 🟢 |
| RCV-P0-02b | FAT 碎片（格式感知） | 结构化文件 | 链断时按格式结构重组簇链（无黄金 MD5） | `FatFragmentStitcher` + `FatFragmentStitchSynthetic` + UI「格式感知碎片重组」 | 🟢 |
| RCV-P0-03 | 目录结构开关 | DFTT 有目录样本 | 开=层级；关=平铺 | CTest `RecoverBuildTargetPath` + GUI 手工 RCV-003 | 🟢 |
| RCV-P0-04 | 完成统计与导出 | 任意批量 | 成功/失败/跳过 + 明细文件 | GUI 手工 RCV-004/005（完成页 + 导出明细） | 🟢 |
| RCV-P0-05 | **重名策略** | 同批两文件同名 | 自动 `name (1).ext` 等不冲突路径后写出 | CTest `UtilsUniquePath` + `RecoverEngine` | 🟢 |
| RCV-P0-06 | 碎片场景（元数据） | `CFREDS-DFR-05-*` | 分区上可发现已删除文件（与 DFR-01 同 harness） | `CFReDS_DFR05_FAT` / `CFReDS_DFR05_NTFS` | 🟢（镜像存在时） |
| RCV-P0-06b | 碎片场景（写出大小） | DFR-05 | 删除文件写出字节数与 manifest 一致 | `CFReDS_DFR05_RecoverSmoke` | 🟢（镜像存在时；无 MD5） |
| RCV-P0-06c | 碎片质量曲线 | DFR-05 + braid | 导出后 `FormatPayloadScore` CSV；结构化样本 `Complete` 门槛 | `CFReDS_DFR05_QualityCurve` + `FragmentFatStructured_QualityGate` + `FragmentNtfsStructured_QualityGate` + `CFReDS_BraidNtfs_FragmentStitch` | 🟡（DFR-05 多为 txt/Weak；FAT/NTFS 合成结构化门禁已绿；CFReDS braid NTFS 仍为冒烟） |

### 5.3 预览 P0（恢复决策核心）

| ID | 场景 | 数据集 | 通过标准 | 状态 |
|----|------|--------|----------|------|
| PRV-P0-01 | 图片预览 | `MEDIA-XIPH-POSTER-BIG` / 扫描结果 JPG | 缩略图或「无法预览」 | ⬜ |
| PRV-P0-02 | 视频首帧 | `MEDIA-SAMPLELIB-MP4-5S` | 首帧或降级，无崩溃 | ⬜ |
| PRV-P0-03 | 大文件限流 | &gt;100MB 视频 | UI 可交互、内存无失控 | ⬜ |

### 5.4 修复 P0

| ID | 场景 | 数据集 | 通过标准 | 自动化 | 状态 |
|----|------|--------|----------|--------|------|
| REP-P0-01 | ZIP 截断 | `REPAIR-TRUNC-ZIP` | 修复成功 + 自检 | CTest `RepairSamples` | 🟢 |
| REP-P0-02 | MP4 ftyp 损坏 | `REPAIR-CORRUPT-MP4` | 同上 | CTest `RepairSamples` | 🟢 |
| REP-P0-03 | JPEG 截断 | `REPAIR-TRUNC-JPG` | 同上或明确部分成功 | CTest `RepairSamples` | 🟢 |
| REP-P0-04 | 扫描→修复闭环 | GUI + DFTT 可恢复 JPG/ZIP | staging → 修复页 → 报告 | GUI 手工 | 🟡 |
| REP-P0-05 | Office docx/pptx/xlsx | `repair_samples` 扩展集 | 打开级校验 | 待样本/用例 | ⬜ |

### 5.5 发版门禁（仅三条主线）

满足以下 **全部** 方可称「扫描/恢复/修复达到 v1 可发布水准」（仍未必等于 EaseUS 全场景）：

1. §5.1 中 SCN-P0-01~04、06 为 🟢  
2. §5.2 中 RCV-P0-01~04 为 🟢，且 RCV-P0-05 已关闭（实现重命名或用户可选策略）  
3. §5.3 中 PRV-P0-01~02 为 🟢  
4. §5.4 中 REP-P0-01~04 为 🟢  
5. `scripts/Regress.ps1`（或 CI 等价）在 Release 配置全绿  
6. [PERFORMANCE_BASELINE.md](./PERFORMANCE_BASELINE.md) 至少填入 6MB/62MB 镜像实测一行  
7. §6.6 **TYPE-P0-01**（`UiTypeCoverage`）持续 PASS；TYPE-P0-02/03 在发版前标 🟢  

### 5.6 文件类型 P0（新增）

| ID | 场景 | 通过标准 | 自动化 | 状态 |
|----|------|----------|--------|------|
| TYPE-P0-01 | UI 类型树 ⊆ 可扫描覆盖 | `UiTypeCoverage`：321 扩展名均有签名/别名/容器规则 | CTest `UiTypeCoverage` | 🟢 |
| TYPE-P0-02 | 主流图片预览 | jpg/png/gif/webp/heic 扫描结果可缩略图或明确失败 | 手工 PRV + §6.4 | ⬜ |
| TYPE-P0-02b | 文档类网格预览 | pdf/docx 等见 §6.5；依赖 Windows Shell | 手工 PRV；扩展名/读长单测 `GridPreviewExtensions` | 🟡 |
| TYPE-P0-03 | 主流视频预览 | mp4/mov/avi/mkv 首帧或 Shell 缩略图 | 手工 PRV + §6.4 | ⬜ |
| TYPE-P0-04 | 主流修复格式 | zip、docx/xlsx/pptx、jpg、mp4 样本修复+自检 | `RepairSamples*` | 🟢 |

---

## 6. 支持的文件类型对比（EaseUS vs PhoenixRecover）

> **EaseUS 侧**：下列「官方宣称」来自 [EaseUS Data Recovery Wizard](https://www.easeus.com/datarecoverywizard/) 等产品页（**1000+ 文件类型**、按大类列举格式）。  
> **我方侧**：以仓库源码与 CTest 为准（**2026-05-27** 在 `build/regress` Release 跑 `UiTypeCoverage` 得到下表数字）。  
> **重要**：「UI 能筛选」≠「雕刻必命中」≠「能预览」≠「能修复」——对比时必须分栏。

### 6.1 四层能力口径

| 层级 | 含义 | PhoenixRecover 数据源 |
|------|------|------------------------|
| **L1 文件系统元数据** | 删除/丢失文件按目录项、MFT、inode 找回 | `VolumeSectorScanner` → NTFS / FAT / exFAT / ext4 |
| **L2 签名雕刻（RAW）** | 无 FS 或 FS 损坏时按魔数 carving | `SignatureDB` + `RawScan`（阶段 3） |
| **L3 UI 类型筛选** | 结果树按扩展名分类、搜索 | `UiFileTypeCatalog`（9 类） |
| **L4 预览 / 修复** | 恢复前决策 & 损坏文件处理 | `GridPreviewExtensions` + `MediaThumbnails`（含文档 Shell）；`RepairFormatUtil` |

### 6.2 总量与文件系统

| 指标 | EaseUS（官方） | PhoenixRecover（实测/代码） | 差距 |
|------|----------------|----------------------------|------|
| 宣传文件类型数 | **1000+** | **514** 签名条目；**~309** 不重复扩展名（雕刻）；**321** UI 筛选扩展名 | 🔴 数量约为 1/3（条目级）；勿对外写「1000+」 |
| 文件系统 | NTFS、FAT、exFAT、ext2/3、**HFS+**、**ReFS** 等 | NTFS、FAT、exFAT、**ext4**、**ReFS（只读解析+恢复，合成金标）**；**无** HFS+ / APFS | 🔴 Mac/部分服务器卷 |
| 自定义签名 | 不明 | `testdata/custom_signatures.json` + `SIGNATURE_FORMAT.md`；`LoadFromFile` Qt 解耦（零依赖 `scan/SignatureJson.h`），CTest `SignatureDBLoad` 90 项验证（字段/严格 JSON 拒绝/恶意 hex 跳过/AC 重建/真实样例） | 🟢 可扩展，headless 可用 |
| UI↔签名一致性 | — | CTest `UiTypeCoverage` 强制 321 扩展名有覆盖规则 | 🟢 |

### 6.3 按大类对比（扩展名数量 + 预览/修复）

UI 扩展名数量来自 `UiFileTypeCatalog.cpp`（与类型树一致）。EaseUS 代表格式来自官网大类说明。

| 大类 | UI 扩展名数 | EaseUS 代表格式（摘要） | L1/L2 扫描 | L4 预览 | L4 修复 | 差距摘要 |
|------|------------|-------------------------|------------|---------|---------|----------|
| **图片** | 66 | JPG/PNG/GIF/TIFF、**CR2/NEF/ARW**、PSD、SVG… | 🟢 多签名 + RAW 相机 | 🟡 **~40** 扩展名（含 RAW 缩略图尝试） | 🟡 JPG/PNG/GIF；HEIC/WebP/部分 RAW 靠 FFmpeg；**非**还原 RAW 原件 | 🟡 相机 RAW「能看不能修原格式」 |
| **视频** | 37 | MP4/MOV/AVI/MKV/WMV/FLV/3GP… | 🟢 | 🟡 **~15** 扩展名（首帧/Shell） | 🟡 MP4/MOV 结构修复 + FFmpeg 重封装 | 🟡 rmvb/vob 等预览弱 |
| **文档** | 111 | DOC/DOCX、XLS/XLSX、PPT/PPTX、**PDF**、HTML、EPUB… | 🟡 列表含 **doc/xls/ppt/pdf**；雕刻/元数据因格式而异 | 🟡 **Windows** 下 pdf/docx 等网格 **Shell 缩略图**（见 §6.5）；非内嵌渲染 | 🟡 **仅** docx/pptx/xlsx（ZIP 容器）；**无** 旧版 OLE **doc/xls/ppt** 修复 | 🟡 与 EaseUS 内嵌预览仍有差距 |
| **音频** | 37 | MP3/WAV/FLAC/AAC/OGG/M4A… | 🟡 签名+元数据 | 🔴 无波形/播放预览 | 🔴 无音频修复器 | 🔴 音频为 EaseUS 显性大类 |
| **压缩/镜像** | 37 | ZIP/RAR/7z/TAR/GZ、**ISO**… | 🟡 | 🔴 | 🟡 ZIP/JAR/APK；**无** RAR/7z 修复 | 🔴 RAR 仅扫描/恢复，不修 |
| **邮件** | 6 | **PST**/OST/EML/MSG/DBX… | 🟡 扩展名在树中 | 🔴 | 🔴 | 🔴 邮件预览/导出弱于 EaseUS |
| **书签/未保存/其他** | 27 | 系统文件等 | 🟡 | 🔴 | 🔴 | 非消费级卖点 |

### 6.4 修复扩展名清单（PhoenixRecover）

与 `repair/RepairFormatUtil.cpp` 一致（**45** 个扩展名，扫描右键「尝试修复」同此集合）：

| 子类 | 扩展名 |
|------|--------|
| 压缩/Office | zip, jar, apk, **docx, pptx, xlsx** |
| 图片 | jpg, jpeg, jfif, jpe, png, gif, webp, heic, heif, tif, tiff, bmp, dib |
| 相机 RAW（多为转码/预览） | cr2, nef, arw, orf, rw2, dng, raw, raf |
| 视频 | mp4, mov, m4v, m4a, avi, mkv, webm, flv, wmv, mpg, mpeg, m2ts, ts, 3gp, 3g2, asf, ogv, divx |

EaseUS 另售 **Video Repair / Photo Repair** 产品线；我方修复器在主线内，但覆盖深度与商用样本量仍 🟡。

### 6.5 预览扩展名清单（PhoenixRecover）

与 `GridPreviewExtensions` 一致：

- **图片**：jpg, jpeg, jfif, jpe, png, gif, bmp, dib, webp, ico, tif, tiff, heic, heif, avif, jp2, j2c, j2k, ppm, pgm, pbm, pam, xpm, tga, cr2, nef, arw, dng, orf, rw2, pef, srw, raf, 3fr, mrw, raw, psd, exr, hdr, svg  
- **视频**：mp4, m4v, mov, avi, mkv, wmv, webm, flv, mpg, mpeg, ts, m2ts, m2t, 3gp, 3g2  
- **文档（网格 Shell 缩略图，Windows）**：pdf, docx, pptx, xlsx, doc, ppt, xls, rtf, txt, md, odt, ods, odp, epub, html, htm  

**说明**：文档首帧依赖系统已安装预览处理程序；无缩略图时网格显示「无法预览」。

### 6.6 扫描覆盖 vs EaseUS：典型缺口（按扩展名）

| 扩展名/族 | EaseUS | PhoenixRecover | 优先级 |
|-----------|--------|----------------|--------|
| **pdf** | 扫描+预览 | UI/签名有；**网格 Shell 预览**（无修复） | P1 内嵌/首屏文本 |
| **doc / xls / ppt**（OLE） | 扫描+预览 | 元数据/雕刻可能找到；**无预览；修复仅 Office Open XML** | P1 |
| **rar / 7z** | 扫描+解压类恢复 | UI 树有 **rar、7z**；修复 **无** | P1 扫描误报率 |
| **pst / ost** | 邮件恢复卖点 | 树上有；**无专用解析/预览** | P2 |
| **ai / sketch / fig** 等设计稿 | 部分支持 | 树含 **ai** 等；依赖签名质量 | P2 |
| **ReFS / HFS+ / APFS** 卷 | 支持 | **ReFS 只读解析+恢复已接生产探测链**（`RefsParser`，CTest `RefsRecoverBytes` 80 项，合成金标含嵌套目录）；HFS+ / APFS **不支持**；真实 Windows ReFS 卷未验 | 🟡 ReFS 合成已通，真实卷待验；HFS+/APFS P2 |

### 6.7 如何复现统计（避免文档与代码漂移）

```powershell
# UI 321 扩展名均被 SignatureDB/别名规则覆盖
ctest -C Release -R UiTypeCoverage --test-dir build\regress

# 文档扩展名 + 预览读长（Qt）
ctest -C Release -R GridPreviewExtensions --test-dir build\regress

# CFReDS DFR-05（需 testdata/cfreds/*.dd）
ctest -C Release -R CFReDS_DFR05 --test-dir build\regress
```

发版前：**TYPE-P0-01** 必须 PASS；扩展名数量变化时同步改本文 §6.2–6.4。

---

## 7. 与 EaseUS 差距对照（逐项）

图例：**🟢 接近/达标** **🟡 有但弱** **🔴 明显落后** **⚪ 未评/缺数据**

| # | 能力点 | EaseUS | PhoenixRecover | 差距 | 建议优先级 |
|---|--------|--------|----------------|------|------------|
| 1 | 统一深度扫描（用户无感三阶段） | 🟢 | 🟢 已实现 `UnifiedVolumeScanner` | 小：阶段文案与耗时优化 | P1 |
| 2 | FAT/NTFS/exFAT 删除恢复 | 🟢 | 🟡 DFR-01 + **DFR-05** CTest；**W2 补 exFAT 自动探测链 `ExfatProbeDispatch`，W3 补 `ExfatRecoverBytes` 真实 exFAT 恢复字节/MD5 门禁并修复 CAR-003（已删连续流被 FAT 拼接器改写）**；仍缺多簇/碎片化 exFAT 门禁 | 中：补碎片化 exFAT / EXT 镜像 | **P0** |
| 3 | ext2/3/4 删除恢复 | 🟢 | 🟡 有 `Ext4Parser`，含 **JBD2 journal 只读回放**（可救回「元数据已提交未 checkpoint」的掉电窗口）；**已有 `CFReDS_EXT4_DFR01` 与 `DFR_Linux_Batch` CTest（W4 校正旧「无 CTest」表述）**；仍缺 ext2/3 独立金标样本、JBD2 checksum 校验、真实掉电卷 | 中 | P1 |
| 4 | 格式化 / 分区丢失 | 🟢 | 🟡 Deep+Raw 有；**W3 补合成金标 `FormattedVolumeRecovery`**（快速格式化模型，22 项，双工具链绿），已进 `expected_results.csv`；真实快速格式化磁盘仍未测 | 中 | P1 |
| 5 | 碎片 / 交织碎片 | 🟢 | 🟡 DFR-05 **元数据删除可见**已 CTest；**恢复字节/交织重组**仍有限 | 大 | **P0**（恢复写出） |
| 6 | 10 万+ 结果 UI | 🟢 | 🟡 有 `DESIGN_LargeResultUI`；缺实测 | 中 | **P0**（体验） |
| 7 | 恢复前预览（图/视/**文档**） | 🟢 | 🟡 图/视 + **文档 Shell**；PRV 手工矩阵未全绿 | 中 | **P0** |
| 8 | 同卷写保护 | 🟢 | 🟢 | — | 保持 |
| 9 | 重名/冲突策略 | 🟢 自动重命名常见 | 🟢 **自动 `name (n).ext`**（可选覆盖未做） | 小 | P2 |
| 10 | 批量恢复性能 | 🟢 | ⚪ 无基线数据 | 未知 | P1 |
| 11 | 文件修复（Office/zip/媒体） | 🟢 | 🟡 有修复器+自检；场景少于商用 | 中 | P1 |
| 12 | 扫描→修复一键 | 🟢 | 🟢 右键+staging | 小：可补工具栏 | P2 |
| 13 | 误报率（雕刻） | 🟢 长期调参 | 🟡 **CAR-002 已首度量化**（`RawScanFalsePositive`：8 MiB 随机噪声 14.38 FP/MiB，植入场景 precision 12.80% / recall 100%，见 `PERFORMANCE_BASELINE.md §9`）；**调参未做**，真实卷未测 | 中 | P1 |
| 14 | 多语言 / 向导 polish | 🟢 | 🟡 中文为主 | 大（商用） | P2 后 |
| 15 | 信任与运营（签名、支持、许可） | 🟢 | 🟡 NSIS/Runbook 有 | 非引擎 | 发布轨 |
| 16 | **文件类型数量（宣传口径）** | 🟢 **1000+** | 🟡 **514** 签名 / **321** UI 筛选 | 中：勿夸大；持续补签名 | P1 |
| 17 | **文档类预览**（doc/xlsx/pdf） | 🟢 | 🟡 网格 **Shell** 预览（§6.5）；非应用内渲染 | 中 | P1 |
| 18 | **音频类**（扫描+预览） | 🟢 | 🟡 扫描 🟡 / 预览 🔴 | 中 | P1 |
| 19 | **邮件 PST/OST** | 🟢 | 🟡 扩展名在树 / 无专精 | 中 | P2 |
| 20 | **旧版 Office doc/xls/ppt 修复** | 🟢 部分产品支持 | 🔴 仅 docx/pptx/xlsx | 中 | P1 |
| 21 | **RAR/7z**（恢复后可用性） | 🟢 | 🟡 扫描有 / 无修复 | 小 | P2 |
| 22 | **文件系统 HFS+/ReFS** | 🟢 | 🔴 未实现 | 大（Mac/Server） | P2 后 |

**结论（诚实）：** 引擎骨架与 EaseUS **同赛道**；达到「用户敢付钱」仍差 **碎片文件恢复字节成功率、大列表性能实测、预览手工矩阵全绿、文档内嵌预览、文件类型广度（1000+ vs ~500 条目）** 等硬功夫。类型对比细节见 **§6**。

---

## 8. 工程优先级（恢复赛道质量基线 / 并行轨）

> **定位变更后说明**：本节保留三主线内部的 P0/P1/P2 排序，作为「并行轨」的执行清单；**产品级优先级以 [ROADMAP_RECOVERY.md](./ROADMAP_RECOVERY.md) 的 A0–A5 为准**（广度优先，A0 写路径安全底座最高）。两者不冲突：本节管「恢复做得对」，A0–A5 管「功能补齐顺序」。

### P0 — 不做不算「接近 EaseUS」

1. ~~**RCV-P0-05**~~：**已完成**（`Utils::MakeUniqueFilePath` + `RecoverEngine`）  
2. **RCV-P0-06 / SCN-P0-07**：DFR-05 **元数据**已 CTest；**DFTT #6/#7 恢复字节**已 CTest（FAT 碎片仍弱）；**DFTT #11** 雕刻已 `DFTT_11_CARVE_FAT`  
3. **PRV-P0-01~02** + **TYPE-P0-02b**：预览手工全绿 + 文档 Shell 在干净机验证  
4. **UI 10 万行**：按 PERFORMANCE_BASELINE §4 填数并优化至阈值内  
5. **§5 矩阵状态列**：每 sprint 更新，发版前全 🟢  
6. **文档内嵌预览**（可选）：PDF 首屏 / docx 正文（超越 Shell）  

### P1 — 缩小场景差距

- CFReDS dfr-02/03 删除场景自动化  
- exFAT 与 ext4 的 DFR-01 集成测试：ext4 已有 `CFReDS_EXT4_DFR01`；**exFAT 探测链已由 `ExfatProbeDispatch` 覆盖（W2/W3），恢复字节门禁仍待办**  
- 雕刻误报率指标 + 调参  
- 修复：Office 样本进 `repair_samples.csv` + CTest  
- **签名库**：向 400+ **不重复扩展名** 推进（非单纯堆条目）；优先 **pdf、rar、主流音频**  

### P2 — 体验抛光（仍属主线）

- 扫描页工具栏「尝试修复」  
- 修复结果应用内预览（非仅系统默认打开）  
- 可恢复性列默认可见  
- 音频轻量预览（波形或系统 Shell）  

---

## 9. 如何跑回归（开发日常）

```powershell
# 无 GUI：单元 + 镜像（需 testdata 已下载）
.\scripts\Regress.ps1

# 仅修复
cmake --build build\regress --config Release --target test_repair_samples
cd build\regress; ctest -C Release -R Repair

# 恢复字节对比（示例，参数见脚本内帮助）
.\testdata\tools\Compare-RecoveryResults.ps1 -Strict
```

GUI 冒烟：按 [TEST_EXECUTION_MATRIX.md](./TEST_EXECUTION_MATRIX.md) SCN/RCV/PRV 手工列执行，结果写入 §5 状态列。

---

## 10. 文档维护

| 变更类型 | 更新位置 |
|----------|----------|
| 新增 P0 场景 | 本文 §5 + `datasets.csv` |
| 新 CTest | 本文 §4.4 + TEST_EXECUTION_MATRIX |
| 性能达标 | PERFORMANCE_BASELINE + 本文 §5.5 门禁 |
| **签名/UI/预览/修复扩展名变更** | 本文 **§6.2–6.5** + 跑 `UiTypeCoverage` |
| 范围变动 | 本文 §2 + `ROADMAP_RECOVERY.md` A0–A5（须评审；阶段状态随 sprint 更新） |
| **A0 写路径阶段状态 / 安全不变量变更** | `ROADMAP_RECOVERY.md §3.2 组件清单与验证记录` + 本文 §2 A0 硬门禁；每新增一条写能力同时提交回滚用例与前后哈希比对结果 |
| **A1 只读数据源 / 金标样本变更** | `ROADMAP_RECOVERY.md §4 A1` + `testdata/virtual-disk-golden/manifest.json` + 对应 CTest；新增或替换真实镜像须重跑 `Verify-VirtualDiskGolden.py` 重新生成 `expectations.tsv`，并复核负向验证仍生效 |
| **扫描数据源接线 / `SectorReader` 入口变更** | `ROADMAP_RECOVERY.md §4 A1-2` + `disk/VirtualDiskSectorSource.h` + `tests/test_virtual_disk_sector_source.cpp`；新增容器格式须同步扩展名路由、`RecoveryLocationPage` 过滤器、真实镜像金标用例与「打开前后镜像文件逐字节不变」断言；虚拟路径不得打开写句柄（S1） |
| **虚拟 RAID 布局 / 重建算法变更** | `ROADMAP_RECOVERY.md §4 A1-4a` + `disk/VirtualRaid.h` + `tests/test_virtual_raid.cpp`；任何布局公式、GF 参数或降级重建语义变更须重跑独立正向 oracle 全量比对与源码级负向验证，并复核「缺盘/坏扇区不静默零填充」不变量。**新增真实阵列支持前不得在 UI 暴露 RAID 入口** |
| **BitLocker 解密 / 密钥派生 / FVE 元数据解析变更** | `ROADMAP_RECOVERY.md §4 A1-3a` + `disk/BitLockerCrypto.h` / `disk/BitLockerVolume.h` + `tests/test_bitlocker_{crypto,volume}.cpp`；改动前用 `python testdata/tools/Gen-BitLockerGolden.py --verify-only` 复核金标完整性，改动后重跑独立 Python AES oracle 全量比对 + 源码级负向验证，并复核「零写方法、零设备句柄、打开前后源卷逐字节不变」不变量。**A1-3b 真实卷金标完成前不得在 UI 暴露 BitLocker 解锁入口，不得宣称支持真实卷恢复** |
| **ReFS 结构解析 / 校验链 / 数据重组变更** | `ROADMAP_RECOVERY.md §4 A1-5a` + `fs/refs/RefsCrypto.h` / `fs/refs/RefsVolume.h` + `tests/test_refs_{crypto,volume}.cpp`；改动前用 `python testdata/tools/Gen-ReFSGolden.py --verify-only` 复核金标完整性，改动后重跑 13 个合成卷全量比对（48 条文件期望 / 40 份内容双指纹）与源码级负向验证，并复核「VLCN 空洞不静默零填充、源缓冲逐字节不变、损坏副本按 Merkle 摘要拒绝」不变量。金标布局属**合成卷扁平化约定**，改动须同步生成器与 `ROADMAP` 覆盖边界说明。**A1-5b 真实卷金标完成前不得在 UI 暴露 ReFS 入口，不得宣称支持真实 ReFS 卷恢复** |
| **mdadm 元数据 / RAID 探测 / 组合源配置变更** | `ROADMAP_RECOVERY.md §4 A1-4b-a` + `disk/RaidProbe.h` / `disk/ScanSourceChain.h` + `tests/test_raid_probe.cpp` / `tests/test_scan_source_chain.cpp`；改动前用 `python testdata/tools/Gen-RaidProbeGolden.py --verify-only` 复核金标完整性，改动后重跑 16 个合成用例 + 20 条逐层双指纹 + 两重源码级负向验证。三条硬不变量不得放宽：**元数据与 parity 矛盾必须拒绝**、**盲探测同分/超预算必须拒绝**、**探测失败不得降级为单盘扫描**；配置解析须继续拒绝未知键/重复键/坏值，凭据不得进入 `Report()` / `Describe()` / `LastError()`。**A1-4b-b 真实阵列金标完成前不得在 UI 暴露 RAID 入口，不得宣称支持真实阵列恢复** |
| **签名多模式匹配器 / 按类型恢复参数化引擎变更** | `ROADMAP_RECOVERY.md §4 A1-6` + `scan/SignatureMatcher.h` + `tests/test_signature_matcher.cpp`；改动前用 `python testdata/tools/Gen-SignatureMatcherGolden.py --verify-only` 复核 `manifest.json` 的 SHA-256，改动后重跑 CTest #77（AC 单元属性 / 金标单缓冲 vs 独立 oracle 9 命中逐项相等 / 负向区零误报 / **流式 512 B 窗口 == 单缓冲**（含 `Finish()` 冲刷 footer）/ type-filter / alignment），并复核三条硬不变量：**跨窗口 magic 仅命中一次**、**footer 经 `Finish()` 冲刷定稿且跨窗口 OR 累积正确**、**源码级篡改转移表须 FAIL**。**A1-6/7/8 已接主路径并落地**（`ScanBatch` 流式 AC 替换线性 `memcmp`、`DeepScan` 损坏 NTFS 回退 RAW 雕刻、并行 RAW 扫描分块+合并），对应 CTest `RawScanAC` / `DeepScanFallback` / `RawScanParallel`；**A1-9 `SignatureDB::LoadFromFile` 已完成 Qt 解耦**（`std::ifstream` + 零依赖 `scan/SignatureJson.h`，headless 构建同样可用；CTest `SignatureDBLoad` 90 项）。改动 `scan/SignatureJson.h` 或加载逻辑须重跑该 CTest 并复核源码级负向验证（去掉 `m_matcherBuilt=false` 失效须导致 `Match()` / `ScanBatch()` 断言 FAIL）；运行时内建签名实测 **513** 条，§6 的 **514** 为 `UiTypeCoverage` 口径，勿混用 |
| **P0 W1–W5 回归基线 / exFAT 探测与恢复字节门禁 / 误报率变更** | `ROADMAP_RECOVERY.md §8` + `tests/test_exfat_probe_dispatch.cpp` / `tests/test_exfat_recover_bytes.cpp` / `tests/test_rawscan_false_positive.cpp` + `testdata/manifests/expected_results.csv` + `PERFORMANCE_BASELINE.md §9`；改动 `fs/exfat/ExfatParser.cpp` 的 `NoFatChain` 处理或拼接器调用条件后必须重跑 `ExfatRecoverBytes`（30 项），并复核两条硬不变量：**已删连续流簇链不得被 FAT 启发式拼接器改写**、**恢复写出字节须与独立手写 exFAT oracle 逐字节相等且整镜像 FNV-1a 不变**；两轮源码级负向验证（放开拼接器 / 恢复旧 `chainNoFat`）须各自 FAIL。GCC 7.3 与 GCC 14.2 双工具链均须编译通过；GCC14 回归命令须把 `/c/msys64/mingw64/bin` 置于 PATH 首位，否则 `std::filesystem` 相关 exe 会因旧 `libstdc++-6.dll` 抢先加载报 `0xc0000139` |
| **并行 RAW 扫描 / 生产接线变更** | `ROADMAP_RECOVERY.md §4 A1-10` + `scan/RawScan.{h,cpp}` / `scan/CarveModuleService.{h,cpp}` / `scan/UnifiedVolumeScanner.h` + `tests/test_rawscan_parallel_wiring.cpp`；改动后重跑 CTest `RawScanParallel` + `RawScanParallelWiring`，并复核三条硬不变量：**并行 worker 必须按 `m_db->GetAll()` 快照精确克隆签名集**（回退成默认 ctor 会使自定义库用例 serial=5 / parallel=63 而 FAIL）、**文件 / 进度回调只在调用线程发射**（回调不得进 worker）、**默认 `numThreads == 1` 保持串行零行为变化**。配置链 `UnifiedScanConfig.rawScanThreads` → `MakeCarveConfig` → `CarveConfig.numThreads` → `RawScan::SetThreadCount` 断开须触发源码级守卫 FAIL；`targetExtensions` 过滤与 `cancelled` 传播不得回退 |

---

## 11. 一句话对外表述

> PhoenixRecover 是集 **数据恢复、分区管理、备份还原与磁盘工具** 于一体的一体化工具，全面对标 DiskGenius；恢复赛道坚持 **扫描找得到、恢复写得对、损坏修得明**，所有写操作执行默认拒绝的授权与可回滚事务。

**对外表述约束**：在 `ROADMAP_RECOVERY.md` 各阶段门禁达标前，不得宣称与 DiskGenius「功能等效」或「恢复效果等效」。功能清单可以对齐，恢复算法效果与硬件兼容性属于长期打磨项（见 `ROADMAP_RECOVERY.md §6` 风险红线 4）。单个阶段完成后按阶段名称宣传，例如「已支持虚拟磁盘只读恢复」，不做整体等效表述。
