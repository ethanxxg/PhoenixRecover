# 测试报告索引

本目录只保留 **2026-06-03** 最终结论与可复现数据。中间过程日志、旧版 CSV 已删除。

| 文档 | 用途 |
|------|------|
| **[DFR_TEST_IMAGES_NIST_MATRIX_20260603.md](./DFR_TEST_IMAGES_NIST_MATRIX_20260603.md)** | **dfr-test-images 逐镜像 vs NIST FTK**（62 有基准 + 4 无基准） |
| [FILE_CARVING_IMAGES_REPORT_20260603.md](./FILE_CARVING_IMAGES_REPORT_20260603.md) | **file-carving-images** L0–L5 Graphic 雕刻 vs 官方布局 |
| [CUSTOM_CARVING_IMAGES_REPORT_20260605.md](./CUSTOM_CARVING_IMAGES_REPORT_20260605.md) | **custom-carving-images** 20 镜像 manifest 布局 + RawScan 批测 |
| [DIGITAL_CORPORA_DISK_IMAGES_REPORT_20260608.md](./DIGITAL_CORPORA_DISK_IMAGES_REPORT_20260608.md) | **digital-corpora-disk-images** SectorReader + FS 批测 |
| [file_carving_graphic_results.csv](./file_carving_graphic_results.csv) | 上表机器可读 CSV |
| [custom_carving_layout_results.csv](./custom_carving_layout_results.csv) | custom-carving 布局 1100 项 |
| [custom_carving_results.csv](./custom_carving_results.csv) | custom-carving RawScan 批测 |
| [digital_corpora_results.csv](./digital_corpora_results.csv) | digital-corpora 批测汇总 |
| [digital_corpora_recover_failures.csv](./digital_corpora_recover_failures.csv) | digital-corpora 恢复失败明细 |
| [dfr_nist_matrix.csv](./dfr_nist_matrix.csv) | 上表机器可读 CSV |
| [WINDOWS_TEST_REPORT_20260603.md](./WINDOWS_TEST_REPORT_20260603.md) | Windows 平台 **47 项 CTest** 结果与环境 |
| [TESTDATA_RECOVERY_RESULTS_20260603.md](./TESTDATA_RECOVERY_RESULTS_20260603.md) | **逐镜像**：删几个、恢复几个（人话结论） |
| [TESTDATA_MASTER_TEST_PLAN.md](./TESTDATA_MASTER_TEST_PLAN.md) | `testdata/` 全量范围、阶段划分、执行命令 |
| [NIST_DFR_COMPARE_20260603.md](./NIST_DFR_COMPARE_20260603.md) | NIST 对比背景、引擎修复、CTest 命令 |
| [dfr_nist_compare_run.csv](./dfr_nist_compare_run.csv) | 批量脚本原始 CSV |

**阅读顺序**：问「是否与官方一致」→ **NIST 逐镜像对照表** → 其余报告。
