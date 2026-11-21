# P0-6 · 引擎扫描 / 引擎恢复 / 引擎文件格式匹配

- 日期：2026-11-14
- 触发：用户硬约束「一定要用引擎去扫描和恢复，包括文件格式匹配」
- 红线：源盘 `\\.\PhysicalDrive2` 全程 `GENERIC_READ`、零写入、未执行任何修复；零 GUI 改动；**生产引擎源码不改**（本轮是验证任务，不是改引擎任务）

## 一、改造前的真实缺口

| 环节 | 改造前 | 是否满足约束 |
|---|---|---|
| 发现（哪些文件存在） | `plan.json` 的 ids 全部来自 `candidates_vol*.csv`，即 `tools/mft_oracle.py` 独立解析 MFT 的 Python 清单 | **否**：发现源是独立脚本 |
| 恢复（写出字节） | `phoenix_headless recover` → `RecoverEngine::RecoverBatch()` | 是 |
| 文件格式匹配 | **完全不存在**；只按 MFT 文件名/扩展名记录，验证只比 SHA-256 | **否** |

`phoenix_headless recover` 内部虽然会调用 `NtfsParser::ScanMetadata()` 枚举节点，但那只是「按 oracle 给出的 id 集合去匹配」，决定权仍在 oracle。

## 二、改造后的三段引擎链

```
DISCOVERY   phoenix_headless scan
            └─ NtfsParser::Open / ScanMetadata / GetAllFiles   → engine_nodes_vol*.csv
            └─ RecoverEngine::ReadFileDataPrefix()            → 数据前缀（resident / NTFS 压缩 / runlist 感知）
            └─ SignatureDB::Match() + FormatPayloadScoreUtil  → 引擎格式判定列

RECOVERY    phoenix_headless recover  --engine-format 1
            └─ NtfsParser::ScanMetadata / GetAllFiles         → 按 id 选出 FileNode
            └─ RecoverEngine::RecoverBatch()                  → 写出恢复文件
            └─ SignatureDB::Match(写出的字节前缀)             → 每行结果带格式判定

VERIFY      tools/mft_oracle.py hash / verify_engine_batch.py
            └─ 独立 MFT oracle 只作为**字节级真值**，绝不参与发现或类型判定
```

`SignatureDB` 是生产 GUI 用的同一个类（513 条内建签名、Aho-Corasick 自动机），`FormatPayloadScoreUtil::Score/ProbeExtension` 是生产恢复引擎的格式打分器。编排器里没有任何「按扩展名猜类型」的代码。

### 新增/修改文件

| 文件 | 作用 |
|---|---|
| `tools/phoenix_headless.cpp` | 新增 `scan` 子命令；`enumerate/recover` 增加引擎格式判定；`recover` 结果 CSV 新增 `declaredExt,engineExt,probeExt,formatScore,formatReadOk,formatAgreement,formatMatcher` |
| `tests/CMakeLists.txt` | headless 目标链接 `SignatureDB.cpp` / `UiTypeSignatureSupplement.cpp` / `ExtensionCoverage.cpp` / `UiFileTypeCatalog.cpp`；新增 CTest `EngineScanFormatMatch`；输出名改为 `phoenix_headless_p06.exe`（避免覆盖正在运行的旧 exe） |
| `tests/test_engine_scan_format_match.cpp` | 36 项可证伪门禁（见 §四） |
| `tools/batch_recover.py` | `CANDIDATE_SOURCE = "engine"`；候选读 `engine_nodes_vol*.csv`；`assert_engine_discovery()` 硬门禁；`summarize_engine_format()`；`plan-engine` → `plan_engine.json`；`run --plan` |
| `tools/verify_engine_scan.py` | 从原始 CSV/JSON 重算引擎发现覆盖率与格式判定分布，不信任自述结论 |
| `engine_nodes_vol63.csv` / `engine_nodes_vol209728638.csv` | 引擎扫描产物（发现源） |
| `engine_scan_coverage.json` | 覆盖复核结论 |
| `plan_engine.json` | 引擎驱动的剩余批次计划 |

## 三、真实盘实扫结果（只读）

| 卷 boot LBA | 引擎节点 | 用户文件节点 | 格式探测 | 签名命中 | 坏扇区 |
|---:|---:|---:|---:|---:|---:|
| 63 | 11,134 | 9,856 | 9,856（全量） | 8,765 | 0 |
| 209728638 | 940,662 | 869,721 | 8,174（跨 6 批均匀抽样 id） | 860 | 0 |

- 两次扫描均加载 **513** 条内建签名，日志：`engine_scan_vol63.log` / `engine_scan_vol209728638.log`。
- 大卷未做全量格式探测（869k 次前缀读会拖慢并争抢坏盘 I/O），改由 `recover --engine-format 1` 在恢复阶段对**每个写出的文件**做引擎格式判定，零额外源盘读取。
- 卷 63 的 9,856 次探测中 9,855 次前缀读取成功；大卷 8,174/8,174 成功。
- 大卷签名命中率明显低于卷 63，反映的是**抽样 id 的实际内容分布**（大量无 magic / 压缩 / 加密 / 未知格式），不是匹配器失效；正好说明判定依据是字节而非扩展名。

### 覆盖复核（`tools/verify_engine_scan.py`）

