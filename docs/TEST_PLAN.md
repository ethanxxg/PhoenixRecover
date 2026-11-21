# PhoenixRecover 测试计划

> **版本**: 1.0  
> **日期**: 2026-05-26  
> **维护者**: PhoenixRecover 开发团队

---

## 1. 测试策略概述

PhoenixRecover 的测试体系分为四个层级，按执行频率从高到低排列：

| 层级 | 类型 | 依赖环境 | 执行方式 | 目标 |
|------|------|----------|----------|------|
| L1 | 单元测试 | 纯 C++ | CTest 自动 | 验证独立算法与数据结构的正确性 |
| L2 | 集成测试 | Windows + Qt | CTest 自动 | 验证 I/O→解析→文件列表 的端到端流程 |
| L3 | 性能测试 | Windows + Qt + 大镜像 | 脚本半自动 | 验证扫描速度、内存占用在可接受范围内 |
| L4 | 手工验收测试 | 完整 GUI 环境 | 人工执行 | 验证用户可见的 UI 流程与恢复质量 |

### 1.1 质量门禁

- **提交前**: L1 全部通过（`scripts/Regress.ps1 -TestsOnly`）
- **合并前**: L1 + L2 全部通过
- **发版前**: L1 + L2 + L3 + L4 P0 用例全部通过

---

## 2. 单元测试清单（L1）

所有单元测试位于 `tests/` 目录，通过 CTest 注册。

| CTest 名称 | 源文件 | 依赖 | 测试内容 |
|------------|--------|------|----------|
| `UtilsNumeric` | `test_utils_numeric.cpp` | 纯 C++ | `ReadLE16/32/64`、`ReadSignedLE` 符号扩展、`AlignUp` 对齐 |
| `FatStructs` | `test_fat_structs.cpp` | 纯 C++；Windows 下链接 `FatStructs.cpp` | FAT BPB 结构体 sizeof、FAT12/16/32 常量、`FatDateTimeToFiletime`（Windows-only 分支） |
| `NtfsStructs` | `test_ntfs_structs.cpp` | 纯 C++ | NTFS 结构体 sizeof、`DecodeDataRuns` 算法（稀疏/密集/负偏移） |
| `CarveCommon` | `test_carve_common.cpp` | 纯 C++ | `CarveResult` 默认值、赋值、拷贝语义 |
| `ScanProgress` | `test_scan_progress.cpp` | 纯 C++ | `ScanProgress`、`UnifiedScanConfig` 默认值验证（A1-10 起新增 `rawScanThreads`，默认 1） |
| `SignatureDB` | `test_signature_db.cpp` | Qt5::Core（仅 GUI 构建） | 内建签名匹配 |
| `SignatureDBAC` | `test_signature_db_ac.cpp` | 纯 C++，`PHOENIX_BUILD_GUI=0` | AC 匹配与线性匹配行为等价、真实签名集提速 |
| `SignatureDBLoad` | `test_signature_db_load.cpp` | 纯 C++，`PHOENIX_BUILD_GUI=0` | **A1-9**：零依赖 `SignatureJson.h` 解析器属性与严格拒绝、`LoadFromFile` 字段逐项核对、无效条目跳过、hex 校验、AC 匹配器重建（`Match`/`ScanBatch`）、`testdata/custom_signatures.json` 真实样例；源码级负向验证 |
| `RawScanParallel` | `test_rawscan_parallel.cpp` | 纯 C++，`PHOENIX_BUILD_GUI=0` | **A1-8**：8 MiB 合成镜像，N=2/4/8 与串行逐偏移（rawOffset/ext/size）完全一致、chunk 边界 512 B 重叠去重 |
| `RawScanParallelWiring` | `test_rawscan_parallel_wiring.cpp` | 纯 C++，`PHOENIX_BUILD_GUI=0`；**链接生产 `CarveModuleService.cpp`** | **A1-10**：26 项。配置默认值 / `MakeCarveConfig` 映射、精确签名集构造器、自定义与过滤签名库下 `CarveModuleService` 的 parallel==serial、`targetExtensions` 真实生效、回调零跨线程、取消传播、源码级接线守卫；两轮源码级负向验证（断开 `SetThreadCount`、worker 回退内建库） |

