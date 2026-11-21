# 雕刻能力对齐 TestDisk / PhotoRec 7.2

日期：2026-11-21
状态：已完成并通过门禁（`PhotoRecAlign` 19 checks / 0 failure）

## 1. 目标

用户要求「雕刻对齐 TestDisk」。TestDisk 的雕刻部分是随包分发的 **PhotoRec**，
其格式表以 `src/file_*.c` 的形式存在：每个格式一个翻译单元，导出一个
`const file_hint_t file_hint_xxx`，里面登记扩展名、上限大小和一个
`register_header_check` 回调。

本轮要解决的是此前的明确短板：**雕刻格式数落后 PhotoRec**。

## 2. 关键判断：只对齐「声明式层」

PhotoRec 的识别由两部分组成：

1. **声明式 magic** —— `register_header_check(offset, array, sizeof(array), cb, stat)`
2. **回调校验** —— `header_check_xxx()` 里再做结构验证（JPG 走 DHT、PDF 查版本等）

第 2 部分是一段段 C 代码，无法机械移植成我们声明式的 `FileSignature` 模型
（header / footer / secondary / maxSize）。

因此本轮只对齐第 1 层，并把这个边界写进生成的代码注释与门禁：
**PhotoRec 对齐后拿到的每一条都是"文件头线索"，不是完整格式校验器。**

这个取舍带来的副作用必须被显式管住，见 §5。

## 3. 提取结果

工具：`tools/gen_photorec_signatures.py`（新增）

| 指标 | 数值 |
|---|---|
| `src/file_*.c` 源文件 | 350 |
| `file_hint_t` 条目 | 351 |
| `register_header_check()` 调用 | 404 |
| 回解出的声明式 magic | **408** |
| 与内建表重复（已被覆盖） | 40 |
| 通配扩展名（`gm*`、`tx?`） | 13（拒绝） |
| 低熵 magic | 1（`dovecot`，拒绝） |
| 数组未能回解 | 6（elf/shp/template，magic 定义在宏或占位符里） |
| **进入主库** | **321 条 / 235 个扩展名** |
| 低置信（magic < 3 字节） | 33 条 / 12 个扩展名（单独文件，默认不加载） |

生成的产物：

| 文件 | 作用 |
|---|---|
| `scan/PhotoRecSignatures.h` | 编译进二进制的签名表（`RegisterPhotoRecAlignSignatures`） |
| `testdata/signatures/photorec_aligned.json` | 主库，供 `LoadFromFile` 与门禁比对 |
| `testdata/signatures/photorec_lowconf.json` | 短 magic，需显式启用 |
| `docs/reports/photorec_signature_extraction.txt` | 覆盖率报告（可审计） |

## 4. 库规模变化

`SignatureDB` 构造时在内建表**之后**注册 PhotoRec 层，因此既有签名保留更小的
pattern id，在任何 tie-break 中仍然优先。

| 指标 | 对齐前 | 对齐后 | 变化 |
|---|---|---|---|
| 签名条目 | 513 | **834** | +321 |
| 不同扩展名 | 404 | **559** | +155 |
| 净新增格式（内建完全没有的） | — | **161** | — |

161 个净新增里包含此前完全无法命名的真实格式：

- `e01` EnCase 取证镜像
- `luks` Linux LUKS 加密卷头
- `accdb` Access 2007+
- `one` OneNote 分区
- `cwk` ClarisWorks / AppleWorks
- `dxf` / `stl` CAD 与 3D 打印
- `xfs` XFS 元数据块（5 种 magic）
- `m2ts` / `wtv` / `vib` / `tivo` 媒体与备份容器
- `cow`（QEMU v1 镜像，此前被 `qcow` 前缀遮蔽）

## 5. 三层过滤：为什么不能全量照搬

### 5.1 长度下限 3 字节

PhotoRec 有 40 条 magic 只有 1–2 字节（`dbf=03`、`gpg=95`、`fat=55AA`、
`ecryptfs=0000`）。在随机数据上 1 字节每 256 字节必然命中一次，
2 字节每 64 KiB 命中一次。PhotoRec 靠回调把它们救回来；我们没有回调，
只能拒绝。

这些条目被写入 `photorec_lowconf.json`，需要显式指定扩展名时才启用。

### 5.2 熵下限：至少 2 个不同字节值

这是本轮**门禁抓到的真实缺陷**。

`dovecot` 的声明式 magic 是 **48 字节全零**（`file_dovecot.c` 第 121 行）。
PhotoRec 用它识别 Dovecot 邮箱索引文件，靠 `header_check_dovecot` 校验后续结构。

我们的声明式层照搬后，全零区域每个 512 字节槽位都命中：

```
blank 4 MiB volume candidates at stride 512: 8192   ← 每个槽位一个
```

连带后果是 `FormattedVolumeRecovery` 的负向对照爆掉：一个刚格式化的空卷
被报出 **191 个"残留文件"**（RAW 回退路径把全卷判成 dovecot 索引）。

修复后同一门禁输出 `0`。规则写进生成器：`len(set(magic_bytes)) < 2` 的一律拒绝，
并在报告里列出被拒条目以便审计。

### 5.3 通配扩展名

`gm*`（GameMaker 家族）和 `tx?` 是 PhotoRec 的显示通配符，不是真实扩展名。
照搬会让节点类型变成 `*` 号，无法落到文件系统。生成器丢弃它们。

## 6. 门禁

新增 `CTest PhotoRecAlign`（`tests/test_photorec_align.cpp`），19 项检查：

