# PhoenixRecover 产品需求文档（PRD）

> **版本**: v1.1  
> **产品名称**: PhoenixRecover  
> **对标产品**: EaseUS Data Recovery Wizard  
> **技术栈**: C++ / Qt Widgets (MSVC / GCC / Clang)  
> **平台**: Windows 10/11 (x64) [P0] · Linux (x64) [P1] · macOS (x64 / Apple Silicon) [P2]

---

## 1. 产品概述

### 1.1 产品定位
PhoenixRecover 是一款跨平台本地数据恢复工具，对标 EaseUS Data Recovery Wizard，提供从本地磁盘、外接存储设备中恢复误删除、格式化、分区丢失等场景下的文件恢复能力。首发平台为 Windows，后续扩展至 Linux 和 macOS。

### 1.2 目标用户
| 用户群体 | 典型场景 |
|----------|----------|
| 普通个人用户 | 误删文件、清空回收站、U盘/SD卡文件丢失 |
| 摄影/视频创作者 | SD卡照片恢复、视频文件修复 |
| 企业IT运维 | 磁盘故障数据抢救、分区丢失恢复 |
| 技术爱好者 | 多阶段深度扫描、签名雕刻恢复、磁盘镜像取证 |

### 1.3 核心价值主张
- **三步恢复**: 选位置 → 扫描 → 恢复，操作门槛极低
- **智能多阶段扫描引擎**: 单一扫描流水自动分阶段执行（FS 元数据解析 → 深度扫描 → 签名雕刻），无需手动选择模式
- **可视化预览**: 恢复前预览文件内容，减少无效恢复
- **恢复概率分级**: 明确标注文件可恢复性（高/中/低），指导用户决策

---

## 2. 功能架构总览

```
PhoenixRecover
├── 数据恢复（核心流程）
│   ├── P1: 恢复位置选择页
│   ├── P2: 扫描进度页
│   ├── P3: 扫描结果 & 预览页
│   ├── P4: 恢复目标选择页
│   └── P5: 恢复完成页
├── 辅助工具
│   ├── 磁盘镜像（Disk Imaging）        → 详见 §6
│   ├── S.M.A.R.T. 磁盘健康监测          → 详见 §7
│   ├── SD卡专用恢复引导
│   ├── WinPE 启动盘制作
│   └── 文件修复（照片/视频/文档）        → 详见 §5
└── 系统设置
    ├── 默认恢复路径
    ├── 扫描线程数
    ├── 会话自动保存
    └── 多语言支持
```

---

## 3. 核心恢复流程 — 详细需求

### 3.1 P1: 恢复位置选择页（RecoveryLocationPage）

**页面职责**: 用户选择扫描目标；扫描模式由引擎内部自动决定，无需用户干预。

#### 3.1.1 快捷位置区域
| 选项 | 说明 | 优先级 |
|------|------|--------|
| 回收站（Recycle Bin） | 扫描 `$Recycle.Bin` 目录 | P0 |
| 桌面（Desktop） | 扫描当前用户桌面目录 | P0 |
| 选择文件夹（Select Folder） | 弹出目录选择对话框，扫描指定目录 | P0 |
| 外接设备（External Device） | 检测并列出 USB/SD/移动硬盘 | P1 |

#### 3.1.2 磁盘列表区域
- 枚举本机所有逻辑分区（盘符 + 文件系统类型 + 容量/已用）
- 每个磁盘项显示：
  - 盘符与卷标（如 `Local Disk (C:)`）
  - 文件系统类型（NTFS / FAT32 / exFAT / ext4）
  - 容量进度条（已用 / 总容量）
- 支持选中单个磁盘作为扫描目标
- 需调用 `PartitionProbe` 获取分区信息，调用 `SysInfo` 获取磁盘详情

#### 3.1.3 扫描流程说明
扫描采用**单一扫描流水线**，用户无需手动选择模式。点击"开始扫描"后，引擎内部自动按以下阶段依次执行：

| 阶段 | 内部引擎 | 说明 |
|------|----------|------|
| 阶段 1：FS 元数据解析 | `QuickScan` | 解析文件系统元数据（MFT / FAT 表 / exFAT inode），快速发现可恢复文件 |
| 阶段 2：深度扫描 | `DeepScan` | 全盘逐扇区深度分析，发现已删除区域中的残留文件 |
| 阶段 3：签名雕刻 | `RawScan` | 基于 `SignatureDB` 的文件签名（Magic Bytes）进行无文件系统雕刻 |

每个阶段发现的文件实时追加到结果列表，用户可在扫描过程中随时浏览已发现的文件，也可在任意阶段暂停或停止扫描。

#### 3.1.4 交互行为
- 选中位置后，"Start Scan" 按钮可用
- 点击 "Start Scan" → 发射信号 `requestStartScan(locationOption, diskLetter)`
- 页面不执行任何扫描逻辑（遵循 STYLE_GUIDE 规范）

#### 3.1.5 信号定义
```cpp
signals:
    void requestStartScan(const QString &locationOption,
                           const QString &diskLetter);
```

---

### 3.2 P2: 扫描进度页（RecoveryScanPage — 扫描中状态）

**页面职责**: 展示扫描实时进度，允许用户边扫描边浏览已发现的文件。

#### 3.2.1 顶部状态栏
| 信息项 | 数据来源 |
|--------|----------|
| 扫描目标 | locationOption + diskLetter |
| 当前阶段 | 当前正在执行的扫描阶段名称（如"FS 元数据解析"/"深度扫描"/"签名雕刻"） |
| 已发现文件数 | `foundCount` (来自 Worker 信号) |
| 已扫描容量 / 总容量 | `scannedBytes / totalBytes` |
| 已用时间 | 页面本地计时器 |
| 进度百分比 | `percent` (0-100) |

