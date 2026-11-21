# Digital Corpora 磁盘镜像测试报告（恢复闭环 · 第二轮）

| 字段 | 值 |
|------|-----|
| 日期 | 2026-06-08 |
| 数据集 | `testdata/digital-corpora-disk-images/` |
| 工具 | `test_digital_corpora_batch.exe` |
| 汇总 CSV | [digital_corpora_results_run2.csv](./digital_corpora_results_run2.csv) |
| 失败明细 CSV | [digital_corpora_recover_failures.csv](./digital_corpora_recover_failures.csv) |
| 运行日志 | [digital_corpora_run.log](./digital_corpora_run.log) |

> 注：`digital_corpora_results.csv` 若在 IDE 中打开会被锁定无法覆盖；本轮完整结果见 `digital_corpora_results_run2.csv`。

---

## 1. 测试闭环说明

| 阶段 | 内容 |
|------|------|
| ① 打开镜像 | `SectorReader::OpenImage` + 扇区 0 读取 |
| ② 识别分区/FS | MBR/VBR → FAT / NTFS / exFAT；无 VBR 时 fallback `PartitionRecoveryService` + `Ext4Parser` |
| ③ 扫描元数据 | 统计总文件数、正常/已删 |
| ④ **恢复写出** | `RecoverEngine::RecoverFile` 对所有可恢复非目录文件 |
| ⑤ 校验 | 写出文件大小 == 目录项 `size` |
| ⑥ 失败明细 | 无法恢复的文件写入 `digital_corpora_recover_failures.csv` |

---

## 2. 恢复汇总（2026-06-08 第二轮）

| 指标 | 数量 |
|------|------|
| 可读镜像（`.dd`/`.raw`） | **20** |
| 参与 FS 扫描 | **19** |
| **可恢复文件合计** | **36,974** |
| **成功恢复** | **36,733（99.35%）** |
| **失败** | **241（0.65%）** |
| 基础设施 FAIL | **0** |
| 总耗时 | ~2.5 min（domexusers 40GiB NTFS 占 ~110s） |

### 按镜像

| 镜像 | 结果 | FS | 可恢复 | 成功 | 失败 | 说明 |
|------|------|-----|--------|------|------|------|
| wiped_disk.dd | SMOKE_ONLY | — | 0 | 0 | 0 | 262 GiB，>50GiB 跳过 |
| canon2 gen1–2,6 | **RECOVER_FULL** | FAT12 | 110 | 110 | 0 | |
| canon2 gen3–5 | RECOVER_PARTIAL | FAT12 | 111 | 108 | 3 | 已删 `_MG_0037.JPG` size_mismatch |
| casper-rw gen0 | SCAN_OK | ext4 | 0 | 0 | 0 | ext 已识别，无可恢复文件 |
| casper-rw gen1–3 | RECOVER_PARTIAL | ext4 | 2,084 | 1,859 | 225 | ext 稀疏/块对齐导致 size 偏差 |
| domexusers.dd | RECOVER_PARTIAL | NTFS | 34,135 | 34,124 | 11 | 11 个 IE 缓存文件名非法路径 |
| ntfs1 gen0 | SCAN_OK | NTFS | 0 | 0 | 0 | 仅元数据，无可恢复内容 |
| ntfs1 gen1–2 | **RECOVER_FULL** | NTFS | 28 | 28 | 0 | |
| ubnist1 gen0–1, gen3.raw | **RECOVER_FULL** | FAT32 | 474 | 474 | 0 | |
| nps-2010-emails | RECOVER_PARTIAL | FAT16 | 32 | 30 | 2 | 损坏目录项/非法文件名 |
| linux-dc3dd-first | SMOKE_ONLY | — | 0 | 0 | 0 | 无识别 FS |

---

## 3. E01 → raw 转换状态

| 状态 | 数量 | 说明 |
|------|------|------|
| 已转换可用 | **11** | casper gen1–3、domexusers、ntfs1 gen0–2、ubnist1 gen0–1、emails、linux-dc3dd-first |
| 跳过（已有 dd） | **8** | canon2 gen1–6、wiped、casper gen0、ubnist1 gen3.raw 等 |
| 转换失败 | **6** | 见下表 |
| 仍 NEEDS_EWF | **5** | E01 在但无有效 dd/raw |
| UNSUPPORTED | **7** | AFF/DMG/001（按约定暂不支持） |

### 转换失败明细

| E01 | 原因 |
|-----|------|
| domexusers.redacted | EWF 分卷缺失（offset 0x24a070000 segment 不在） |
| ubnist1.gen2 | pyewf 读失败（offset 0） |
| ubnist1.gen3.E01 | pyewf 读失败（已有 gen3.raw 可用） |
| linux-dc3dd-second | D: 盘空间不足 |
| windows-ftkimager-first/second | D: 盘空间不足 |

**磁盘空间**：D: 在转换 domexusers（40GiB）后几乎满（~0 字节剩余）。已删除失败转换产生的部分 `.dd`（redacted 9GiB、gen3.dd 1.5GiB、linux-dc3dd-second 0.5GiB）释放 ~11GiB。

---

## 4. 失败分类（241 条）

| 类别 | 数量 | 典型原因 |
|------|------|----------|
| canon2 已删 JPEG | 3 | `_MG_0037.JPG` 删除后部分覆盖，size_mismatch |
| casper-rw ext4 | 225 | `.ko`/缓存文件 i_size 与块分配不完全一致 |
| domexusers NTFS | 11 | IE 临时文件名含 `%`、`[` 等，Windows 无法创建路径 |
| nps-2010-emails | 2 | 损坏/乱码文件名，CreateFile error 123 |

完整列表见 [digital_corpora_recover_failures.csv](./digital_corpora_recover_failures.csv)。

---

## 5. 复现命令

```powershell
# E01 转换（pyewf 已可用）
powershell -File testdata\tools\Expand-EwfToRaw.ps1 -RepoRoot d:\dev\PhoenixRecover -Roots digital-corpora-disk-images

# 构建 + 完整闭环
cd d:\dev\PhoenixRecover\build\win2019-ninja
cmd /c _build_release.bat
powershell -File scripts\Run-DigitalCorporaValidation.ps1
```

可选环境变量：

| 变量 | 说明 |
|------|------|
| `PHOENIX_DC_MAX_FS_BYTES` | 超过此大小的镜像仅 smoke（默认 50GiB） |
| `PHOENIX_DC_STRICT_RECOVER` | 任意恢复失败则退出码 1 |
| `PHOENIX_DC_REPORT_CSV` | 汇总 CSV 路径 |

---

## 6. 后续建议

1. **释放 D: 盘空间** 后补转 `linux-dc3dd-second`、`windows-ftkimager-*`（各 ~1GiB raw）。
2. **ubnist1.gen2.E01** 需检查 EWF 分卷完整性或重新下载。
3. **ext4 size 校验**：casper-rw 大量 `size_mismatch` 可能需对 ext4 使用 `i_size` vs 实际块数更宽松的验收策略。
4. **NTFS 非法路径**：domexusers 的 IE 缓存文件名可跳过或 sanitize 后写出。
