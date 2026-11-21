# PhoenixRecover 测试执行矩阵（可落地版）

> 配合 [TEST_DATA_SOURCES.md](./TEST_DATA_SOURCES.md) 使用。  
> **产品范围（仅扫描/恢复/修复）与 EaseUS 对标门禁**见 [PRODUCT_SCOPE_SCAN_RECOVER_REPAIR.md](./PRODUCT_SCOPE_SCAN_RECOVER_REPAIR.md)。  
> 目标：把"有样本"转成"可执行、可复现、可追踪"的回归流程。

## 1. 使用方式

- **先准备样本**：按 `TEST_DATA_SOURCES.md` 的 P0 / MVP 拉取一批数据。
- **再跑矩阵**：按本表逐项执行，记录结果与证据（日志/截图/导出报告）。
- **最后回填**：把每轮结果写入 `testdata/manifests/datasets.csv` 或你们内部测试系统。

## 2. 执行矩阵（V1）

### 自动化覆盖说明

- **CTest 自动**：已有 CTest 注册用例覆盖，可通过 `scripts/Regress.ps1` 自动执行
- **半自动**：可通过脚本辅助但需人工判断结果
- **手工执行**：需人工操作 GUI 进行验证

| 用例ID | 模块 | 目标能力 | 推荐数据源 | 最小步骤 | 通过标准 | 自动化状态 | 最近执行 |
|---|---|---|---|---|---|---|---|
| SCN-001 | 扫描 | 单卷扫描可启动/可结束 | DFTT FAT/NTFS 基础镜像 | 选盘 → 开始扫描 → 等待完成 | 状态从"扫描进行中"变"扫描完成"；无崩溃 | CTest 自动（DFTT_FAT_Undelete / DFTT_NTFS_Undelete） | (待执行) |
| SCN-002 | 扫描 | 阶段化进度文案可见 | 任意中等镜像（>1GB） | 开始扫描，观察顶部状态 | 显示 `扫描进行中 · 阶段 1/3 元数据解析`（或后续阶段文案） | 手工执行 | (待执行) |
| SCN-003 | 扫描 | 取消生效 | 大镜像（Digital Corpora） | 扫描中点击"停止" | 任务在可接受时间内停止，UI无卡死 | 手工执行 | (待执行) |
| SCN-004 | 结果树 | 路径树/类型树联动 | DFTT + CFReDS 混合 | 扫描完成后切换路径/类型、搜索 | 列表随选择变化，勾选状态一致 | 手工执行 | (待执行) |
| SCN-005 | 结果集 | 大量结果下滚动与刷新 | Govdocs1 子集 / 大镜像 | 快速滚动、切换 tab、搜索 | 无明显卡死；结果数量和选择状态正确 | 手工执行 | (待执行) |
| RCV-001 | 恢复 | 恢复目录选择 + 同卷保护 | 任意可恢复镜像 | 选同卷目录恢复 | 弹出阻止提示，拒绝在源卷恢复 | 手工执行 | (待执行) |
| RCV-002 | 恢复 | 按勾选批量恢复 | DFTT Undelete | 勾选多文件 → 恢复全部 | 成功/失败/跳过统计可见，目标目录有输出文件 | 半自动（Compare-RecoveryResults.ps1 -Strict） | (待执行) |
| RCV-003 | 恢复 | 保留目录结构开关 | 有目录层级的样本 | 分别开/关"保留原目录结构"恢复 | 开启时保留层级；关闭时平铺输出 | 手工执行 | (待执行) |
| RCV-004 | 恢复 | 恢复完成页（覆盖层） | 任意恢复任务 | 恢复后查看完成视图 | 统计、明细表、返回按钮可用 | 手工执行 | (待执行) |
| RCV-005 | 恢复 | 明细导出 | 任意恢复任务 | 点击"导出明细..." | 生成 UTF-8 文本，内容含状态/目标/原因 | 手工执行 | (待执行) |
| PRV-001 | 预览 | 图片缩略图 | Xiph 图像样本 / 常见图片 | 切预览模式 | 常见格式可显示，失败样本有"无法预览"降级 | 手工执行 | (待执行) |
| PRV-002 | 预览 | 视频首帧缩略图 | Xiph/BBB 小视频样本 | 切预览模式等待生成 | 可见首帧或合理失败提示，无崩溃 | 手工执行 | (待执行) |
| PRV-003 | 预览 | 大文件预览限流 | 较大视频样本 | 观察预览速度与内存占用 | 无明显内存失控；UI可交互 | 手工执行 | (待执行) |
| CAR-001 | 雕刻 | 基础 carving 命中 | DFTT Basic Carving #1/#2 | 扫描后查看命中类型 | 能发现对应签名类型文件 | 手工执行 | (待执行) |
| CAR-002 | 雕刻 | 误报控制 | 混合噪声样本 | 统计无效结果比例 | 误报率在团队阈值内（阈值由版本定义） | 手工执行 | (待执行) |
| REP-001 | 报告 | 日志完整性 | 任意全流程样本 | 扫描+恢复+导出各跑一次 | 日志含关键阶段、耗时与错误信息 | 手工执行 | (待执行) |

### 自动化覆盖率统计