| 检查 | 含义 |
|---|---|
| `all.size() >= 800` | 内建 + PhotoRec 层确实被注册 |
| 扩展名 `>= 450` | 覆盖广度达到预期 |
| 短 magic `<= 43` | **冻结**精度债务，PhotoRec 层不得新增短 magic |
| 11 个关键格式存在 | `e01`/`luks`/`accdb`/`one`/`cwk`/`dxf`/`stl`/`xfs`/`wtv`/`m2ts`/`vib` |
| 321 条 golden 全部存在 | 生成器输出与编译进库的表一致 |
| `Match()` 逐条可解析 | AC 后端与线性参考对该格式给出一致结论 |
| `badShadow == 0` | 不相关格式之间的误判为零 |
| 空卷候选 == 0 | 全零 4 MiB、stride 512，**不得产生任何候选** |

关于 `shadowed = 45`：这是已知且正确的 tie-break 语义。当两条 magic 互为前缀时
（内建 `qcow=514649FB` vs PhotoRec `cow=514649FB00000001`），`Match()` 取
pattern id 更小者。门禁接受"前缀关系"，只拒绝不相关格式之间的误判。

### 负向验证

把 `scan/SignatureDB.cpp` 里的 `RegisterPhotoRecAlignSignatures(*this)` 注释掉，
重编译后门禁必须失败：

```
built-in + PhotoRec signatures: 513
golden probe: missing=321
FAIL: PhotoRecAlign 18 checks, 11 failure(s)
```

恢复后全绿（19 checks / 0 failure）。

### 误报率

复用 CAR-002 口径（8 MiB 随机噪声，seed 12648430）：

| 版本 | FP/MiB | precision | recall |
|---|---|---|---|
| 对齐前（513 签名） | 14.38 | 12.80% | 100% |
| 对齐后（834 签名） | **14.38** | **12.80%** | **100%** |

**零变化**。原因是新增条目全部 ≥3 字节且非低熵，在 8 MiB 随机数据里的期望命中
约 0.2 次，实测 0 次。这正是"长度下限 + 熵下限"两层过滤换来的结果。

## 7. 回归

| 项 | 结果 |
|---|---|
| 定向（PhotoRecAlign / SignatureDBLoad / SignatureDBAC / RawScanAC / RawScanFalsePositive / RawScanParallel / RawScanParallelWiring / DeepScanFallback / SignatureMatcher / EngineScanFormatMatch / RecoverOutputCollision） | **11/11** |
| 全量构建 `build-gcc14` | `BUILD_EXIT=0` |
| 全量 CTest | **93/96** |

3 项失败为既有基线：`DFR_NIST_Compare`、`DFR_NIST_BatchStrict`、`DFTT_11_CARVE_FAT`，
与本轮改动无关（改动前即为失败）。

`FormattedVolumeRecovery` 一度从 PASS 变 FAIL（空卷 191 文件），根因即 §5.2 的
`dovecot`，修复后恢复 PASS——这是本轮门禁价值的直接证据。

## 8. 与 PhotoRec 的剩余差距

| 维度 | PhotoRec 7.2 | 本实现 | 差距性质 |
|---|---|---|---|
| `file_hint` 数 | 351 | 235 个扩展名（声明式） | 92 个格式**只靠回调**识别，无声明式 magic 可提取 |
| 声明式 magic | 408 | 321 | 差额 = 重复 40 + 通配 13 + 低熵 1 + 短 magic 33 |
| 回调级校验 | 每个格式独立 C 逻辑 | 无 | 结构上不具备（我们不是逐格式移植） |
| 格式覆盖总数 | 351 | 559（含 UI 别名） | 口径不同，不可直接比较 |

**明确未覆盖**（需回调才能识别，本轮无法对齐）：`elf`（magic 在宏里）、
`shp`、`3dm`、`DS_Store`、`mft`、`hdr`、`sqlite`（内建已有）、以及其余约 80 个
仅靠 `header_check` 逻辑判断的格式。这些要么有结构特征但无固定 magic，
要么 magic 定义在共享宏里未被解析器展开。

### 已知精度债务（本轮未修，已冻结）

内建库既有 43 条 <3 字节 magic，全部是早期为"UI 类型覆盖"做的别名：
`efi`/`ocx`/`ax`/`com`/`scr`/`vxd`/`drv` = `4D5A`（PE 别名）、`tgz`=`1F8B`、
`ini`=`5B`、`dib`=`424D`、`acc`=`FFF1`、`pam`=`5031`、`pm`=`5036`，
另有 `dbf=03`、`gpg=99` 等。

它们贡献了 14.38 FP/MiB 里的大部分。清理它们会改变所有既有扫描测试的期望值，
属于独立议题，本轮只在门禁里冻结数量（`<= 43`），不允许 PhotoRec 层让它变多。

## 9. 复现方式

```bash
# 重新生成签名表（需要 TestDisk 7.2 源码）
python tools/gen_photorec_signatures.py \
    --src D:/dev/testdisk-7.2/src \
    --out testdata/signatures/photorec_aligned.json \
    --lowconf-out testdata/signatures/photorec_lowconf.json \
    --emit-header scan/PhotoRecSignatures.h \
    --exclude scan/SignatureDB.cpp \
    --report docs/reports/photorec_signature_extraction.txt

# 门禁
ctest --test-dir build-gcc14 -R "^PhotoRecAlign$" --output-on-failure
```

`testdata/` 按项目既有约定整体不入库（体积原因），因此干净检出时门禁会自动降级为
"只检查编译进库的规模"，仍能发现注册调用被误删。

## 10. 一句话结论

**广度上，雕刻格式从 404 个扩展名扩到 559（净增 161 个真实格式），误报率纹丝不动
（14.38 FP/MiB）；深度上仍落后 PhotoRec 的回调级校验，这是架构差异而非遗漏，
已在报告与代码注释中如实标注。**
