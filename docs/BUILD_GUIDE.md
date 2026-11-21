# PhoenixRecover 构建指南

本文档详细说明如何从源码构建 PhoenixRecover 项目，涵盖环境准备、配置选项、编译方式及测试运行。

---

## 环境要求

### Windows（主要开发平台）

| 工具 | 最低版本 | 说明 |
|------|----------|------|
| **MSVC** | 2019 (v16.x) 或更高 | 需要 C++17 支持；推荐使用 Visual Studio 2022 |
| **Qt** | 5.14.0+ | 需要 Core、Gui、Widgets 模块；推荐 5.14.2 msvc2017_64 预编译包 |
| **CMake** | 3.16+ | 项目 `cmake_minimum_required(VERSION 3.16)` |
| **Git** | 2.x | 用于获取源码 |

> **注意**：仅构建单元测试（不含 GUI）时 **无需安装 Qt**，详见下方"仅测试构建"章节。

### Linux（可选 / 实验性）

| 工具 | 最低版本 | 说明 |
|------|----------|------|
| **GCC** | 9+ | 需要 C++17 filesystem 支持 |
| **Qt** | 5.14.0+ | 同上；通常通过发行版包管理器安装 |
| **CMake** | 3.16+ | — |

> ⚠️ 当前卷/扇区访问模块（`disk/`、部分 `recover/`）大量依赖 Windows API，Linux 下完整链接 GUI 目标会在首处 `Windows.h` 处失败，属预期行为。仅测试目标可正常编译。

### macOS（可选 / 实验性）

| 工具 | 最低版本 | 说明 |
|------|----------|------|
| **Clang** | 12+ | Apple Clang 或 LLVM Clang 均可 |
| **Qt** | 5.14.0+ | 可通过 Homebrew 安装：`brew install qt@5` |
| **CMake** | 3.16+ | — |

> 跨平台编译约定定义在 `common/Platform.h`，使用宏 `PHOENIX_OS_WINDOWS`、`PHOENIX_OS_LINUX_DESKTOP`、`PHOENIX_OS_MACOS` 区分平台行为。

---

## 获取源码

```bash
git clone https://github.com/<your-org>/PhoenixRecover.git
cd PhoenixRecover
```

如果使用 SSH：

```bash
git clone git@github.com:<your-org>/PhoenixRecover.git
cd PhoenixRecover
```

克隆完成后确认目录结构正确：

```
PhoenixRecover/
├── CMakeLists.txt          # 顶层 CMake 配置
├── PhoenixRecover.vcxproj  # Visual Studio 项目文件
├── common/                 # 公共头文件与工具
├── controllers/            # 控制器层
├── disk/                   # 磁盘/卷访问
├── fs/                     # 文件系统解析器 (NTFS, FAT, exFAT, ext4)
├── media/                  # 媒体缩略图
├── pages/                  # UI 页面
├── recover/                # 恢复引擎
├── scan/                   # 扫描引擎（快速/深度/原始/雕刻）
├── scripts/                # 自动化脚本
├── tests/                  # 单元测试
├── testdata/               # 测试数据与 DFTT 镜像
└── docs/                   # 文档
```

---

## 配置与构建

### 方式一：完整构建（含 GUI）

适用于开发和发布，需要已安装 Qt 5.14+。

```powershell
# 创建构建目录
mkdir build
cd build

# 配置（指定 Qt 安装路径）
cmake .. -DCMAKE_PREFIX_PATH="C:\Qt\5.14.2\msvc2017_64"

# 编译
cmake --build . --config Release
```

**Linux/macOS 示例：**

```bash
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH=/opt/Qt/5.14.2/gcc_64
cmake --build . --config Release
```

**关键 CMake 变量说明：**

| 变量 | 默认值 | 说明 |
|------|--------|------|
| `PHOENIX_BUILD_GUI` | `ON` | 是否构建 Qt GUI 主程序 |
| `CMAKE_PREFIX_PATH` | — | Qt 安装路径（如 `C:\Qt\5.14.2\msvc2017_64`） |
| `CMAKE_BUILD_TYPE` | — | 构建类型：Debug / Release / RelWithDebInfo / MinSizeRel |

### 方式二：仅测试构建（无需 Qt）

当只需运行单元测试、不涉及 GUI 时，可关闭 `PHOENIX_BUILD_GUI`，**无需安装 Qt**：

```powershell
mkdir build-tests
cd build-tests

# 配置（关闭 GUI）
cmake .. -DPHOENIX_BUILD_GUI=OFF

# 编译
cmake --build . --config Release
```