```
ENGINE DISCOVERY COVERS ALL 879546 PLAN IDS
  batch 1..6   requested 9,233 / 619 / 105,901 / 244,040 / 53,715 / 466,038
               missing = 0   (6/6 PASS)
  vol 63          engineOnly=4    oracleOnly=0
  vol 209728638   engineOnly=27   oracleOnly=0
```

`oracleOnly = 0` 说明独立 oracle 列出的每个候选，引擎也都独立发现了；引擎还多发现 31 个节点。历史批次 1–3 虽由早期 oracle 清单驱动（已归档、不重跑），但引擎扫描复核覆盖其全部 id，口径可追溯。

负向守卫 `--negative-guard` 人为注入 1 个引擎未发现的 id → 结论 FAILED、EXIT=1，证明门禁不是摆设。

## 四、可证伪门禁 `EngineScanFormatMatch`

`ctest -R "^EngineScanFormatMatch$"` → **1/1 Passed（36 项 / 0 FAIL）**。

1. 引擎签名库对真实 magic 分类：`jpg / png / pdf / zip / gif / wav`；其中 WAV 走 `RIFF` + offset 8 的 `WAVE` **secondary 子类型判别**。
2. **magic / 扩展名冲突样本**：声明 `txt`、内容 PNG，引擎必须判 `png`；`Score("txt", png)` 不得判 `Complete`。若退化为「按文件名猜类型」，此门禁立即失败。
3. AC 自动机与 legacy Linear 匹配结果逐样本一致。
4. `SignatureDB::ScanBatch()` 流式扫描能在绝对偏移 4096 处命中 PNG 头。
5. 源码级守卫：`phoenix_headless.cpp` 必须调用 `ScanMetadata / GetAllFiles / RecoverBatch / ReadFileDataPrefix / SignatureDB::Match / FormatPayloadScoreUtil::Score`；`batch_recover.py` 必须是 `CANDIDATE_SOURCE = "engine"` 且不含 oracle 候选 csv 作为发现源。

负向：`PHOENIX_NEGATIVE_ENGINE_FORMAT_GUARD=1 ./tests/test_engine_scan_format_match.exe` → **6 项 FAIL / EXIT=1**（模拟「按扩展名猜格式」+「候选取自 oracle」）。

编排器还有运行期硬门禁 `assert_engine_discovery()`：每批恢复前逐个校验本批 id 是否在引擎扫描集合里，缺失即 `RuntimeError`，不会偷偷回退到 oracle。

## 五、引擎驱动的续跑计划

`python tools/batch_recover.py plan --plan-out plan_engine.json --exclude-completed 1 --start-batch 4`

```
candidateSource = engine
4 batches / 762,624 files / 62.96 GiB   (batch 4..7)
排除已完成 id 109,045 个
```

启动：`python tools/batch_recover.py run --plan plan_engine.json`（后台，逐批恢复 → 写记录 → 释放本项目 `recover_staging/batch_NNN` → 下一批）。批次 4 的旧暂存（182,089 个未验证文件 / 20 GiB，后台进程中断时留下、无批次记录）由编排器先 `truncate(0)` 释放再重跑。

## 六、边界与观察项（不得夸大）

- **未完成**：批次 4–7 的恢复/逐文件 SHA-256 比对仍在进行；本文件只固化「引擎口径」的证据链，不声称全部批次已完成。
- **RAW 签名雕刻**：本盘 MFT 完好，发现走 `NtfsParser` 元数据扫描；引擎 `RawScan + SignatureDB::ScanBatch` 的格式匹配能力已由 CTest 锁定，但**未对本盘做全盘 RAW 雕刻**（1 TB、误报基线见 `PERFORMANCE_BASELINE.md §9`）。
- **观察项（P0-6 阶段未改生产引擎；P0-7 已修 `NtfsParser.cpp` 的 `$ATTRIBUTE_LIST` 缺陷，见 `P0-7_attribute_list_fix.md`）**：`scan/SignatureDB.cpp:490` 的 `sqlite` 内建签名注册为 9 字节 `SQLite 3 `（hex `53514C697465203320`），与注释 `SQLite format 3\0` 不符。本轮刻意不修改被测引擎，记为后续独立修复项。
- **格式命中率口径**：`engineExt` 非空 = 513 条内建签名命中；未命中不等于文件损坏，只表示该格式不在签名库或文件无标准 magic。`formatAgreement=mismatch` 可能是扩展名伪造/错误，也可能是签名库优先级导致，不作为恢复成功与否的判据。
- 源盘中途 `forensic_baseline.py --verify` 结论仍为 `SOURCE DISK UNCHANGED`；本轮新增扫描同样只读。

## 七、后续：P0-7 由本轮证据触发的引擎修复

本报告第六节「未改生产引擎」仅描述 P0-6 阶段。P0-6 的引擎发现清单随后暴露了 `$ATTRIBUTE_LIST` 扩展记录漏解析（id 78 / 56,725,784 字节），P0-7 已修改 `fs/ntfs/NtfsParser.cpp`、新增 `NtfsAttributeList` CTest 与负向守卫，并用 `phoenix_headless_p07.exe` 在真实盘上完成字节级复核（SHA-256 完全一致）。归档的 P0-6 CSV / 覆盖报告 / 批次记录保持原样不重写。详见 `P0-7_attribute_list_fix.md`。