> **定向回归命令（MinGW / headless）**：`ctest --output-on-failure -R "^(UtilsNumeric|FormatPayloadScore|ScanProgress|SignatureDBAC|SignatureDBLoad|RawScanAC|RawScanParallel|RawScanParallelWiring|DeepScanFallback|SignatureMatcher)$"`。全量 `mingw32-make -j4` 受既有 MinGW g++ 7.3 `<filesystem>` 缺失阻塞（`test_utils_unique_path.cpp` / `test_fat_fragment_stitch_synthetic.cpp`），属工具链差异。
>
> **性能基准（A1-11，非 CTest）**：`bench_rawscan_parallel` 由 CMake 选项 `PHOENIX_BUILD_BENCH=ON` 启用（默认 OFF），计时断言不接入 CTest 以免抖动；复现命令与实测数据见 [`PERFORMANCE_BASELINE.md`](./PERFORMANCE_BASELINE.md) §8。

### 执行命令

```powershell
# 仅运行单元测试（无需 Qt / GUI）
scripts/Regress.ps1 -TestsOnly

# 或手动
cmake -S . -B build -DPHOENIX_BUILD_GUI=OFF
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

---

## 3. 集成测试清单（L2）

集成测试位于 `tests/integration/`，使用 DFTT（Digital Forensic Tool Testing）标准镜像，验证文件系统解析器的端到端正确性。

### 3.1 前置条件

- 操作系统：Windows（SectorReader 依赖 `CreateFile` API）
- 构建选项：`PHOENIX_BUILD_GUI=ON`（需要 Qt5）
- 测试数据：`testdata/dftt/` 目录下的 `.dd` 镜像文件

### 3.2 测试用例

| CTest 名称 | 源文件 | 镜像 | 验证内容 |
|------------|--------|------|----------|
| `DFTT_FAT_Undelete` | `test_dftt_fat_undelete.cpp` | `6-undel-fat/6-fat-undel.dd` | 见下方详述 |
| `DFTT_NTFS_Undelete` | `test_dftt_ntfs_undelete.cpp` | `7-undel-ntfs/7-ntfs-undel.dd` | 见下方详述 |

#### 3.2.1 DFTT FAT Undelete

**镜像说明**: FAT16 文件系统，包含 6 个已删除文件和 2 级嵌套目录。

**验证项**:

| # | 检查点 | 预期值 |
|---|--------|--------|
| 1 | `SectorReader::OpenImage()` 成功 | `true` |
| 2 | `FatParser::Open()` 成功 | `true` |
| 3 | `FatParser::ScanMetadata()` 成功 | `true` |
| 4 | 找到 `sing.dat` | 大小 = 780 B |
| 5 | 找到 `mult1.dat` | 大小 = 3801 B |
| 6 | 找到 `frag1.dat` | 大小 = 1584 B |
| 7 | 找到 `frag2.dat` | 大小 = 3873 B |
| 8 | 找到 `dir1/mult2.dat` | 大小 = 1715 B |
| 9 | 找到 `dir1/dir2/frag3.dat` | 大小 = 2027 B |
| 10 | `dir1` 目录节点存在 | `isDirectory = true` |
| 11 | `dir2` 目录节点或路径可达 | 通过路径或节点验证 |

#### 3.2.2 DFTT NTFS Undelete

**镜像说明**: NTFS 文件系统，包含驻留文件、多簇文件、碎片文件、多级子目录文件及 MFT 记录已被复用的目录。

**验证项**:

| # | 检查点 | 预期值 |
|---|--------|--------|
| 1 | `SectorReader::OpenImage()` 成功 | `true` |
| 2 | `NtfsParser::Open()` 成功 | `true` |
| 3 | `NtfsParser::ScanMetadata()` 成功 | `true` |
| 4 | 找到 `res1.dat`（驻留文件） | 大小 = 101 B，`residentData` 非空 |
| 5 | 找到 `sing1.dat` | 大小 = 780 B |
| 6 | 找到 `mult1.dat` | 大小 = 3801 B |
| 7 | 找到 `frag1.dat` | 大小 = 1584 B |
| 8 | 找到 `frag2.dat` | 大小 = 3873 B |
| 9 | 找到 `dir1/mult2.dat` | 大小 = 1715 B |
| 10 | 找到 `dir1/dir2/frag3.dat` | 大小 = 2027 B |
| 11 | 找到 `dir3/sing2.dat` | 大小 = 1005 B |

### 3.3 数据基线

预期文件名、大小和 MD5 记录在 `testdata/manifests/expected_results.csv`，来源为 DFTT 官方文档。

### 3.4 执行命令

```powershell
# 完整构建（含 GUI + 集成测试）
cmake -S . -B build -DPHOENIX_BUILD_GUI=ON -DCMAKE_PREFIX_PATH="<Qt5路径>"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

