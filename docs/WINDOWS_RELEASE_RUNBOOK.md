# Windows 正式发布运行手册（NSIS + 代码签名）

> 本机 Qt：**5.14.2 / MSVC 2017 64-bit**  
> 脚本参数 `-QtPrefix` 填 **Kit 根目录**（含 `lib\cmake\Qt5Core`），**不要**填 `bin` 目录。

| 你提供的路径 | 脚本应使用 |
|-------------|-----------|
| `C:\Qt\Qt5.14.2\5.14.2\msvc2017_64\bin` | `C:\Qt\Qt5.14.2\5.14.2\msvc2017_64` |

也可设置环境变量（任选其一）：

```powershell
$env:CMAKE_PREFIX_PATH = "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64"
```

---

## 前置条件

- Visual Studio 2019（或带 MSVC v142 工具链）
- CMake 3.16+
- NSIS（`makensis.exe` 在 PATH）
- 仓库根目录执行下列命令

---

## 阶段 1：发布前门禁（必做）

构建 GUI + 全量 CTest + 修复样本完整性；可选修复功能测试。

```powershell
cd C:\Users\TH\Desktop\dev\test\PhoenixRecover

powershell -ExecutionPolicy Bypass -File "scripts\Regress.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release `
  -RepairFunctionalTest
```

仅快速单测（不编 GUI）：

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Regress.ps1" -TestsOnly -Config Release
```

或一键 PreRelease（测试 + 可选再打 NSIS 包，见脚本内 `-SkipInstaller`）：

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\PreRelease.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release
```

**通过标准**：CTest 全部 Passed；`RepairSamplesIntegrity` 通过；若启用 `-RepairFunctionalTest` 则 ZIP/MP4 修复用例通过。

---

## 阶段 2：生成发布目录 + NSIS 安装包（必做）

版本号由仓库根目录 **`VERSION`** 文件维护（当前 `1.0.0.20260527`）；脚本未传 `-Version` 时自动读取。

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Build-Release.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release `
  -Installer NSIS `
  -Version "1.0.0.20260527" `
  -RunSmokeTest
```

**产物位置**（默认）：

| 产物 | 路径 |
|------|------|
| 绿色版目录 | `release\`（含 `PhoenixRecover.exe` + Qt 依赖） |
| NSIS 安装包 | `release\PhoenixRecover-Setup-1.0.0.20260527.exe` |

单独重打安装包（已存在 `release\` 时）：

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Create-Installer.ps1" `
  -SourceDir "release" -Version "1.0.0.20260527"
```

---

## 阶段 3：代码签名（大众发布，证书就绪后）

将 `-SignCommand` 换成你的 signtool 命令（示例，勿直接用于生产）：

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Build-Release.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release `
  -Installer NSIS `
  -Version "1.0.0.20260527" `
  -SignCommand "signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /a"
```

建议至少签名：**安装包 exe**、**`release\PhoenixRecover.exe`**。

---

## 阶段 4：干净机验收（强烈建议）

在未安装 VS/Qt 的 Windows 10/11 上：

1. 安装 `PhoenixRecover-Setup-x.y.z.exe`
2. 启动 → 选盘扫描 → 恢复一次（注意同卷保护提示）
3. 侧栏「文件修复」→ 选样本或损坏文件 → 导出报告
4. 卸载，检查无异常残留

崩溃诊断目录（默认）：安装目录下 `crash_reports\`（含 `.dmp` / `.txt` / `.log`）。

---

## 合规归档（发版同日）

- 复制 `docs\THIRD_PARTY_NOTICE.md` 到发布说明或安装包 `docs\`
- 由 `docs\SBOM.template.json` 生成 `release\SBOM-1.0.0.20260527.json` 并填写实际组件版本
- 更新 `docs\CHANGELOG.md`

---

## 快捷入口（本仓库 Qt 路径已写死）

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Release-Windows.ps1"
```

可选参数：`-SmokeOnly`、`-TestsOnly`、`-SkipBuild`、`-Version "1.0.0.20260527"`。
