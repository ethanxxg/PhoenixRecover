# PhoenixRecover 变更日志

本文件记录 PhoenixRecover 各版本的面向用户变更。格式基于 [Keep a Changelog](https://keepachangelog.com/)。

---

## v1.0.0.20260527 (2026-05-27)

### 发布说明

- Windows 正式发布候选：NSIS 安装包、崩溃诊断（minidump + 日志归档）、文件修复 UI 闭环（ZIP/MP4 + 报告导出）。
- 发布门禁：`PreRelease.ps1` / `Regress.ps1` / `RepairSamplesIntegrity`；版本号统一由仓库根目录 `VERSION` 文件维护。

### 扫描 / 恢复 / 预览（主线）

- **恢复重名**：目标已存在时自动 `name (1).ext` 命名后继续写出（`Utils::MakeUniqueFilePath` + `RecoverEngine`）；CTest `UtilsUniquePath`。
- **文档网格预览**：pdf、docx、Office 等使用 **Windows Shell** 缩略图（`GridPreviewExtensions` + `MediaThumbnails::documentPreviewPixmapForLocalFile`）。
- **CFReDS DFR-05 回归**：`dfr-05-fat.dd` / `dfr-05-ntfs.dd` 上验证删除文件可见性；CTest `CFReDS_DFR05_FAT` / `CFReDS_DFR05_NTFS`（无镜像时 SKIP）。
- **签名单测**：`test_signature_db` 链接 `UiTypeSignatureSupplement`；PK 与 `%PDF` 头断言与当前签名顺序一致。
- **CTest**：`GridPreviewExtensions`（文档扩展名与预览读长）。
- **DFTT #11 雕刻**：`DFTT_11_CARVE_FAT`（镜像 MD5 + `paul.jpg` 字节黄金；缺镜像 SKIP）。
- **FAT 格式感知碎片重组（产品）**：`FatFragmentStitcher` + `FormatPayloadScore` 在 FAT 链断裂时按 JPEG/PNG/PDF/ZIP/MP4 等结构为候选簇链打分（不依赖黄金 MD5）；`RecoverOptions::formatAwareFatFragments` 默认开启；恢复页可勾选「格式感知碎片重组」（`QSettings` 键 `recover/formatAwareFragments` 持久化）。
- **碎片重组模块化**：新增 `FragmentStitchPolicy` + `FragmentStitchService` 作为唯一集成门面；`FatParser` / `RecoverEngine` / 扫描流水线不再直接依赖 `FatFragmentStitcher`；扫描与恢复共用同一 `QSettings` 开关。见 `docs/FRAGMENT_STITCH_MODULE.md`。
- **DFTT #6 恢复字节**：`DFTT_FAT_RecoverBytes`（`sing.dat`/`mult1.dat`/`frag1.dat`/`frag2.dat`/`mult2.dat`/`frag3.dat` 黄金 MD5）；修复 FAT 每簇只读 1 扇区。
- **DFTT #7 恢复字节**：`DFTT_NTFS_RecoverBytes`（`expected_results.csv` 全部 8 项 MD5）。
- **DFTT 黄金包**：`DFTT_Recover_GoldenBundle`（`Invoke-DfttRecoverGolden.ps1` 串行 FAT/NTFS recover-bytes；缺镜像 SKIP）。
- **DFR-05 恢复冒烟**：`CFReDS_DFR05_RecoverSmoke`（写出后按 manifest 大小校验；缺 `dfr-05-*.dd` SKIP）。
- **DFTT 黄金串跑**：`DFTT_Recover_GoldenBundle`（`testdata/tools/Invoke-DfttRecoverGolden.ps1`；缺镜像 SKIP）。
- **DFR-05 恢复冒烟**：`CFReDS_DFR05_RecoverSmoke`（写出后按 size 校验；缺 `dfr-05-*.dd` SKIP）。
- **FAT 链构建**：`BuildClusterChain` 在 EOC 后仅在簇数不足时才顺序扩展，避免误读后续簇。
- **RawScan 签名**：`504B0304` 优先匹配 `.zip`（结构雕刻走 `CarveZip`）。

### 新功能（相对早期原型）

- 文件修复页：批量修复、进度/取消、CSV/TXT 报告导出。
- 崩溃诊断包与 CTest `CrashDiagnostics`。
- 发布脚本：`Build-Release.ps1`、`Release-Windows.ps1`（Qt 5.14.2 msvc2017_64）。

---

## v0.1.0 (开发中，已并入 1.0.0.20260527)

### 新功能

- **智能多阶段扫描引擎**：三阶段流水线（FS 元数据解析 → 深度扫描 → 签名雕刻），用户无需手动选择扫描模式，自动执行最优策略
- **多文件系统支持**：NTFS / FAT12 / FAT16 / FAT32 / exFAT / ext4 文件系统元数据解析与文件发现
- **300+ 内建文件签名识别**：覆盖常见文档、图片、音视频、压缩包等格式
- **外部 JSON 签名文件加载**：支持用户自定义文件签名，扩展识别能力（详见 `docs/SIGNATURE_FORMAT.md`）
- **批量文件恢复**：含进度显示、实时取消支持、同卷写保护校验
- **恢复完成视图**：统计摘要 + 文件明细表 + 导出恢复报告
- **文件修复**：ZIP；**Office docx/pptx/xlsx**（ZIP 重建 + 核心部件自检）；图片 JPEG/PNG/GIF + FFmpeg 扩展；视频 MP4/MOV + FFmpeg 容器；修复报告含 **validated** 字段
- **磁盘镜像功能**：支持将物理磁盘或分区导出为镜像文件
- **S.M.A.R.T. 磁盘健康监测**：读取并展示磁盘健康状态指标
- **搜索 / 筛选 / 排序**：基于 QTableView + Model/View 架构，支持文件名模糊搜索、扩展名筛选、大小范围、日期范围、可恢复性等级过滤
- **可恢复性分级显示**：以颜色标识文件恢复成功概率（正常/已重建/已删除）
- **同卷写保护**：恢复前自动校验目标路径，防止写入源磁盘造成二次损坏
- **会话保存 / 恢复**：扫描结果自动持久化，下次启动可继续上次工作
- **跨平台基础**：SectorReader / DiskManager 完成 Windows / Linux / macOS 平台拆分

### 改进

- 扫描模式收敛为单一流水线，用户无需选择模式，降低使用门槛
- RecoveryScanPage 拆分为 ScanProgressBar / PreviewPanel / ScanFooterBar / FilterPanel 四个子组件，提升可维护性
- 扫描进度链路增加阶段文本透传（`阶段 1/3 元数据解析` 等），提升过程可观测性
- 恢复完成展示从简单弹窗升级为结构化完成视图
- 恢复明细支持导出为 UTF-8 文本报告
- CTest 单元测试 + DFTT 镜像端到端集成测试，自动化回归脚本覆盖

### 已知限制

- Linux / macOS 构建尚未完整验证，Unix 平台实现为 v0 基础版本
- 大结果集（>10 万条）性能优化仍需实测调优
- 加密 ZIP、ZIP64、分卷 ZIP 暂不支持；JPEG/PNG 仅做常见截断/头尾修复，严重损坏可能仍无法打开
- 部分视频/图片格式依赖 FFmpeg（可置于程序目录或 PATH）
- Grid 预览视图尚未迁移到 Model/View 架构
- 不支持加密 ZIP、ZIP64、分卷 ZIP 的修复
- MP4 修复不处理加密内容或完全无 box 结构的裸数据