---

## 4. 性能测试计划（L3）

### 4.1 测试场景

| 场景 | 镜像/设备 | 指标 | 阈值 |
|------|----------|------|------|
| 小镜像 FAT 扫描 | `6-fat-undel.dd`（~1 MB） | 扫描耗时 | < 1 秒 |
| 小镜像 NTFS 扫描 | `7-ntfs-undel.dd`（~1 MB） | 扫描耗时 | < 1 秒 |
| 中型 NTFS 扫描 | 用户提供 10–50 GB NTFS 镜像 | 扫描速率 | ≥ 100 MB/s |
| 大型结果集渲染 | 10 万条 `FileNode` 加载到 UI | 首次渲染耗时 | < 3 秒 |
| 内存占用 | 10 万条 `FileNode` 扫描 | 峰值 RSS | < 500 MB |

### 4.2 测量方法

- 使用 `std::chrono::high_resolution_clock` 在测试代码中计时
- 使用 Windows 性能计数器或 `Process Explorer` 监控内存
- 统一扫描流水线中 `ScanResult.duration` 字段已内置计时

### 4.3 基线记录

首次通过后记录为基线，后续版本如回归超过 20% 需调查。

---

## 5. 手工验收测试计划（L4）

### 5.1 P0 必测用例（发版阻断）

| ID | 用例 | 步骤 | 预期结果 |
|----|------|------|----------|
| A-01 | USB 磁盘扫描 | 插入 USB → 选择磁盘 → 扫描 → 等待完成 | 扫描完成，文件列表正常显示 |
| A-02 | 删除文件恢复 | 在 A-01 结果中勾选已删除文件 → 选择目标目录 → 恢复 | 文件成功恢复，内容可正常打开 |
| A-03 | 同卷保护 | 恢复时选择源磁盘同一分区作为目标 | 弹出阻止提示，不允许恢复 |
| A-04 | 扫描取消 | 扫描进行中 → 点击取消 | 扫描停止，UI 不崩溃，可重新扫描 |
| A-05 | 镜像文件扫描 | 选择 .dd 镜像文件 → 扫描 | 等同于物理磁盘扫描，文件列表正确 |

### 5.2 P1 扩展用例

