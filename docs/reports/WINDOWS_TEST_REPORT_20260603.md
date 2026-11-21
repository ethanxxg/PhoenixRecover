# PhoenixRecover Windows 测试报告

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-03 |
| 平台 | Windows 10 x64 |
| 构建 | VS2019 + Ninja，Qt 5.14.2，`build/win2019-ninja` |
| CTest | **48 / 48 PASS** |

> **dfr-test-images 是否与 NIST 官方一致** → [DFR_TEST_IMAGES_NIST_MATRIX_20260603.md](./DFR_TEST_IMAGES_NIST_MATRIX_20260603.md)（62/62 PASS + 4 无基准）  
> **逐镜像恢复数字（人话）** → [TESTDATA_RECOVERY_RESULTS_20260603.md](./TESTDATA_RECOVERY_RESULTS_20260603.md)  
> **testdata 范围与命令** → [TESTDATA_MASTER_TEST_PLAN.md](./TESTDATA_MASTER_TEST_PLAN.md)

---

## 1. 构建与运行

```powershell
call "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat"
cd d:\dev\PhoenixRecover
cmake -S . -B build\win2019-ninja -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DPHOENIX_BUILD_GUI=OFF -DCMAKE_PREFIX_PATH=C:\Qt\Qt5.14.2\5.14.2\msvc2017_64
cmake --build build\win2019-ninja --parallel 4

$env:PHOENIX_QT_BIN = "C:\Qt\Qt5.14.2\5.14.2\msvc2017_64\bin"
$env:PATH = "$env:PHOENIX_QT_BIN;$env:PATH"
cd build\win2019-ninja
ctest -C Release --output-on-failure
```

说明：MSBuild 生成器因缺 Spectre 库（MSB8040）不可用，已改用 Ninja。

---

## 2. CTest 结果摘要

| 类别 | 数量 | 结果 |
|------|------|------|
| 单元测试 | 15 | 全部 PASS |
| DFTT 集成 | 6 | 全部 PASS（含 FAT/NTFS MD5 金标） |
| CFReDS / DFR 批量 | 11 | 全部 PASS |
| 碎片重组 | 5 | 全部 PASS |
| 分区 / 修复 / 冒烟 | 10 | 全部 PASS |
| **合计** | **47** | **0 FAIL** |

### 核心数据源

| 数据集 | 路径 | 用途 |
|--------|------|------|
| DFTT | `testdata/dftt/` | FAT/NTFS 删除恢复 + 雕刻金标 |
| CFReDS | `testdata/cfreds/` | DFR-01/05 标准场景 |
| DFR 扩展 | `testdata/dfr-test-images/` | 24 场景 × 多文件系统 |
| 雕刻 | `custom-carving-images/`、`file-carving-images/` | manifest / L0–L5 Graphic（[报告](./FILE_CARVING_IMAGES_REPORT_20260603.md)） |

---

## 3. 结论

- **引擎**：扫描、恢复、雕刻、分区探测、碎片重组、修复样本均在 CTest 覆盖范围内通过。
- **dfr-test-images（66 镜像）**：与 NIST FTK 3.3.0 对照见 [逐镜像矩阵](./DFR_TEST_IMAGES_NIST_MATRIX_20260603.md) — **62 有基准全部 PASS**，4 变体无官方表项。
- **66 个 Windows DFR 镜像**（cfreds + dfr-test-images）快速批量：**57 PASS / 9 WARN / 0 FAIL**（16 文件/分区限制，详见恢复结论文档）。
- **限制**：
  - 批量测试每镜像最多尝试恢复 **16** 个已删文件；
  - Digital Corpora 的 **E01** 需 `ewfexport` 转 raw 后才能扫描（本机无工具时 CTest SKIP）。

---

## 4. 证据

| 文件 | 说明 |
|------|------|
| [TESTDATA_RECOVERY_RESULTS_20260603.md](./TESTDATA_RECOVERY_RESULTS_20260603.md) | 逐镜像人话结论 |
| [dfr_windows_recovery_20260603.csv](./dfr_windows_recovery_20260603.csv) | 66 镜像 CSV |
| `build/win2019-ninja/Testing/Temporary/LastTest.log` | 最近一次 CTest 日志 |