| 类别 | 用例数 | CTest 覆盖 | 半自动 | 需手工 |
|------|--------|-----------|--------|--------|
| 扫描 | 5 | 1 (SCN-001) | 0 | 4 |
| 恢复 | 5 | 0 | 1 (RCV-002) | 4 |
| 预览 | 3 | 0 | 0 | 3 |
| 雕刻 | 2 | 0 | 0 | 2 |
| 报告 | 1 | 0 | 0 | 1 |
| **合计** | **16** | **1 (6%)** | **1 (6%)** | **14 (88%)** |

### CTest 已注册用例清单（节选；以 `tests/CMakeLists.txt` 为准）

| CTest 名称 | 类型 | 数据源 | 验证内容 |
|------------|------|--------|----------|
| UtilsNumeric | 单元 | 无 | ReadLE / AlignUp / ReadSignedLE |
| UtilsUniquePath | 单元 | 无 | 恢复目标冲突自动重命名路径 |
| FatStructs | 单元 | 无 | FAT 结构体 sizeof / 常量 / FatDateTimeToFiletime |
| NtfsStructs | 单元 | 无 | NTFS 结构体 sizeof / DecodeDataRuns |
| CarveCommon | 单元 | 无 | CarveResult 结构体默认值 / 赋值 |
| ScanProgress | 单元 | 无 | ScanProgress / UnifiedScanConfig 默认值 |
| UiTypeCoverage | 单元 | 无 | UI 类型树扩展名均被签名/别名覆盖 |
| SignatureDB | 单元 | 无 | JPEG/PNG/ZIP/PDF 等魔数匹配 |
| GridPreviewExtensions | 单元 | 无 | 文档扩展名 + 预览读长上限 |
| DFTT_FAT_Undelete | 集成 | DFTT #6 (6 MB) | FatParser 删除文件可见性与大小 |
| DFTT_FAT_RecoverBytes | 集成 | DFTT #6 | `sing.dat`/`mult1.dat`/`frag1.dat`/`frag2.dat`/`mult2.dat`/`frag3.dat` 写出后 MD5 |
| DFTT_11_CARVE_FAT | 集成 | DFTT #11（可选） | RawScan + `paul.jpg` 黄金 MD5 |
| DFTT_NTFS_Undelete | 集成 | DFTT #7 (6 MB) | NtfsParser 删除文件可见性与大小 |
| DFTT_NTFS_RecoverBytes | 集成 | DFTT #7 | 写出 MD5（`expected_results.csv` 8 项全检） |
| DFTT_Recover_GoldenBundle | 集成 | DFTT #6/#7（可选） | PowerShell 串行调用 FAT/NTFS recover-bytes exe |
| PartitionProbe | 集成 | CFReDS + DFTT #10 | SectorReader / MBR 解析 / VBR 文件系统检测 |
| CFReDS_FAT_DFR01 | 集成 | NIST DFR-01-FAT (1 GB) | FAT12/16/32 三分区删除恢复 |
| CFReDS_NTFS_DFR01 | 集成 | NIST DFR-01-NTFS (1 GB) | NTFS 删除恢复 |
| CFReDS_DFR05_FAT | 集成 | NIST DFR-05-FAT（可选） | 碎片场景 FAT 删除文件可见 |
| CFReDS_DFR05_NTFS | 集成 | NIST DFR-05-NTFS（可选） | 碎片场景 NTFS 删除文件可见 |
| CFReDS_DFR05_RecoverSmoke | 集成 | DFR-05 FAT/NTFS（可选） | 删除文件写出后按 manifest 大小校验（无黄金 MD5） |
| CrashDiagnostics | 单元 | 无 | 崩溃诊断打包 |
| RepairSamples / RepairSamplesIntegrity | 集成 | repair_samples | 修复样本 MD5 与引擎 |

## 3. 版本门禁建议（Release Gate）

每次发版前至少满足：

- **P0 必跑**：`SCN-001/002/003`、`RCV-001/002/004/005`、`PRV-001`、`REP-001`
- **通过率**：P0 用例通过率 = 100%
- **阻断条件**：
  - 同卷保护失效
  - 恢复过程崩溃或产物大面积损坏
  - 扫描无法取消或 UI 长时间无响应

## 4. 结果记录模板（建议）

可复制下表到你们测试系统：

| 字段 | 示例 |
|---|---|
| run_id | `RUN-2026-04-02-01` |
| build_version | `v0.2.0-dev+abc123` |
| case_id | `RCV-002` |
| dataset_id | `DFTT-NTFS-UNDELETE-01` |
| os | `Windows 10 22H2` |
| result | `PASS / FAIL / BLOCKED` |
| elapsed_ms | `182340` |
| evidence | `log path / screenshot path / report path` |
| notes | `失败文件均为已覆盖簇` |

## 5. 与现有文档关系

- 数据来源：见 [TEST_DATA_SOURCES.md](./TEST_DATA_SOURCES.md)
- 总体计划：见 [TECHNICAL_PLAN.md](./TECHNICAL_PLAN.md)
- 实施流水：见 [DEVELOPMENT_LOG.md](./DEVELOPMENT_LOG.md)
- 性能基准：见 [PERFORMANCE_BASELINE.md](./PERFORMANCE_BASELINE.md)