| ID | 用例 | 步骤 | 预期结果 |
|----|------|------|----------|
| B-01 | NTFS 驻留文件恢复 | 扫描 NTFS 卷 → 找到小于 700B 的文件 → 恢复 | 文件内容完整 |
| B-02 | FAT32 碎片文件恢复 | 扫描 FAT32 卷 → 恢复碎片文件 | 文件内容正确（MD5 匹配） |
| B-03 | 预览功能 | 扫描结果中点击图片/文档文件 | 右侧预览面板显示缩略图 |
| B-04 | 筛选与排序 | 搜索框输入关键词 / 点击表头排序 | 结果正确过滤和排序 |
| B-05 | 会话保存/恢复 | 扫描完成 → 关闭程序 → 重新打开 | 上次扫描结果自动加载 |
| B-06 | NTFS 压缩文件恢复 | 扫描含 LZNT1 压缩文件的 NTFS 卷 → 恢复 | 文件解压正确 |

---

## 6. 回归测试流程

### 6.1 自动化回归（CTest）

```powershell
# 全量回归（单元 + 集成）
scripts/Regress.ps1

# 仅单元测试（无需 Qt）
scripts/Regress.ps1 -TestsOnly
```

`Regress.ps1` 执行流程：
1. `cmake` 配置并编译
2. `ctest` 运行所有注册测试
3. （可选）对指定恢复输出目录执行结果比对（见 §6.2）

**W1 双工具链可信基线（2026-11-08 实测）**：主力 `build-gcc14` 用 MSYS2 GCC 14.2.0；`build-ac`（Qt MinGW g++ 7.3.0）因缺 `<filesystem>` 无法全量构建，但 W2/W3/W5 新增测试已双工具链编译通过。

```bash
# GCC 14.2：全量构建（PHOENIX_BUILD_GUI=OFF）
cd build-gcc14 && PATH="/c/msys64/mingw64/bin:$PATH" mingw32-make -k -j6

# GCC 14.2：bounded 回归（含 W2/W3/W5 + 5 个 carving 集成）
# PATH 顺序是硬要求：旧版 libstdc++-6.dll 抢先加载会让 std::filesystem 相关
# exe 以 0xc0000139 启动失败。
PATH="/c/msys64/mingw64/bin:$PATH" ctest --output-on-failure -R "^(UtilsNumeric|...|ExfatRecoverBytes|...|FileCarving_Graphic_All)$" --timeout 240

# GCC 7.3：定向验证 W2/W3/W5
cd ../build-ac && PATH="/c/Qt/Qt5.14.2/Tools/mingw730_64/bin:$PATH" \
  mingw32-make -j4 test_exfat_recover_bytes test_exfat_probe_dispatch test_rawscan_false_positive
PATH="/c/Qt/Qt5.14.2/Tools/mingw730_64/bin:$PATH" \
  ctest --output-on-failure -R "^(ExfatProbeDispatch|ExfatRecoverBytes|RawScanFalsePositive)$"
```

**2026-11-08 实测结果**（`docs/reports/regression_baseline_2026-11-08.txt`）：

| 构建目录 | 结果 |
| --- | --- |
| `build-gcc14` 全量构建 | `BUILD_EXIT=0` |
| `build-gcc14` 最终快速 33 项 | **33/33 passed，84.92 s**（含 W2/W3/W5 四个新门禁 + 4 个 carving 集成 + DFR 批量） |
| `build-gcc14` `FileCarving_Graphic_All` | 1/1 passed，**245.39 s**，须 `--timeout 900`；`--timeout 240` 会误报 Timeout |
| `build-gcc14` `DFR_Windows_Batch` | 1/1 passed，12/12 镜像 PASS，1.30 s |
| `build-ac`（GCC 7.3）W2/W3/W5 + 格式化卷定向 | 原 3/3 passed（3.14 s）；`FormattedVolumeRecovery` 另编译运行 22/22 通过 |

**未纳入 bounded 基线**：`DFTT_FAT_RecoverBytes` / `DFTT_NTFS_RecoverBytes`（暴力簇重建，GCC 14.2 下 >6 min CPU，需单独性能 triage）；CFReDS 1 GiB 大镜像批量用例。

### 6.2 恢复结果对比

