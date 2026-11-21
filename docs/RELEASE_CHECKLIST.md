# PhoenixRecover 发版检查清单

本文档定义了 PhoenixRecover 从代码冻结到正式发布的完整检查流程。每次发版时复制此清单，逐项确认完成后方可发布。

---

## 发版前检查

- [ ] 代码冻结，所有 P0 bug 已修复
- [ ] PRD 中 P0 功能全部实现
- [ ] 构建验证（Release 配置，无警告）
- [ ] CTest 全部通过
- [ ] DFTT 镜像集成测试通过（`Compare-RecoveryResults -Strict`）
- [ ] TEST_EXECUTION_MATRIX P0 用例 100% 通过
- [ ] 性能基准无显著回退
- [ ] UI 无明显视觉缺陷
- [ ] 许可证文件完整（LICENSE、第三方声明）
- [ ] CHANGELOG.md 已更新
- [ ] 版本号已更新（CMakeLists.txt / .rc 文件）

---

## 构建发布包

- [ ] Release 配置构建
- [ ] windeployqt 打包 Qt 依赖
- [ ] NSIS/WiX 安装包生成
- [ ] 安装包冒烟测试（安装 -> 启动 -> 扫描 -> 卸载）
- [ ] 代码签名（如有证书）

### 发布前门禁（测试 + 可选安装包）

本机 Qt Kit 根目录（**非** `bin`）：`C:\Qt\Qt5.14.2\5.14.2\msvc2017_64`

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\PreRelease.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release
```

完整步骤见 [WINDOWS_RELEASE_RUNBOOK.md](./WINDOWS_RELEASE_RUNBOOK.md)；快捷命令：`scripts\Release-Windows.ps1`

仅 CTest（无 Qt GUI）：`-TestsOnly`

### 推荐命令（NSIS）

```powershell
powershell -ExecutionPolicy Bypass -File "scripts\Build-Release.ps1" `
  -QtPrefix "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64" `
  -Config Release `
  -OutputDir "release" `
  -Installer NSIS `
  -Version "1.0.0.20260527" `
  -RunSmokeTest
```

### 单独生成安装包

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Create-Installer.ps1" `
  -SourceDir "release" -Version "1.0.0.20260527"
```

### 单独运行冒烟测试

```powershell
powershell -ExecutionPolicy Bypass -File "scripts/Smoke-Installer.ps1" `
  -InstallerPath "release\PhoenixRecover-Setup-1.0.0.20260527.exe"
```

---

## 发布后

- [ ] GitHub Release / 分发渠道上传
- [ ] 更新项目 README
- [ ] 通知用户/团队