#### 3.2.2 扫描控制按钮
| 按钮 | 行为 | 优先级 |
|------|------|--------|
| **暂停（Pause）** | 暂停扫描引擎，保留当前状态 | P0 |
| **停止（Stop）** | 终止扫描，保留已发现结果 | P0 |
| **查看结果（View Results）** | 边扫描边跳转到结果浏览（不中断扫描） | P1 |

#### 3.2.3 实时文件列表
- 左侧：路径视图（Path View）/ 类型视图（Type View）双Tab
  - **路径视图**: 按磁盘目录层级展示 QTreeWidget
  - **类型视图**: 按文件类别分组（Documents / Images / Videos / Audio / Archives / Others）
- 中间：文件表格（QTableWidget），列定义：

| 列名 | 宽度 | 说明 |
|------|------|------|
| 勾选框 | 30px | 复选框，支持全选/反选 |
| 文件名 | 自适应 | 文件名 + 扩展名 |
| 大小 | 80px | 人类可读格式（KB/MB/GB） |
| 类型 | 80px | 文件类型分类 |
| 修改日期 | 100px | YYYY-MM-DD 格式 |
| 恢复概率 | 80px | High(绿) / Medium(橙) / Low(红) |
| 发现阶段 | 80px | 标注文件由哪个扫描阶段发现（元数据 / 深度 / 签名） |

- 右侧：文件详情面板
  - 预览区域（图片缩略图 / 文档预览 / 文件图标占位）
  - 文件属性（名称、大小、类型、日期、路径、恢复概率）

#### 3.2.4 搜索与筛选
| 功能 | 说明 | 优先级 |
|------|------|--------|
| 文件名搜索 | 按文件名/扩展名关键字实时过滤 | P0 |
| 文件类型筛选 | 按类别快速过滤（Filter 下拉） | P0 |
| 大小范围筛选 | 按文件大小区间过滤 | P1 |
| 日期范围筛选 | 按修改日期区间过滤 | P1 |
| 恢复概率筛选 | 按 High/Medium/Low 过滤 | P1 |

#### 3.2.5 数据批量追加性能
- 采用 batch 模式追加文件（`appendScanFileBatch`），每批 100-500 条
- 大结果集（>10万文件）需考虑虚拟化渲染或分页加载
- 树节点延迟展开（lazy expand），避免一次性构建全树

#### 3.2.6 信号定义
```cpp
signals:
    void requestBack();       // 返回位置选择页
    void requestStopScan();   // 停止/取消扫描
    void requestPauseScan();  // 暂停扫描
    void requestResumeScan(); // 恢复扫描

public slots:
    void appendScanFileBatch(const QVector<ScanFileDto> &batch);
    void setScanProgress(int percent, quint32 foundCount, 
                         quint64 scannedBytes, quint64 totalBytes);
    void setScanStageChanged(const QString &stageName);  // 阶段切换通知
    void setScanFinished(bool ok, const QString &message);
```

---

### 3.3 P3: 扫描结果 & 预览页（RecoveryScanPage — 扫描完成状态）

**页面职责**: 完整展示扫描结果，支持预览、多选、导出。

#### 3.3.1 结果统计摘要
- 顶部显示：总发现文件数、总大小、扫描耗时
- 文件类型快速切换标签（All / Documents / Images / Videos / Audio / Archives / Others）

#### 3.3.2 文件选择能力
| 功能 | 说明 | 优先级 |
|------|------|--------|
| 复选框多选 | 每行一个勾选框 | P0 |
| 全选/反选 | 表头复选框控制 | P0 |
| 按类型快选 | 点击类型标签自动勾选该类型所有文件 | P1 |
| 选中计数 | 顶部实时显示"已选 N 个文件 (XX MB)" | P0 |

#### 3.3.3 文件预览
| 文件类型 | 预览方式 | 优先级 |
|----------|----------|--------|
| 图片（JPG/PNG/BMP/GIF） | 右侧面板缩略图预览 | P0 |
| 文本（TXT/CSV/LOG） | 右侧面板文本预览（前 1KB） | P1 |
| 文档（DOCX/PDF） | 显示文件图标 + 元数据 | P2 |
| 视频（MP4/AVI） | 显示文件图标 + 帧截图（如可用） | P2 |
| 其他 | 显示文件类型图标 | P0 |

#### 3.3.4 会话管理
| 功能 | 说明 | 优先级 |
|------|------|--------|
| 导出文件列表 | 导出为 CSV 文件 | P1 |
| 保存扫描会话 | 序列化扫描结果到文件（`ScanSerializer`） | P1 |
| 加载扫描会话 | 从已保存文件恢复扫描结果 | P1 |

#### 3.3.5 操作按钮
- **"Recover Selected"**: 携带选中文件列表，跳转到 P4
- **"New Scan"**: 返回 P1 重新选择
- **"Export List"**: 导出结果到 CSV
- **"Save Session"**: 保存当前扫描状态

#### 3.3.6 信号定义
```cpp
signals:
    void requestRecover(const QVector<quint64> &selectedFileIds);
    void requestExportList(const QString &exportPath);
    void requestSaveSession(const QString &savePath);
    void requestLoadSession(const QString &loadPath);
```

---

### 3.4 P4: 恢复目标选择页（RecoveryTargetPage）

**页面职责**: 用户选择恢复文件的保存位置。

#### 3.4.1 恢复目标选项
| 选项 | 说明 | 优先级 |
|------|------|--------|
| 本地文件夹 | 选择不同于源盘的本地目录 | P0 |
| 外接设备 | 保存到 USB / 移动硬盘 | P0 |
| 网络位置 | 保存到网络共享 / NAS | P2 |

