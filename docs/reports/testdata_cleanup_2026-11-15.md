# testdata 空间整理（2026-11-15）

| 字段 | 值 |
|------|-----|
| 触发 | 用户要求整理 `D:\dev\PhoenixRecover\testdata`，可清即清 |
| 执行 | `py -3 tools/testdata_cleanup.py --apply --only wiped_disk` |
| 日志 | `docs/reports/testdata_cleanup_2026-11-15.json` |
| 结果 | testdata 426 GiB → **163 GiB**；D: 可用 138 GiB → **400 GiB** |

---

## 1. 清理口径

判定原则：**只删「可重生成的派生副本」与「字节重复件」，不动任何原始下载件与金标**。

| 类别 | 规则 | 处理 |
|------|------|------|
| E01 展开产物 | 同目录存在同名 `.E01/.E02/.E03/.aff/.raw/.zip` 的 `.dd` | 本轮仅删无测试价值者 |
| 全零擦除盘 | `wiped_disk.dd`（54 MB E01 展开成 262 GiB 零） | **删除，且不得重新展开** |
| 字节重复 | `.old` 与同名文件 MD5 相同 | 删除 |
| 下载器残留 | `*.larkcache.linco`（二进制缓存） | 删除 |
| 原始下载件 | `.E01/.E02/.E03/.aff/.raw/.zip/.dmg/.pdf/.xml` | 一律保留 |
| 金标语料 | `cfreds/ dfr-test-images/ dftt/ *-golden/ custom-carving-images/ …` | 一律保留 |

## 2. 实际删除

| 文件 | 体积 | 理由 |
|------|------|------|
| `digital-corpora-disk-images/circl-2023-wiped/wiped_disk.dd` | 262.35 GiB | 54 MB `wiped_disk.E01` 的全零展开；`test_digital_corpora_batch` 对 >50 GiB 仅 SMOKE，从不扫描 |
| `digital-corpora-disk-images/nps-2009-domexusers/nps-2009-domexusers.xml.old` | 36.5 MiB | 与 `nps-2009-domexusers.xml` MD5 完全一致（`1c52a490…`） |
| `digital-corpora-disk-images.larkcache.linco` | 82 B | 网盘下载器缓存残留 |

`wiped_disk.E01`、`domexusers.xml` 均保留。

## 3. 删除机制（易踩的坑）

本机文件删除默认进入**回收站**：目录项立即消失、但簇未释放，`df`/`fsutil` 仍报占用。
262 GiB 的 `$RVCBDUG.dd` 与 `$RBA06RO.old` 需在 `D:\$RECYCLE.BIN\S-1-5-…-1001\`
中连同配对 `$I*` 元数据一并移除，空间才真正回落（清理后 555 G 已用）。

> 回收站中仍有用户自有 2020–2022 年 `.mp4` 约 30 GiB，**未触碰**。

## 4. 保留但可再释放（未执行）

其余 20 个 E01 展开 `.dd` 合计 **50.2 GiB**（domexusers 40 GiB、ubnist1 gen0/gen1 各 2 GiB、
casper-rw 3×600 MiB、ftkimager/dc3dd 3×963 MiB、ntfs1 3×492 MiB、canon2 6×29.7 MiB 等）。

保留理由：它们是 `DigitalCorpora_Batch_All` 唯一可用的可读语料（domexusers 一次可恢复 34,135 个
NTFS 文件）；删掉后该 CTest 不会失败，但会退化成 `NEEDS_EWF` 空跑。

若要释放：

```bash
py -3 tools/testdata_cleanup.py --apply            # 全量 20 项，50.2 GiB
# 之后必须清空回收站对应项才真正释放
# 需要时重新展开（pyewf 只在 py -3 下可用）：
powershell -File testdata/tools/Expand-EwfToRaw.ps1 -RepoRoot . -Roots digital-corpora-disk-images
```

## 5. 回归验证

`build-gcc14`（MSYS2 GCC 14.2.0，PATH 首位 `/c/msys64/mingw64/bin`）定向 12 项：

| 结果 | 测试 |
|------|------|
| PASS 11/12 | SignatureDBAC、SignatureDBLoad、ExfatProbeDispatch、ExfatRecoverBytes、ExfatMulticlusterBytes、FormattedVolumeRecovery、VirtualDiskSource、VirtualDiskGolden、CustomCarving_ManifestGolden、CustomCarving_ManifestLayout、CFReDS_DFR05_BRAID_FAT |
| FAIL 1 | `DFTT_11_CARVE_FAT`：9 个金标 MD5 未雕出（`enterprise.wav`/`holly.xls`/`lin_1.2.pdf`/…） |

`DFTT_11` 属**既有失败**，与本次清理无关：删除清单只含 `digital-corpora` 下 2 个文件，
`testdata/dftt/` 未被触碰；失败形态是「引擎没雕出期望文件」，不是「镜像缺失」。

金标完整性逐项确认：`cfreds/dfr-01-xfat.dd`、`custom_signatures.json`、`signature-matcher-golden`、
`bitlocker-golden`、`raid-probe-golden`、`refs-golden`、`virtual-disk-golden`、`dftt`、
`forensics-samples`、`sleuthkit_test_data`、`custom-carving-images`、`dfr-test-images`、
`manifests/expected_results.csv` —— 全部存在。

## 6. 附注：坏盘重插后未枚举

`Get-Disk` 只有 Disk 0/1（两块三星 NVMe），USB 层亦无 Mass Storage/UASP 设备，
说明桥接盒未被系统识别（供电/线材/接口或桥接芯片掉线），并非盘体可见但读失败的状态。