此模式下 CMake 不会调用 `find_package(Qt5)`，仅编译 `tests/` 子目录中的测试目标 `phoenix_unit_tests`。

### 方式三：Visual Studio IDE

1. 使用 Visual Studio 2019/2022 打开 `PhoenixRecover.vcxproj`（或通过"打开文件夹"功能让 VS 自动识别 CMakeLists.txt）。
2. 在项目属性 → VC++ Directories 中确认 Qt 头文件和库路径已正确设置。
3. 选择 Release/Debug 配置后按 `Ctrl+B` 构建。

> 提示：也可使用 Visual Studio 的"CMake 项目"模式（文件 → 打开 → CMake），这样配置会统一由 CMakeLists.txt 管理。

### 方式四：命令行构建（MSBuild）

在 **Developer Command Prompt for VS** 或 **Developer PowerShell** 中：

```powershell
msbuild PhoenixRecover.vcxproj /p:Configuration=Release /p:Platform=x64
```

或使用 CMake 驱动构建（推荐）：

```powershell
cmake --build build --config Release --parallel
```

`--parallel` 参数启用多核并行编译，显著加速构建过程。

---

## 运行测试

### 使用 CTest

构建完成后，在构建目录中运行：

```powershell
cd build
ctest -C Release --output-on-failure
```

参数说明：
- `-C Release`：指定测试的构建配置
- `--output-on-failure`：仅在测试失败时显示详细输出
- `--verbose` 或 `-V`：显示所有测试的详细输出

当前注册的测试用例：
- **UtilsNumeric**：验证 `common/Utils.h` 中的数值工具函数

### 使用 Regress.ps1 回归脚本

`scripts/Regress.ps1` 是自动化回归入口，一键完成 **配置 → 编译 → CTest → （可选）恢复结果对比**。

**基本用法：**

```powershell
# 在仓库根目录执行
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -QtPrefix "C:\Qt\5.14.2\msvc2017_64"
```

**仅运行单元测试（无需 Qt）：**

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -TestsOnly
```

**全量回归 + 恢复结果金标对比：**

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" `
    -QtPrefix "C:\Qt\5.14.2\msvc2017_64" `
    -RecoveryOutput "D:\gold\recover"
```

**完整参数列表：**

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `-RepoRoot` | string | 脚本上级目录 | 仓库根路径 |
| `-BuildDir` | string | `build\regress` | 构建输出目录 |
| `-Config` | string | `Release` | 构建配置（Debug/Release/RelWithDebInfo/MinSizeRel） |
| `-QtPrefix` | string | — | Qt 安装路径，可用 `CMAKE_PREFIX_PATH` 环境变量替代 |
| `-TestsOnly` | switch | — | 仅构建测试，不含 GUI（等同 `-DPHOENIX_BUILD_GUI=OFF`） |
| `-RecoveryOutput` | string | — | 恢复结果目录；设置后会调用 `Compare-RecoveryResults.ps1 -Strict` |
| `-ExpectedCsv` | string | `testdata\manifests\expected_results.csv` | 预期结果 CSV |
| `-CompareReport` | string | `testdata\manifests\compare_report.csv` | 对比报告输出路径 |
| `-SkipConfigure` | switch | — | 跳过 cmake 配置步骤（用于增量构建） |

**环境变量支持：**
- `CMAKE_PREFIX_PATH`：可替代 `-QtPrefix` 参数指定 Qt 路径

**退出码说明：**
- `0`：全部通过
- 非零：配置/编译/测试/对比中任一步骤失败

---

## 目录结构说明

### 构建产物位置

| 构建方式 | 默认输出目录 | 说明 |
|----------|--------------|------|
| 手动 CMake 构建 | `build/` | 用户自行创建 |
| Regress.ps1 | `build/regress/` | 回归脚本默认路径 |
| Visual Studio | `x64/Release/` 或 `x64/Debug/` | VS 默认输出 |

构建目录内部结构：

```
build/
├── PhoenixRecover.exe       # GUI 主程序（仅 PHOENIX_BUILD_GUI=ON 时）
├── tests/
│   └── phoenix_unit_tests.exe  # 单元测试可执行文件
├── CMakeCache.txt           # CMake 缓存
└── ...
```

### 测试数据位置