#### 3.4.2 安全校验
- **源盘覆盖检测**: 如果用户选择的目标路径与扫描源盘相同，弹出警告
- **磁盘空间检测**: 目标磁盘剩余空间 < 待恢复总大小时，显示空间不足警告
- **路径合法性检测**: 目标路径必须可写

#### 3.4.3 恢复摘要
- 显示：待恢复文件数量、总大小
- 文件列表缩略展示（前5个文件名 + "等 N 个文件"）

#### 3.4.4 信号定义
```cpp
signals:
    void requestStartRecover(const QString &targetPath,
                              const QVector<quint64> &fileIds);
    void requestBack();  // 返回结果页
```

---

### 3.5 P5: 恢复完成页（RecoveryCompletePage）

**页面职责**: 展示恢复结果摘要。

#### 3.5.1 结果统计
| 信息项 | 说明 |
|--------|------|
| 成功数 | 成功恢复的文件数量（绿色） |
| 失败数 | 恢复失败的文件数量（红色） |
| 总大小 | 实际恢复的数据总量 |
| 恢复路径 | 目标文件夹路径 |

#### 3.5.2 详细恢复日志
- 逐文件列出恢复状态（成功 / 失败 + 失败原因）
- 失败文件支持"重试"操作

#### 3.5.3 操作按钮
| 按钮 | 行为 |
|------|------|
| **打开文件夹** | 在资源管理器中打开恢复目标目录 |
| **新建恢复** | 返回 P1 开始新的恢复流程 |
| **查看日志** | 打开详细恢复日志 |

#### 3.5.4 信号定义
```cpp
signals:
    void requestOpenFolder(const QString &path);
    void requestNewRecovery();
    void requestRetryFailed(const QVector<quint64> &failedFileIds);
```

---

## 4. 辅助工具模块需求

### 4.1 磁盘镜像（Disk Imaging）

