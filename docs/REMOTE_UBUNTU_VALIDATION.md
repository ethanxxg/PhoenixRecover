# 远程 Ubuntu 验证（磁盘占用最小）

目标机示例：`test@192.168.150.128`，源码目录：`/home/test/Desktop/linux/PhoenixRecover`。

## 最小测试数据

仅同步 `testdata/tools/linux-minimal-testdata.manifest` 中列出的路径（默认 **dfr-01-ext.dd.bz2**，约数百 MB 压缩包；在目标机解压为 ~1GB `.dd`）。

| 阶段 | 内容 |
|------|------|
| 传输 | `.bz2` 压缩镜像 + 验证脚本 |
| 目标机 | `bunzip2 -k` 解压 → 构建 → CTest + `Run-DfrLinuxValidation.sh` |

## 步骤 1：Windows 推送最小数据

```powershell
cd C:\Users\TH\Desktop\dev\test\PhoenixRecover
powershell -ExecutionPolicy Bypass -File testdata\tools\Push-RemoteUbuntuValidation.ps1 `
  -RemoteHost 192.168.150.128 -RemoteUser test -RemotePassword 123456
```

首次同步源码（排除 `build/`、大体积 `testdata`）：

```powershell
powershell -File testdata\tools\Push-RemoteUbuntuValidation.ps1 -SyncSourceCode
```

## 步骤 2：SSH 登录目标机执行闭环

```bash
cd /home/test/Desktop/linux/PhoenixRecover
chmod +x testdata/tools/Remote-Ubuntu-Validation.sh
bash testdata/tools/Remote-Ubuntu-Validation.sh
```

## 批量全量（推荐：一轮只测一个镜像）

当本地 `testdata/dfr-test-images/**/**-ext.dd.bz2` 已就位（例如用 `testdata/dfr-test-images/download-all.ps1` 下载）时，可用循环脚本在 Ubuntu 上逐个解压/验证，并在每轮结束后删除该轮镜像，避免远程磁盘被占满：

```powershell
cd C:\Users\TH\Desktop\dev\test\PhoenixRecover
python testdata\tools\Remote-Ubuntu-Loop.py --host 192.168.150.128 --user test --password 123456
```

输出（运行后生成，不纳入版本库）：

- `docs/reports/REMOTE_UBUNTU_LOOP.log`
- `docs/reports/REMOTE_UBUNTU_LOOP_SUMMARY.csv`

Linux 回归以 CTest `DFR_Linux_Batch`、`CFReDS_EXT4_DFR01` 为准；Windows 侧报告见 `docs/reports/README.md`。

## 磁盘建议

- 仅 **dfr-01**：压缩包 + 解压后约 **1GB+** 镜像 + 构建产物（`build/linux-ubuntu-val` 数百 MB）。
- 勿同步整个 `dfr-test-images`（全量解压可达数十 GB）。
- 验证完成后可删除 `.dd` 仅保留 `.bz2`：`rm testdata/dfr-test-images/dfr-01/dfr-01-ext.dd`

## 远程 Invoke 脚本防卡死（自动清理）

所有 `testdata/tools/Invoke-*.py` 与 `Remote-Ubuntu-Loop.py` 共用 `invoke_remote_guard.py`：

- **启动时**：结束本机同脚本、同主机的旧 Python/PowerShell 残留进程（避免 SSH 等 `sudo` 密码挂起）。
- **运行中**：远程命令有总时长与无输出空闲超时，超时自动断开（退出码 124）。
- **退出时**：释放 `%TEMP%` 下的会话锁文件。

生成 ext4 变体 / 仅验证变体：

```powershell
python testdata\tools\Invoke-GenerateExt4VariantsRemote.py --host 192.168.150.128 --user test --password 123456
python testdata\tools\Invoke-VerifyExt4VariantsRemote.py --host 192.168.150.128 --user test --password 123456
```
