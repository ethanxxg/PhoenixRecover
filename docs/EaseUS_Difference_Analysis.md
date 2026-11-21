# PhoenixRecover 对标 EaseUS 的差异化分析

| 对标 EaseUS 功能点 | EaseUS 通常做法（概览） | 你当前代码状态 | 主要差异化/缺口（证据指向） |
|---|---|---|---|
| 快速/深度/RAW 扫描模式选择 | UI 上通常有"快速/深度/RAW(签名/文件雕刻)"切换，并决定走不同引擎与策略 | 已收敛为单一扫描流水（`UnifiedVolumeScanner`），内部分阶段执行；不再向用户暴露模式选择，符合产品需求 | [已关闭] `RecoveryScanMode` 已删除（LOG-20260402-002）；`UnifiedVolumeScanner` 为唯一高层入口，UI 展示阶段标签（LOG-20260402-006） |
| 扫描引擎路由（Quick/Deep/Raw） | 根据用户选择切换不同扫描引擎与流程（可能串行/并行） | 已统一为 `UnifiedVolumeScanner` 单一入口；`QuickScan`/`DeepScan`/`RawScan` 作为内部阶段存在，不再由用户选择触发 | [已关闭] Worker 收敛为 `UnifiedVolumeScanner`（LOG-20260402-002），模式路由逻辑已移除 |
| 扫描取消/暂停 | EaseUS 通常有"停止/取消扫描"，并能尽快终止后台任务 | 已实现取消：`std::atomic<bool> cancelFlag_` 从 UI → Controller → Worker → `SectorReader::SetCancelFlag` 全链路贯通（LOG-20260402-001）；暂停尚未实现 | [部分关闭] 取消已落地（`SectorReader::ReadSectors` 在循环中检查取消标志并以 `ERROR_OPERATION_ABORTED` 返回）；暂停功能仍缺失 |
| 扫描范围（目标类型） | EaseUS 通常能处理更多"位置类型"（回收站/桌面/指定目录/外接设备等）并给出对应扫描方式 | 回收站/桌面/选择文件夹进入扫描页后，直接提示"不支持，仅本地磁盘扫描" | `RecoveryScanPage::setNonDiskPlaceholder()` 里写死了"当前版本仅支持从本地磁盘进入扫描" |
| 结果列表中文件选择（单/多选/勾选） | 通常支持勾选多个文件、全选/反选、按类型快速选择 | 已实现勾选模型（`QTableWidget` + CheckBox），支持按勾选收集文件 ID 并批量恢复（LOG-20260402-003） | [已关闭] `onRecoverAllClicked()` 从表格勾选框收集可恢复文件 ID，支持批量选择与恢复 |
| "恢复到…"目标路径选择 | EaseUS 允许选择保存目录，并校验目标不可覆盖源（或给提示） | 已实现完整闭环：目录选择对话框 + `RecoverEngine::ValidateTargetPath` 同卷校验 + `RecoverEngine::RecoverBatch` 批量恢复 + `QProgressDialog` 进度（LOG-20260402-003） | [已关闭] `recoverAllBtn_` 已连接 `onRecoverAllClicked()`，完成目录选择→同卷校验→批量恢复→进度展示全链路 |
| 恢复进度与结果回显 | EaseUS 会显示恢复进度、成功/失败数量、失败原因、可重试等 | 已实现：`QProgressDialog` 实时进度 + 恢复完成视图（成功/失败/跳过统计 + 明细表 + "导出明细…"按钮 + "打开目录"按钮）（LOG-20260402-003/004/005） | [已关闭] `RecoverEngine::RecoverBatch` 已被 UI 调用；完成视图展示统计摘要与逐文件状态/原因明细，支持导出 UTF-8 报告 |
| 预览/打开文件 | EaseUS 多数支持预览（图片/视频/文档）或"打开查看" | 已实现缩略图预览模式：列表/网格视图切换（`viewToggleBtn_`）、`ScanPreviewKind` 多策略缩略图加载（ResidentBlob/VolumeSlice/FullNode）、图片与视频缩略图渲染；原 `openBtn` 已移除，无独立文件打开功能 | [部分关闭] 图片/视频缩略图预览已落地（`loadPreviewThumbBytes` + `MediaThumbnails`）；但无法对单个文件执行"打开/全屏预览"，文档类预览不支持 |
| 扫描结果的"路径视图/类型视图"同步与交互一致性 | EaseUS 通常两种视图都能导航到同一结果集，并保持选择上下文 | 实现了"路径树 + 类型树 + 右侧表格"，但筛选/同步能力偏基础 | `RecoveryScanPage` 里提供了 path/type 两棵树，并可按当前 tab 重建表格，但缺少 EaseUS 级别同步策略（例如搜索筛选后的联动选择集合复用） |
| 搜索与筛选（按文件名/大小/日期/类型） | EaseUS 通常有搜索框、筛选面板，可能支持按扩展名/大小区间等 | UI 上有 `filterBtn` 和 `searchEdit` 控件，但未接入树/表的真实过滤逻辑（仍为纯展示） | [未关闭] `filterBtn` 和 `searchEdit` 仅存在于 UI，未关联到 `QTreeWidget` / `QTableWidget` 的过滤流程 |
| 大结果量性能（虚拟化/增量渲染/分页） | EaseUS 对超大量文件会做性能优化（虚拟列表、增量装载、避免卡顿） | 未改进；仍使用 `QHash + QTreeWidget/QTableWidget` 直接插入，无虚拟化/分页策略 | [未关闭] `appendScanFileBatch` 仍逐项 `addChild` / `insert`，海量结果存在卡顿风险（缺少延迟渲染/虚拟化） |
| 数据可靠性/可恢复性分级可视化 | EaseUS 往往标注"可恢复/可能恢复/不可恢复"并影响恢复动作 | 底层 `QuickScan` 计算 `FileNode::recoverability`，但 UI 表格列仍未展示分级信息或可视化样式 | [未关闭] `recoverability` 值已在底层计算，但 `RecoveryScanPage` 表格未增加对应列或颜色/标签展示 |
| EaseUS 常见的多阶段工作流（扫描→预览→选择→恢复→完成页） | 通常有恢复完成页/总结、回到上一步、结果导出等 | 恢复完成页已实现（`recoveryCompletePanel_` 覆盖层：统计摘要 + 明细表 + "打开目录"/"导出明细…"/"返回扫描结果"按钮）（LOG-20260402-005）；但恢复阶段仍为扫描页内覆盖层，非独立路由页面 | [部分关闭] 工作流覆盖：位置页→扫描页→恢复→完成视图；完成页已落地但以扫描页覆盖层实现，尚未独立为路由页面 |