**页面**: DiskImagingPage  
**详细需求**: 见 [§6 磁盘镜像功能需求](#6-磁盘镜像功能需求)

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 源盘选择 | 列出所有物理磁盘和分区 | P0 |
| 镜像类型 | 全盘镜像 / 分区镜像 | P0 |
| 目标路径 | 镜像文件保存位置 | P0 |
| 进度显示 | 实时进度条 + 速度 + 剩余时间 | P0 |
| 坏扇区处理 | 遇到坏扇区时跳过并记录（`BadSectorMap`） | P1 |
| 镜像校验 | 完成后可选 MD5/SHA256 校验 | P2 |

**底层模块**: `disk/Imager.h`, `disk/SectorReader.h`, `disk/BadSectorMap.h`

### 4.2 S.M.A.R.T. 磁盘健康监测

**页面**: DiskHealthPage  
**详细需求**: 见 [§7 S.M.A.R.T. 磁盘健康监测需求](#7-smart-磁盘健康监测需求)

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 磁盘列表 | 显示所有物理磁盘及健康状态标签 | P0 |
| 健康评分 | 综合评分（0-100%）+ 状态（Healthy/Caution/Critical） | P0 |
| S.M.A.R.T. 属性 | 温度、通电时间、重分配扇区、读取错误率等 | P0 |
| 告警通知 | 磁盘健康评分低于阈值时弹出警告 | P1 |

**底层模块**: `disk/SmartInfo.h`

### 4.3 SD卡连接引导

**页面**: SDCardConnectionPage（已有）

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 连接引导图 | 图文指引用户通过读卡器连接 SD 卡 | P0 |
| 设备检测 | 检测到 SD 卡后自动跳转到扫描流程 | P1 |

### 4.4 文件修复工具（File Repair）

**详细需求**: 见 [§5 文件修复功能需求](#5-文件修复功能需求)

| 工具 | 支持格式 | 优先级 |
|------|----------|--------|
| 照片修复 | JPEG, PNG, BMP, GIF, TIFF | P2 |
| 视频修复 | MP4, MOV, AVI, MKV | P2 |
| 文档修复 | DOCX, XLSX, PPTX, PDF | P2 |

### 4.5 WinPE 启动盘制作

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| USB 启动盘 | 将 PhoenixRecover PE 版写入 U 盘 | P2 |
| ISO 导出 | 导出可刻录的 ISO 文件 | P2 |

---

## 5. 文件修复功能需求

### 5.1 功能概述

文件修复模块用于修复在恢复过程中发现的损坏文件，也可独立使用。用户可从本地选择已损坏的文件，由修复引擎自动分析损坏类型并尝试修复。

### 5.2 支持的修复格式

| 文件类型 | 格式 | 修复策略 | 优先级 |
|----------|------|----------|--------|
| 压缩文件 | ZIP | 重建中央目录（Central Directory）、修复本地文件头（Local File Header）、跳过损坏条目提取可用内容 | P1 |
| 视频文件 | MP4 | 重建 moov atom、修复 ftyp/mdat 容器结构、音视频流重新封装 | P1 |
| 图片文件 | JPEG | 修复 SOI/EOI 标记、重建 JFIF/EXIF 头、跳过损坏 MCU 块 | P1 |
| 图片文件 | PNG | 修复 IHDR/IEND 块、重算 CRC 校验、跳过损坏 chunk | P2 |

### 5.3 修复流程

```
选择文件 → 文件分析 → 执行修复 → 查看结果
```

#### 5.3.1 步骤 1：选择文件
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 文件选择 | 支持单选/多选待修复文件 | P0 |
| 来源方式 | ① 从本地浏览器选择 ② 从扫描结果中标记为"损坏"的文件直接导入 | P0 |
| 格式校验 | 选择文件后校验格式是否在支持列表中，不支持的格式给出提示 | P0 |

#### 5.3.2 步骤 2：文件分析
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 损坏诊断 | 自动检测文件损坏类型（头部损坏、结构缺失、数据截断等） | P0 |
| 可修复性评估 | 分析完成后给出修复可能性评估（可修复 / 部分修复 / 无法修复） | P0 |
| 分析进度 | 批量文件时显示逐文件分析进度 | P1 |

#### 5.3.3 步骤 3：执行修复
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 修复执行 | 对可修复文件执行修复操作 | P0 |
| 原文件保护 | 修复后文件保存为新副本，不覆盖原始文件 | P0 |
| 输出路径 | 用户可自定义修复文件的保存目录 | P0 |
| 进度显示 | 显示当前修复进度（当前文件 / 总文件数） | P0 |

#### 5.3.4 步骤 4：查看结果
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 结果列表 | 逐文件显示修复状态（成功 / 部分修复 / 失败） | P0 |
| 修复预览 | 修复成功的图片/视频支持预览比对（修复前 vs 修复后） | P1 |
| 打开文件夹 | 一键打开修复文件所在目录 | P0 |

### 5.4 UI 要求

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 独立入口 | 在"更多工具"页面提供独立入口，也可从扫描结果页导入损坏文件 | P0 |
| 向导式界面 | 采用步骤指示器（Step 1-4）引导用户完成修复流程 | P0 |
| 拖拽添加 | 支持拖拽文件到修复区域 | P1 |
| 批量操作 | 支持批量添加和批量修复 | P1 |

### 5.5 底层模块

| 模块 | 路径 | 说明 |
|------|------|------|
| `CarveZip` | `scan/carve/CarveZip.h/.cpp` | ZIP 结构解析与修复（已实现） |
| `CarveMp4` | `scan/carve/CarveMp4.h/.cpp` | MP4 容器解析与修复（已实现） |
| `FileRepairEngine` | `repair/FileRepairEngine.h/.cpp` | 修复引擎统一调度（待实现） |
| `JpegRepairer` | `repair/JpegRepairer.h/.cpp` | JPEG 修复（待实现） |
| `PngRepairer` | `repair/PngRepairer.h/.cpp` | PNG 修复（待实现） |

---

## 6. 磁盘镜像功能需求

### 6.1 功能概述

磁盘镜像功能用于在数据恢复前对源盘进行逐扇区复制，生成精确的磁盘映像文件，确保后续扫描操作不对源盘造成进一步损害。适用于磁盘存在物理故障或坏扇区的场景。

### 6.2 镜像格式

| 格式 | 扩展名 | 说明 | 优先级 |
|------|--------|------|--------|
| Raw (dd) | `.img` / `.dd` / `.raw` | 逐扇区原始复制，1:1 映射源盘数据，兼容性最好 | P0 |

后续版本可考虑支持压缩镜像（如 `.e01` / `.dmg`），当前 MVP 仅支持 Raw 格式。

### 6.3 源/目标选择

#### 6.3.1 源盘选择
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 物理磁盘列表 | 枚举所有物理磁盘（`\\.\PhysicalDriveN`），显示型号、容量、接口类型 | P0 |
| 分区列表 | 支持选择单个分区进行镜像 | P0 |
| 磁盘信息 | 显示磁盘序列号、固件版本、S.M.A.R.T. 健康状态摘要 | P1 |
| 锁定保护 | 镜像期间对源盘设置只读锁定，防止系统写入 | P1 |

#### 6.3.2 目标选择
| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 目标路径 | 选择镜像文件保存路径 | P0 |
| 空间检查 | 目标磁盘剩余空间必须 ≥ 源盘/分区大小，不足时阻止操作 | P0 |
| 同盘检测 | 禁止将镜像保存到源盘所在物理磁盘上 | P0 |
| 文件命名 | 自动生成默认文件名（格式：`PhysicalDrive{N}_{YYYYMMDD_HHmmss}.img`） | P1 |

### 6.4 镜像执行与进度

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 进度条 | 显示整体进度百分比 | P0 |
| 速度显示 | 实时读写速度（MB/s） | P0 |
| 剩余时间 | 基于当前速度估算剩余时间 | P0 |
| 已复制量 | 已复制数据量 / 总数据量 | P0 |
| 暂停/恢复 | 支持暂停镜像操作后继续 | P1 |
| 取消 | 支持取消镜像操作，取消后提示是否删除不完整的镜像文件 | P0 |

### 6.5 错误处理

| 场景 | 处理策略 | 优先级 |
|------|----------|--------|
| 坏扇区 | 默认跳过坏扇区并填充零字节，记录到 `BadSectorMap` 日志 | P0 |
| 读取超时 | 单扇区读取超时（默认 5s）后跳过，记录并继续 | P0 |
| 目标磁盘已满 | 立即暂停镜像，提示用户更换目标路径或释放空间 | P0 |
| 源盘断开 | 立即停止镜像，保留已完成部分，提示错误 | P0 |
| 坏扇区汇总 | 镜像完成后显示坏扇区统计报告（总数、分布位置） | P1 |

### 6.6 镜像校验

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 校验算法 | 完成后可选 MD5 / SHA-256 校验 | P2 |
| 校验结果 | 显示源盘与镜像文件的哈希值比对结果 | P2 |
| 校验报告 | 可导出校验报告为文本文件 | P2 |

### 6.7 底层模块

| 模块 | 路径 | 说明 |
|------|------|------|
| `Imager` | `disk/Imager.h/.cpp` | 磁盘镜像核心引擎（已实现） |
| `SectorReader` | `disk/SectorReader.h/.cpp` | 扇区级磁盘读取（已实现） |
| `BadSectorMap` | `disk/BadSectorMap.h` | 坏扇区记录与映射（已实现） |

---

## 7. S.M.A.R.T. 磁盘健康监测需求

### 7.1 功能概述

S.M.A.R.T.（Self-Monitoring, Analysis and Reporting Technology）监测模块用于读取磁盘自报告的健康状态数据，帮助用户在数据恢复前评估磁盘可靠性，及时发现潜在硬件故障风险。

### 7.2 功能需求

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 磁盘枚举 | 列出所有支持 S.M.A.R.T. 的物理磁盘（HDD / SSD） | P0 |
| 健康评分 | 综合评分 0-100%，映射状态标签：Healthy（≥80%）/ Caution（40-79%）/ Critical（<40%） | P0 |
| 关键属性展示 | 以表格形式展示 S.M.A.R.T. 属性（ID、属性名、当前值、最差值、阈值、原始值） | P0 |
| 温度监测 | 当前磁盘温度独立展示，超过 55°C 显示警告 | P0 |
| 通电时间 | 显示磁盘累计通电小时数 | P1 |
| 重分配扇区 | 高亮显示重分配扇区计数（Reallocated Sector Count），非零时标红 | P0 |
| 读取错误率 | 展示读取错误率趋势 | P1 |
| 告警通知 | 磁盘健康评分 < 40% 或关键属性超阈值时，在应用启动时弹出警告 | P1 |

### 7.3 关键 S.M.A.R.T. 属性

| 属性 ID | 属性名 | 重要程度 |
|---------|--------|----------|
| 0x05 | Reallocated Sectors Count | 关键 |
| 0xC5 | Current Pending Sector Count | 关键 |
| 0xC6 | Uncorrectable Sector Count | 关键 |
| 0xC4 | Reallocation Event Count | 重要 |
| 0x01 | Raw Read Error Rate | 重要 |
| 0xC2 | Temperature | 重要 |
| 0x09 | Power-On Hours | 参考 |
| 0x0C | Power Cycle Count | 参考 |

### 7.4 UI 要求

| 需求项 | 说明 | 优先级 |
|--------|------|--------|
| 磁盘概览卡片 | 每个磁盘用卡片展示：型号、容量、健康评分仪表盘 | P0 |
| 属性详情表 | 点击卡片展开 S.M.A.R.T. 属性完整表格 | P0 |
| 颜色编码 | 正常=绿、注意=橙、危险=红 | P0 |
| 刷新 | 支持手动刷新磁盘健康数据 | P1 |

### 7.5 底层模块

| 模块 | 路径 | 说明 |
|------|------|------|
| `SmartInfo` | `disk/SmartInfo.h` | S.M.A.R.T. 信息读取接口（已实现） |

### 7.6 平台说明

- Windows：通过 `DeviceIoControl` + `SMART_RCV_DRIVE_DATA` IOCTL 读取
- Linux：通过 `/dev/sdX` 的 `SG_IO` ioctl 或调用 `smartctl` 命令行工具
- macOS：通过 `IOKit` 框架读取 S.M.A.R.T. 数据（仅支持内置磁盘）

---

## 8. 扫描引擎架构

### 8.1 引擎层级关系

```
ScanEngine (抽象基类)
├── QuickScan  — 阶段1: 基于文件系统元数据（MFT/FAT/exFAT inode）
├── DeepScan   — 阶段2: 全盘逐扇区深度分析（含已删除区域）
└── RawScan    — 阶段3: 基于 SignatureDB 的文件签名雕刻（Magic Bytes）
```

三个引擎作为扫描流水线的内部阶段，由 `RecoveryScanWorker` 统一调度，依次执行。用户无需感知具体阶段，UI 仅显示当前正在执行的阶段名称以提供进度参考。

### 8.2 文件系统支持

| 文件系统 | 解析器 | 阶段 1（元数据） | 阶段 2（深度） | 阶段 3（签名） | 优先级 |
|----------|--------|:-:|:-:|:-:|--------|
| NTFS | `NtfsParser` | ✓ | ✓ | ✓ | P0 |
| FAT32/FAT16 | `FatParser` | ✓ | ✓ | ✓ | P0 |
| exFAT | `ExfatParser` | ✓ | ✓ | ✓ | P0 |
| ext4 | `Ext4Parser` | ✓ | ✓ | ✓ | P1 |
| HFS+ / APFS | 待实现 | — | — | ✓ | P2 |

### 8.3 文件签名数据库（SignatureDB）

- 已内置 `scan/SignatureDB.cpp`（76KB），包含主流文件类型签名
- 覆盖的文件类型分类：
  - **文档**: DOC/DOCX, XLS/XLSX, PPT/PPTX, PDF, TXT, RTF
  - **图片**: JPG, PNG, BMP, GIF, TIFF, RAW (CR2/NEF/ARW)
  - **视频**: MP4, AVI, MKV, MOV, WMV, FLV
  - **音频**: MP3, WAV, FLAC, AAC, OGG
  - **压缩**: ZIP, RAR, 7Z, TAR, GZ
  - **数据库**: SQLite, MDB
  - **其他**: EXE, DLL, ISO 等

### 8.4 扫描流水线策略

扫描流水线自动依次执行以下三个阶段，每个阶段的结果实时追加到结果列表：

| 阶段 | 速度 | 恢复深度 | 说明 |
|------|------|----------|------|
| 阶段 1：FS 元数据解析 | 快（分钟级） | 浅（仅元数据可达） | 解析文件系统结构，快速出结果，覆盖刚删除的文件、回收站恢复等场景 |
| 阶段 2：深度扫描 | 中（十分钟级） | 深（含覆写分析） | 逐扇区扫描，补充发现格式化后或分区丢失的文件 |
| 阶段 3：签名雕刻 | 慢（小时级） | 最深（签名匹配） | 无文件系统依赖，基于签名匹配发现严重损坏场景下的文件 |

阶段间自动衔接，已发现文件去重（基于扇区偏移 + 文件大小 + 签名），避免重复结果。用户可随时停止扫描并使用已发现的结果进行恢复。

### 8.5 阶段控制

- 扫描启动后三个阶段自动顺序执行，无需用户干预
- 用户可在任意阶段暂停/停止，停止后已发现结果保留
- 阶段切换时通过 `setScanStageChanged` 信号通知 UI 更新当前阶段显示
- 各阶段发现的文件通过 `ScanFileDto.scanSource` 字段标记来源（0=元数据, 1=深度, 2=签名）

---

## 9. 控制层架构

### 9.1 控制器职责划分

| 控制器 | 职责 | 关联页面 |
|--------|------|----------|
| `RecoveryFlowController` | 管理整体流程路由、页面切换 | 所有页面 |
| `RecoveryScanController` | 管理扫描任务生命周期（启动/暂停/停止），调度三阶段流水线 | P2, P3 |
| `RecoveryController` (新增) | 管理恢复任务（启动/进度/完成/重试） | P4, P5 |
| `DiskImageController` (新增) | 管理磁盘镜像任务 | DiskImagingPage |
| `FileRepairController` (新增) | 管理文件修复任务 | FileRepairPage |

### 9.2 Worker 线程模型

```
UI Thread
  ├── RecoveryScanController
  │     └── QThread → RecoveryScanWorker
  │                     ├── 阶段1: QuickScan
  │                     ├── 阶段2: DeepScan
  │                     └── 阶段3: RawScan
  ├── RecoveryController (新增)
  │     └── QThread → RecoveryWorker
  │                     └── RecoverEngine
  ├── DiskImageController (新增)
  │     └── QThread → ImageWorker
  │                     └── Imager
  └── FileRepairController (新增)
        └── QThread → RepairWorker
                        └── FileRepairEngine
```

### 9.3 信号流转
```
[RecoveryLocationPage] --requestStartScan--> [RecoveryFlowController]
    → 切换到扫描页
    → [RecoveryScanController].startScan(...)
        → Worker线程启动，自动执行三阶段流水线
        → Worker --stageChanged--> [RecoveryScanPage].setScanStageChanged()
        → Worker --batchReady--> [RecoveryScanPage].appendScanFileBatch()
        → Worker --progress--> [RecoveryScanPage].setScanProgress()
        → Worker --finished--> [RecoveryScanPage].setScanFinished()

[RecoveryScanPage] --requestRecover--> [RecoveryFlowController]
    → 切换到恢复目标页

[RecoveryTargetPage] --requestStartRecover--> [RecoveryFlowController]
    → [RecoveryController].startRecover(...)
        → Worker线程启动
        → Worker --progress--> [RecoveryProgressPage].setRecoverProgress()
        → Worker --finished--> [RecoveryCompletePage].setResult()
```

---

## 10. 数据模型

### 10.1 ScanFileDto（已有，需扩展）

```cpp
struct ScanFileDto {
    quint64 id = 0;
    QString name;
    QString fullPath;
    QString extension;
    qint64 sizeBytes = 0;
    quint64 parentId = 0;
    bool isDirectory = false;
    quint64 modifyTimeFt = 0;   // Windows FILETIME
    int recoverability = 0;     // 0=Unknown, 1=High, 2=Medium, 3=Low
    QString category;           // "Document" / "Image" / "Video" ...
    int scanSource = 0;         // 0=元数据阶段, 1=深度扫描阶段, 2=签名雕刻阶段
};
```

### 10.2 RecoverResultDto（新增）

```cpp
struct RecoverResultDto {
    quint64 fileId = 0;
    QString fileName;
    QString targetPath;
    bool success = false;
    QString errorMessage;      // 失败时的错误描述
    qint64 recoveredBytes = 0;
};
```

### 10.3 DiskInfoDto（新增）

```cpp
struct DiskInfoDto {
    QString driveLetter;       // "C:", "D:" ...
    QString volumeLabel;       // "Local Disk", "Data" ...
    QString fileSystem;        // "NTFS", "FAT32", "exFAT" ...
    quint64 totalBytes = 0;
    quint64 usedBytes = 0;
    bool isRemovable = false;  // USB/SD 等外接设备
    int diskType = 0;          // 0=HDD, 1=SSD, 2=USB, 3=SDCard
};
```

### 10.4 RepairResultDto（新增）

```cpp
struct RepairResultDto {
    QString sourceFilePath;    // 源文件路径
    QString repairedFilePath;  // 修复后文件路径
    QString fileType;          // "JPEG" / "PNG" / "MP4" / "ZIP"
    int status = 0;            // 0=待修复, 1=修复成功, 2=部分修复, 3=修复失败
    QString damageDescription; // 损坏类型描述
    QString errorMessage;      // 失败时的错误描述
};
```

---

## 11. 页面清单与路由

### 11.1 完整页面清单

| 页面类名 | 文件路径 | 状态 | 用途 |
|----------|----------|------|------|
| `RecoveryLocationPage` | `pages/RecoveryLocationPage.h/.cpp` | 已有（需增强） | 恢复位置选择 |
| `RecoveryScanPage` | `pages/RecoveryScanPage.h/.cpp` | 已有（需增强） | 扫描进度 & 结果展示 |
| `RecoveryTargetPage` | `pages/RecoveryTargetPage.h/.cpp` | **新增** | 恢复目标选择 |
| `RecoveryCompletePage` | `pages/RecoveryCompletePage.h/.cpp` | **新增** | 恢复完成展示 |
| `DiskImagingPage` | `pages/DiskImagingPage.h/.cpp` | **新增** | 磁盘镜像 |
| `DiskHealthPage` | `pages/DiskHealthPage.h/.cpp` | **新增** | S.M.A.R.T. 监测 |
| `FileRepairPage` | `pages/FileRepairPage.h/.cpp` | **新增** | 文件修复 |
| `MoreRecoveryToolsPage` | `pages/MoreRecoveryToolsPage.h/.cpp` | 已有 | 更多工具入口 |
| `SDCardConnectionPage` | `pages/SDCardConnectionPage.h/.cpp` | 已有 | SD卡连接引导 |
| `SettingsPage` | `pages/SettingsPage.h/.cpp` | **新增** | 应用设置 |

### 11.2 路由（QStackedWidget Index）

```
Index 0: RecoveryLocationPage     (首页)
Index 1: RecoveryScanPage         (扫描 & 结果)
Index 2: RecoveryTargetPage       (恢复目标)
Index 3: RecoveryCompletePage     (恢复完成)
Index 4: DiskImagingPage          (磁盘镜像)
Index 5: DiskHealthPage           (磁盘健康)
Index 6: FileRepairPage           (文件修复)
Index 7: MoreRecoveryToolsPage    (更多工具)
Index 8: SDCardConnectionPage     (SD卡引导)
Index 9: SettingsPage             (设置)
```

---

## 12. 对标 EaseUS 差距与计划

基于 `EaseUS_Difference_Analysis.md` 的分析，以下是关键差距及计划：

| 差距项 | 当前状态 | 目标状态 | 优先级 |
|--------|----------|----------|--------|
| 智能多阶段扫描流水线 | 三引擎独立存在但无统一调度 | 单一流水线自动分阶段执行，UI 显示当前阶段 | **P0** |
| 扫描暂停/取消 | 无暂停取消 | 支持暂停、停止，保留已扫描结果 | **P0** |
| 文件多选（勾选模型） | 单选 | 复选框多选 + 全选/反选 | **P0** |
| 恢复目标路径选择 | 无 | 完整目标选择页 + 安全校验 | **P0** |
| 恢复引擎调用闭环 | RecoverEngine未被调用 | 完整恢复流程 + 进度 + 结果 | **P0** |
| 恢复概率可视化 | 底层有计算，UI未展示 | 表格列显示 High/Medium/Low 标签 | **P0** |
| 文件预览 | 按钮存在无逻辑 | 图片缩略图 + 文本预览 | **P1** |
| 搜索与筛选 | UI存在无逻辑 | 文件名搜索 + 类型/大小/日期筛选 | **P1** |
| 大结果集性能 | 直接插入QTreeWidget | 虚拟化/分页/lazy加载 | **P1** |
| 扫描会话保存/恢复 | ScanSerializer已有 | 接入UI保存/加载流程 | **P1** |
| 路径/类型视图同步 | 基础同步 | 搜索筛选后的联动选择 | **P2** |
| 文件修复 | 底层 Carve 模块已有 | 完整修复 UI + 修复引擎 | **P2** |
| 跨平台支持 | 仅 Windows | Windows + Linux + macOS | **P1-P2** |

---

## 13. 非功能需求

### 13.1 性能要求
| 指标 | 目标值 |
|------|--------|
| 应用启动时间 | < 2 秒 |
| 阶段 1（元数据解析）速度 | > 100 MB/s |
| UI 文件列表渲染 | 10万文件不卡顿（< 100ms 响应） |
| 内存占用 | 扫描时 < 500 MB |

### 13.2 兼容性
| 项目 | 要求 | 优先级 |
|------|------|--------|
| Windows | Windows 10 (1809+) / Windows 11, x64 | P0 |
| Linux | Ubuntu 20.04+, Fedora 36+, Debian 11+, x64 | P1 |
| macOS | macOS 12 (Monterey)+, x64 / Apple Silicon (arm64) | P2 |
| 文件系统 | NTFS, FAT32, FAT16, exFAT, ext4; macOS 增加 HFS+/APFS（仅签名雕刻阶段） | — |
| 设备 | 内置硬盘、SSD、USB、SD卡、移动硬盘 | — |

### 13.3 安全与数据保护
- 扫描过程为只读操作，不向源盘写入任何数据
- 恢复操作前校验目标路径不与源盘重叠
- 管理员权限请求（部分磁盘扫描需要 UAC 提升 / Linux root 权限 / macOS Full Disk Access 授权）

### 13.4 日志
- 扫描/恢复操作全程记录到 `Logger`
- 日志文件位于应用目录下 `logs/` 目录
- 支持日志级别：DEBUG / INFO / WARN / ERROR

### 13.5 跨平台策略

| 层级 | 策略 | 说明 |
|------|------|------|
| UI 层 | Qt Widgets 跨平台 | Qt 原生支持 Windows / Linux / macOS，UI 代码共用 |
| 磁盘访问层 | 平台抽象接口 | `SectorReader` 通过条件编译隔离平台差异：Windows 使用 `CreateFile` + `DeviceIoControl`，Linux 使用 `/dev/sdX` + `ioctl`，macOS 使用 `IOKit` + `diskutil` |
| 文件系统解析层 | 跨平台共用 | NTFS / FAT / exFAT / ext4 解析器为纯算法实现，无平台依赖 |
| 权限管理 | 平台适配 | Windows: UAC 提升；Linux: `pkexec` / `sudo` 提权；macOS: 请求 Full Disk Access 权限 |
| 构建系统 | CMake 统一 | 使用 CMake 管理跨平台构建，CI 覆盖三平台编译与测试 |
| 发布优先级 | Windows P0 → Linux P1 → macOS P2 | Windows 首发，Linux 紧随，macOS 最后支持 |

---

## 14. UI 设计规范

遵循项目 `STYLE_GUIDE.md` 编码规范：
- 页面类只负责 UI 展示和信号发射
- 业务逻辑在 Controller 层处理
- 页面通过信号与 Controller 通信
- Qt 新式信号槽连接

### 14.1 整体布局
- **顶部栏**: 应用Logo + 版本 + 全局操作（设置/帮助/升级）
- **左侧导航**: 主功能区导航（Data Recovery / Disk Image / More Tools / Disk Health / File Repair / Settings）
- **主内容区**: 右侧大面积内容区，由 QStackedWidget 管理页面切换

### 14.2 配色方案
- 主色调：专业深色 / 浅色可切换（暗色主题为主）
- 强调色：用于操作按钮、进度条、选中状态
- 恢复概率颜色：High=绿, Medium=橙, Low=红

### 14.3 交互原则
- 所有扫描/恢复操作显示进度反馈
- 破坏性操作（停止扫描、删除会话）需二次确认
- 长时间操作支持取消
- 步骤指示器显示当前所处流程位置

---

## 15. 版本规划

### v1.0 — MVP（最小可行产品）
**P0 功能全部完成：**
- 恢复位置选择（本地磁盘）
- 智能多阶段扫描流水线（FS 元数据解析 → 深度扫描 → 签名雕刻，自动执行）
- 扫描进度展示 + 暂停/停止
- 扫描结果多选 + 恢复概率展示
- 恢复目标选择 + 安全校验
- 恢复执行 + 完成页

### v1.1 — 增强版
**P1 功能：**
- 外接设备检测与恢复
- 文件预览（图片/文本）
- 搜索与筛选功能
- 扫描会话保存/恢复
- 大结果集性能优化
- 磁盘镜像功能
- Linux 平台支持

### v1.2 — 完整版
**P2 功能：**
- 文件修复工具（ZIP/MP4/JPEG/PNG）
- WinPE 启动盘制作
- 网络位置恢复
- 多语言支持
- S.M.A.R.T. 深度分析
- macOS 平台支持

---

## 附录 A: 已有代码模块映射

| 模块 | 路径 | 说明 | 状态 |
|------|------|------|------|
| QuickScan | `scan/QuickScan.h/.cpp` | 扫描阶段1：FS 元数据解析引擎 | 已实现 |
| DeepScan | `scan/DeepScan.h/.cpp` | 扫描阶段2：深度扫描引擎 | 已实现 |
| RawScan | `scan/RawScan.h/.cpp` | 扫描阶段3：签名雕刻引擎 | 已实现 |
| SignatureDB | `scan/SignatureDB.h/.cpp` | 文件签名数据库 | 已实现 |
| ScanSerializer | `scan/ScanSerializer.h/.cpp` | 扫描结果序列化 | 已实现 |
| ScanEngine | `scan/ScanEngine.h` | 扫描引擎抽象基类 | 已实现 |
| RecoverEngine | `recover/RecoverEngine.h/.cpp` | 恢复引擎 | 已实现（未接入UI） |
| FileFilter | `recover/FileFilter.h` | 文件过滤器 | 已实现 |
| FileNode | `recover/FileNode.h` | 文件节点数据结构 | 已实现 |
| NtfsParser | `fs/ntfs/NtfsParser.h/.cpp` | NTFS解析器 | 已实现 |
| FatParser | `fs/fat/FatParser.h/.cpp` | FAT32解析器 | 已实现 |
| ExfatParser | `fs/exfat/ExfatParser.h/.cpp` | exFAT解析器 | 已实现 |
| Ext4Parser | `fs/ext4/Ext4Parser.h/.cpp` | ext4解析器 | 已实现 |
| SectorReader | `disk/SectorReader.h/.cpp` | 磁盘扇区读取 | 已实现 |
| PartitionProbe | `disk/PartitionProbe.h/.cpp` | 分区探测 | 已实现 |
| Imager | `disk/Imager.h/.cpp` | 磁盘镜像 | 已实现 |
| SmartInfo | `disk/SmartInfo.h` | SMART信息读取 | 已实现 |
| BadSectorMap | `disk/BadSectorMap.h` | 坏扇区映射 | 已实现 |
| Logger | `log/Logger.h/.cpp` | 日志系统 | 已实现 |
| ThreadPool | `thread/ThreadPool.h` | 线程池 | 已实现 |
| CarveZip | `scan/carve/CarveZip.h/.cpp` | ZIP文件雕刻 | 已实现 |
| CarveMp4 | `scan/carve/CarveMp4.h/.cpp` | MP4文件雕刻 | 已实现 |

## 附录 B: 原型页面对应关系

| 原型页面 | 对应类 | 原型入口 |
|----------|--------|----------|
| 1. Location Selection | `RecoveryLocationPage` | 导航 "Data Recovery" |
| 2. Scanning | `RecoveryScanPage` (扫描中) | 点击 "Start Scan" |
| 3. Results & Preview | `RecoveryScanPage` (扫描完成) | 扫描结束或点击 "View Results" |
| 4. Recovery Target | `RecoveryTargetPage` | 点击 "Recover Selected" |
| 5. Recovery Complete | `RecoveryCompletePage` | 恢复完成 |
| Disk Imaging | `DiskImagingPage` | 导航 "Disk Image" |
| Disk Health | `DiskHealthPage` | 导航 "Disk Health" |
| File Repair | `FileRepairPage` | 导航 "File Repair" |
| More Tools | `MoreRecoveryToolsPage` | 导航 "More Tools" |
| Settings | `SettingsPage` | 导航 "Settings" |
