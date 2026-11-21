# PhoenixRecover

**PhoenixRecover** 是一款开源的 Windows 桌面数据恢复工具，采用智能三阶段扫描引擎（文件系统元数据解析 → 深度扫描 → 签名雕刻），可从硬盘、U 盘、SD 卡、SSD 等存储设备中恢复误删除、格式化或丢失的文件。

基于 C++17 和 Qt 5 构建，PhoenixRecover 支持 NTFS、FAT12/16/32、exFAT、ext4 等主流文件系统，内建 300+ 文件签名识别，并提供文件修复、磁盘镜像、S.M.A.R.T. 健康监测等实用工具。

---

## 功能特性

- **智能多阶段扫描** — 三阶段流水线自动选择最优策略，无需手动切换模式
- **多文件系统支持** — NTFS / FAT12/16/32 / exFAT / ext4
- **300+ 文件签名** — 覆盖文档、图片、音视频、压缩包等，支持自定义签名扩展
- **批量恢复** — 进度显示、实时取消、同卷写保护
- **恢复报告** — 统计摘要 + 文件明细 + 导出
- **文件修复** — ZIP 重建 + MP4 remux
- **磁盘镜像** — 物理磁盘 / 分区导出为 raw image
- **S.M.A.R.T. 监测** — 磁盘健康状态一目了然
- **会话管理** — 扫描结果自动保存，下次启动可继续
- **高性能 UI** — QTableView + Model/View 架构，万级文件流畅浏览

## 截图

<!-- TODO: 添加应用截图 -->

---

## 快速开始

### 使用预编译版本

1. 从 [Releases](../../releases) 页面下载最新版本。
2. 解压后运行 `PhoenixRecover.exe`。
3. 选择磁盘 → 扫描 → 勾选文件 → 恢复到其他磁盘。

### 从源码构建

**环境要求**：MSVC 2019+、Qt 5.14+、CMake 3.16+

```powershell
git clone https://github.com/<your-org>/PhoenixRecover.git
cd PhoenixRecover
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="C:\Qt\5.14.2\msvc2017_64"
cmake --build . --config Release
```

详细构建说明请参阅 [docs/BUILD_GUIDE.md](docs/BUILD_GUIDE.md)。

### 一键发布构建

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Build-Release.ps1" -QtPrefix "C:\Qt\5.14.2\msvc2017_64"
```

---

## 文档

| 文档 | 说明 |
|------|------|
| [用户指南](docs/USER_GUIDE.md) | 安装、使用、功能说明 |
| [常见问题](docs/FAQ.md) | 12 个常见问题解答 |
| [构建指南](docs/BUILD_GUIDE.md) | 环境配置与编译方法 |
| [技术架构](docs/ARCHITECTURE.md) | 系统架构与模块设计 |
| [变更日志](docs/CHANGELOG.md) | 版本更新记录 |
| [签名格式](docs/SIGNATURE_FORMAT.md) | 自定义文件签名 JSON 格式 |
| [测试计划](docs/TEST_PLAN.md) | 测试策略与用例 |

---

## 技术架构

PhoenixRecover 采用分层架构设计：

```
┌──────────────────────────────────────┐
│           UI 层 (Qt Widgets)         │
│  Pages / Widgets / Models            │
├──────────────────────────────────────┤
│         控制器层 (Controllers)        │
│  FlowController / ScanController     │
├──────────────────────────────────────┤
│           核心引擎层                  │
│  Scan / Recover / Repair / Carve     │
├──────────────────────────────────────┤
│        文件系统解析层 (fs/)           │
│  NTFS / FAT / exFAT / ext4          │
├──────────────────────────────────────┤
│         磁盘访问层 (disk/)           │
│  SectorReader / DiskManager / SMART  │
└──────────────────────────────────────┘
```

详细架构说明请参阅 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

## 贡献指南

欢迎提交 Issue 和 Pull Request！

1. Fork 本仓库。
2. 创建功能分支：`git checkout -b feature/my-feature`。
3. 提交变更并推送。
4. 创建 Pull Request。

**代码规范**：

- C++17 标准，使用 MSVC `/utf-8` 编译选项。
- 源文件统一 UTF-8 编码。
- 遵循项目现有的命名约定和目录结构。

**运行测试**：

```powershell
# 仅单元测试（无需 Qt）
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -TestsOnly

# 完整回归
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -QtPrefix "C:\Qt\5.14.2\msvc2017_64"
```

---

## 测试数据

仓库**不包含**测试语料与磁盘镜像（`testdata/` 约 163 GiB：CFReDS DFR、DFTT、
Digital Corpora、Sleuth Kit、NapierOne 等）。请按
[docs/TESTDATA.md](docs/TESTDATA.md) 自行下载到 `testdata/` 目录后运行集成测试；
缺少镜像时相关 CTest 记为 `NEEDS_EWF` 跳过而非失败。

---

## 许可证

本项目采用 [MIT 许可证](LICENSE) 发布。

第三方组件许可证信息详见 [LICENSE](LICENSE) 文件末尾的第三方声明段。
