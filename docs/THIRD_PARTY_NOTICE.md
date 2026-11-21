# 第三方组件声明（PhoenixRecover）

本文档列出 PhoenixRecover 分发包中可能包含或链接的第三方软件及其许可证边界。发版前请与 `docs/SBOM.template.json` 一并更新。

## 运行时 / 构建时依赖

| 组件 | 用途 | 许可证 | 分发方式 | 备注 |
|------|------|--------|----------|------|
| Qt 5.14+ | GUI、Core、部分 I/O 抽象 | LGPL v3 / 商业 | 动态链接（windeployqt） | 须随安装包提供 Qt LGPL 合规说明与源码获取方式 |
| Windows SDK | 磁盘扇区读取、minidump、WIC 缩略图 | Microsoft 许可 | 系统 / 静态链接系统库 | 仅 Windows 目标 |
| FFmpeg（可选） | MP4 remux 修复回退 | LGPL / GPL 视构建配置 | 外部可执行文件或动态库 | 若打包 ffmpeg.exe，须在安装目录附带 LICENSE 与构建说明 |
| libzip（可选，`PHOENIX_HAVE_LIBZIP`） | ZIP 修复后归档校验 | BSD-3-Clause | 静态或动态，以 CMake 配置为准 | 默认关闭，不改变对外 API |

## 测试与开发（不随产品默认分发）

| 组件 | 用途 |
|------|------|
| DFTT / CFReDS 镜像 | 集成测试金标 |
| NSIS | 安装包生成 |
| WiX Toolset（可选） | MSI 安装包 |

## 许可证文件位置

- 本项目：`LICENSE`（若存在）
- Qt：随 `windeployqt` 部署的 `Qt5*.dll` 对应 NOTICE
- 可选 libzip：上游 `LICENSE` 副本建议置于 `third_party/libzip/LICENSE`

## SBOM

机器可读清单模板：`docs/SBOM.template.json`。发版时复制为 `release/SBOM-<version>.json` 并填写实际版本号与哈希。

## 合规检查项（发版前）

- [ ] 所有随包 DLL/EXE 已在 NOTICE 中列出
- [ ] LGPL 组件提供源码获取说明（README 或安装包内 TXT）
- [ ] 未将 GPL 组件与闭源模块不当静态链接（若启用 GPL FFmpeg 需法务评审）
- [ ] 可选 libzip 开启时，LICENSE 已并入 `THIRD_PARTY_NOTICE` 与安装包