> **W4 口径校正**：旧版本节描述的 `testdata/tools/Compare-RecoveryResults.ps1` **在当前仓库中并不存在**，`testdata/manifests/expected_results.csv` 也不是「恢复文件名 → MD5/大小」基线格式，而是 W3 建立的**场景矩阵**（`scenario_id,fs,image,start_sector,ctest,check_type,status,evidence,notes`）。因此不存在可执行的通用恢复目录自动比对脚本，旧描述不可作为交付能力引用。

当前实际落地的内容校验由 CTest 承担：

| 门禁 | 校验方式 |
|------|----------|
| `ExfatRecoverBytes` | 独立手写 exFAT oracle 逐字节比对 + MD5 + 整镜像 FNV-1a 只读不变量 |
| `DFTT_*_RecoverBytes` | DFTT 金标恢复字节（暂未纳入 bounded 基线，见 §6.1） |
| `CustomCarving_*` / `FileCarving_*` | 金标 manifest 字节与 MD5 |
| `RawScanFalsePositive` | 已知植入真值下的 TP / FP / FN |

### 6.3 回归报告

| 字段 | 说明 |
|------|------|
| `run_id` | 运行唯一标识（日期 + 构建号） |
| `build_version` | Git commit SHA 或版本号 |
| `case_id` | CTest 名称或手工用例 ID |
| `dataset_id` | 使用的测试数据集标识 |
| `result` | PASS / FAIL / SKIP |
| `evidence` | 日志路径、截图或对比报告路径 |

---

## 7. 测试数据管理

### 7.1 目录结构

```
testdata/
├── dftt/                        # DFTT 标准镜像
│   ├── 6-undel-fat/             # FAT Undelete (#6)
│   │   └── 6-fat-undel.dd
│   ├── 7-undel-ntfs/            # NTFS Undelete (#7)
│   │   └── 7-ntfs-undel.dd
│   └── 11-carve-fat/            # FAT File Carving (#11)
│       └── 11-carve-fat.dd
├── media/                       # 多媒体测试样本
├── manifests/
│   ├── datasets.csv             # 样本清单
│   └── expected_results.csv     # 预期恢复结果基线
├── tools/
│   └── Compare-RecoveryResults.ps1
└── README.md
```

### 7.2 数据来源

详见 `docs/TEST_DATA_SOURCES.md`。

---

## 8. 未来扩展

- [x] exFAT **自动探测链**集成测试：CTest `ExfatProbeDispatch`（W2/W3，合成 boot + `dfr-01-xfat.dd` 真实金标）——解析器早已实现，旧条目「待 ExfatParser 实现」为过时表述
- [x] exFAT DFR-01 **恢复字节 / MD5**门禁：CTest `ExfatRecoverBytes`（W3）——独立手写 exFAT oracle 解析 boot 与已删根目录项，`RecoverEngine` 写出的 4396 字节须逐字节相等且 MD5 一致；同时校验整镜像 FNV-1a 前后不变。该门禁发现并修复了 CAR-003（已删 `NoFatChain` 连续流被 FAT 拼接器改写，详见 `expected_results.csv`）
- [ ] ISO 9660 集成测试（`testdata/dftt/14-iso9660-1/`）
- [x] 格式化后恢复（合成金标）：CTest `FormattedVolumeRecovery`（W3，22 项，GCC 7.3 / GCC 14.2 双绿）——真实快速格式化磁盘仍待补
- [ ] 文件雕刻（File Carving）集成测试（`11-carve-fat.dd`）
- [~] 恢复内容 MD5 验证（端到端恢复 + 比对）：`ExfatRecoverBytes` 已对 1 个真实 exFAT 已删文件做到「恢复写出 → 逐字节 + MD5 + 源镜像只读哈希」全链校验；**覆盖面仍不足**，FAT/NTFS/ext4 / 多簇 / 碎片化场景尚无等价门禁
- [ ] CI/CD 管线集成（GitHub Actions / Azure Pipelines）
- [ ] 模糊测试（随机损坏镜像的鲁棒性验证）