```
testdata/
├── manifests/
│   ├── datasets.csv              # 测试数据集清单
│   ├── expected_results.csv      # 恢复结果金标
│   └── compare_report.csv        # 对比报告（运行时生成）
├── tools/
│   └── Compare-RecoveryResults.ps1  # 恢复结果对比脚本
├── images/                       # DFTT 磁盘镜像
└── README.md                     # 测试数据获取说明
```

---

## 常见问题

### Q1: `find_package(Qt5)` 失败 / 找不到 Qt

**错误信息示例：**
```
CMake Error at CMakeLists.txt:29 (find_package):
  Could not find a package configuration file provided by "Qt5"
```

**解决方案：**

1. 确认已安装 Qt 5.14+，且安装了 MSVC 对应版本的预编译包（如 `msvc2017_64`）。
2. 在 cmake 命令中显式指定 Qt 路径：
   ```powershell
   cmake .. -DCMAKE_PREFIX_PATH="C:\Qt\5.14.2\msvc2017_64"
   ```
3. 或者设置环境变量：
   ```powershell
   $env:CMAKE_PREFIX_PATH = "C:\Qt\5.14.2\msvc2017_64"
   cmake ..
   ```
4. 如果无需 GUI，直接跳过 Qt：
   ```powershell
   cmake .. -DPHOENIX_BUILD_GUI=OFF
   ```

### Q2: `msbuild` 不在 PATH 中

**错误信息示例：**
```
msbuild : 无法将"msbuild"项识别为 cmdlet、函数、脚本文件或可运行程序的名称。
```

**解决方案：**

使用 **Developer Command Prompt for VS** 或 **Developer PowerShell for VS** 打开终端，它们会自动配置 MSVC 工具链路径。

或者手动导入环境：
```powershell
# VS 2022 示例
& "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
```

推荐在 VS 开始菜单中搜索 "Developer PowerShell" 来打开已配置好的终端。

### Q3: DFTT 镜像下载

测试数据集中使用的 DFTT（Digital Forensics Test Tool）磁盘镜像需要单独下载。

**解决方案：**

参见 `testdata/README.md` 获取下载链接和放置说明。镜像文件体积较大，不包含在 Git 仓库中。

### Q4: Linux/macOS 下 GUI 构建失败

**原因：** `disk/`、部分 `recover/` 模块大量使用 Windows API（如 `CreateFile`、`DeviceIoControl`），在非 Windows 平台无法编译。

**解决方案：**

当前阶段在 Linux/macOS 上仅支持测试构建：
```bash
cmake .. -DPHOENIX_BUILD_GUI=OFF
cmake --build .
```

完整跨平台支持需要对 Windows API 调用进行抽象封装，参见 `common/Platform.h` 中的平台宏定义。

### Q5: 编译时中文乱码

**原因：** 源文件使用 UTF-8 编码，MSVC 默认使用系统代码页。

**解决方案：** 项目已在 CMakeLists.txt 中配置 `/utf-8` 编译选项。如果使用 `.vcxproj` 构建，请确认项目属性中 C/C++ → 命令行 中包含 `/utf-8`。

### Q6: 增量构建跳过配置步骤

如果仅修改了源码而未修改 CMakeLists.txt，可使用 `-SkipConfigure` 加速回归：

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Regress.ps1" -TestsOnly -SkipConfigure
```
## 发布与安装包（Windows）

### Build-Release.ps1（含安装包）

`Build-Release.ps1` 现支持：构建、Qt 依赖打包、NSIS/WiX 安装包生成、安装/卸载冒烟测试、可选签名命令。

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Build-Release.ps1" `
  -QtPrefix "C:\Qt\5.14.2\msvc2017_64" `
  -Config Release `
  -OutputDir "release" `
  -Installer NSIS `
  -Version "1.0.0.20260527" `
  -RunSmokeTest
```

### 新增参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `-Installer` | `NSIS` | `None` / `NSIS` / `WiX`（当前推荐 NSIS） |
| `-Version` | （默认读 `VERSION`） | 安装包版本号（用于文件名） |
| `-RunSmokeTest` | false | 生成安装包后执行安装/卸载冒烟测试 |
| `-InstallDir` | `%ProgramFiles%\\PhoenixRecover` | 冒烟测试安装目录 |
| `-SignCommand` | 空 | 可选签名命令模板（支持 `{output}` 占位） |

### 单独脚本

- `scripts/Create-Installer.ps1`：调用 NSIS 生成安装包
- `scripts/Smoke-Installer.ps1`：静默安装 -> 启动 -> 静默卸载冒烟测试

> 依赖：需要 `makensis.exe` 在 PATH（安装 NSIS 后可用）。

---
