# PhoenixRecover vs DiskGenius 6.2 vs TestDisk 7.2：广度与深度对比

> **日期**：2026-11-21
> **基线**：DiskGenius `6.2.0.1829`（官方能力页 + 6.2 更新说明）、TestDisk/PhotoRec `7.2`（官方 wiki）
> **本方口径**：只写有 CTest / 金标 / 报告支撑的能力；引擎已通但未接 UI 的单独标注，不并入"已支持"。
> **标注**：✅ 已实现并有门禁　🟡 部分实现 / 有条件　❌ 未实现　⛔ 明确冻结　❓ 竞品未公开宣称（闭源，无法验证）

---

## 1. 总览

| 维度 | PhoenixRecover | DiskGenius 6.2 | TestDisk 7.2 |
|---|---|---|---|
| 产品定位 | 恢复引擎 + 取证级自证（引擎优先，UI 滞后） | 一体化工具箱（恢复/分区/备份/磁盘工具） | 应急扳手（分区表与启动修复为主） |
| 形态 | Windows Qt GUI + headless CLI | Windows GUI（含 WinPE 启动盘） | 全平台 CLI/TUI（DOS/Win/Linux/macOS/BSD） |
| 授权 | 自研 | 免费版 + Pro（$69.9 起） | GPL v2+ 免费 |
| 可解析文件系统 | **6**：NTFS / FAT12-32 / exFAT / ext2-4 / ReFS | **7**：NTFS / FAT12-32 / exFAT / ext2-4 / ReFS（+BitLocker、存储池、LVM2） | **25+**：上述 + APFS / HFS+ / btrfs / XFS / JFS / ReiserFS / UFS / ZFS / NSS 等（多数仅分区发现） |
| 可写文件系统 | ❌（A3 未做，写路径被 `PHOENIX_ENABLE_DEVICE_WRITES=0` 硬关） | ✅ ext2-4 / FAT / exFAT / NTFS / ReFS | ❌（只导出文件，不改源盘） |
| 删除文件名恢复 | ✅ 5 类 FS | ✅（商业最强项，覆盖最广） | 🟡 仅 FAT / exFAT / NTFS / **ext2** |
| 事务日志回放 | ✅ ext4（JBD2 + JBD v1 + 外部日志） | ❓ | ❌ |
| RAW 签名雕刻 | ✅ 513 签名 + AC 自动机 + 多线程 | ✅ 按类型恢复（签名数未公开） | ✅ PhotoRec >440 格式（**最广**） |
| 逐字节自证 | ✅ **独有**（引擎 verify） | ❌ | ❌ |
| 误报率量化 | ✅ **独有** | ❌ | ❌ |
| 分区结构写回 | 🟡 只读探测 + 候选恢复 | ✅ MBR↔GPT、CRUD、无损调整 | ✅ 分区表写回、boot 重建、MFT 镜像修复 |

---

## 2. 广度 A：文件系统矩阵

图例：解析 = 只读挂载/浏览；删除 = 删除文件名级恢复；写 = 写入/格式化。

| 文件系统 | PhoenixRecover | DiskGenius 6.2 | TestDisk 7.2 |
|---|---|---|---|
| **NTFS** | 解析 ✅（MFT / ADS / `$ATTRIBUTE_LIST` 跨记录 / 压缩 / 稀疏）<br>删除 ✅（MFT 记录 + 逐字节验证）<br>写 ❌ | 解析 ✅ 删除 ✅ 写 ✅ | 解析 ✅ 删除 ✅（ADS 弱）<br>写 ❌ |
| **FAT12/16/32** | 解析 ✅ 删除 ✅（目录项）<br>写 ❌ | 解析 ✅ 删除 ✅ 写 ✅ | 解析 ✅ 删除 ✅<br>boot/FAT 表重建 ✅ |
| **exFAT** | 解析 ✅（含 NoFatChain 修复）<br>删除 ✅ 写 ❌ | 解析 ✅ 删除 ✅ 写 ✅ | 解析 ✅ 删除 ✅ 写 ❌ |
| **ext2/3/4** | 解析 ✅（extent / uninit / bigalloc / inline_data / 稀疏 / 间接块 / JBD2 回放）<br>删除 ✅ 写 ❌ | 解析 ✅ 删除 ✅<br>写 ✅（含格式化，6.2 增强） | 解析 ✅（可导出文件）<br>删除 🟡 **仅 ext2**<br>备份超块定位 ✅ |
| **ReFS** | 解析 ✅（v3.1/v3.14 / 4Kn / 64K 簇 / CRC64+SHA256）<br>删除 ✅（不可达残留目录页，2026-11-21 新增）<br>写 ❌ | 解析 ✅ 删除 ✅（6.2 "增强 ReFS 支持"，深度未公开） | ❌ |
| **APFS** | ⛔ | ❌（官方能力表未列） | 分区发现 ✅ 文件浏览 🟡 |
| **HFS/HFS+/HFSX** | ⛔ | ❌ | 分区发现 ✅ 文件浏览 🟡 |
| **XFS / btrfs / JFS / ReiserFS / UFS / ZFS / NSS** | ⛔ | ❌ | 分区发现 ✅（不保证文件级） |
| **LUKS 加密卷** | ❌ | ❌（仅 BitLocker） | 识别 ✅ 解锁 ✅ |
| **BitLocker** | 🟡 解密引擎 ✅（AES-XTS/CCM 纯 C++ + 金标）<br>**未接 SectorReader / UI** | ✅ 解锁 + 导出（Pro 不限大小） | ❌ |
| **LVM2 / 动态磁盘 / 存储池** | ❌（LDM 动态盘无解析器；208 GiB LDM 盘靠裸 NTFS 探测绕过） | ✅ LVM2 读写、动态盘→基本盘（Pro）、存储池恢复 | 🟡 LVM/LVM2 分区发现 |

---

## 3. 广度 B：磁盘 / 容器 / 工具域

| 功能域 | PhoenixRecover | DiskGenius 6.2 | TestDisk 7.2 |
|---|---|---|---|
| 丢失分区发现 | ✅ MBR/GPT 只读探测 + 候选评分 | ✅ | ✅（最全：Intel/EFI GPT/Mac/Sun/BSD/Xbox/VMware…） |
| 分区表写回 | ❌（A3/A4 未做） | ✅ | ✅ 核心能力 |
| 分区 CRUD / 无损调整 / MBR↔GPT | ❌ | ✅ | ❌ |
| 虚拟磁盘只读 | ✅ VHD fixed/dynamic、VHDX、VMDK sparse、QCOW2 v2/v3、VDI（10 个真实镜像金标） | ✅ VMDK/VDI/VHD/HDD（Pro 可写） | ❌ |
| RAID 只读重组 | 🟡 引擎 ✅ JBOD/0/1/4/5/6/10 + 6 种 parity 布局 + 缺盘重建；**未接扫描主路径/UI** | ✅ 虚拟重组 0/5/6 后可恢复 | 🟡 仅识别 md 1/4/5/6（不重组） |
| 磁盘/分区镜像 | 🟡 raw `.img`、断点续做、坏扇区统计；无专有格式、无还原 | ✅ PMFX 增量备份 + 单分区还原 | ❌ |
| 磁盘克隆 / 系统迁移 | ❌（A2 未做） | ✅ | ❌ |
| 坏道 | 🟡 只读跳过 + `BadSectorMap`；不修复 | ✅ 检测 + 修复 + 多盘并行 | ❌ |
| 扇区编辑器 | ❌ | ✅（十六进制、条件搜索、填充） | 🟡 只读 dump |
| SMART / 测速 | 🟡 `SmartInfo` 读取；无测速 | ✅ 检测 + 基准测速 | ❌ |
| 数据擦除 | ❌ | ✅ DoD 5220.22-M / Gutmann / BMB21-2019 | ❌ |
| WinPE / 引导修复 | ❌（A5 未做） | ✅ 启动盘 + Windows 引导修复 | 🟡 手工修 boot 扇区 |
| 文件修复（内容级） | ✅ JPEG / PNG / GIF / MP4 / Office-ZIP / FFmpeg 重封装（含修复后自检） | 🟡 以"恢复"为主，内容修复有限 | ❌ |
| 报表导出 | 🟡 CSV 明细 | ✅ CSV + Excel（>100 万行已修） | ❌ |
| 跨平台 | 🟡 Windows 为主，Linux 构建路径有文档 | ❌ Windows only | ✅ 全平台 |

---

## 4. 深度 A：恢复算法关键差异

| 深度能力 | PhoenixRecover | DiskGenius | TestDisk |
|---|---|---|---|
| ext4 日志语义 | ✅ JBD2 全事务回放；tag checksum **v2/v3**、commit `h_chksum`、revoke 两阶段、**JBD v1 兼容**、**外部日志设备（UUID 校验）**、**FLEX_BG 回放后 GDT 重载**、**torn-commit 只回放已提交事务** | ❓ 无公开说明 | ❌ 完全不解析日志 |
| NTFS 元数据深度 | ✅ `$ATTRIBUTE_LIST` 跨扩展记录合并（真实盘 56.7 MB 文件修复，SHA-256 与源盘一致）、ADS、稀疏/压缩 runlist | ✅ 成熟 | 🟡 靠 MFT 镜像修 boot，不做属性列表 |
| ReFS 删除恢复 | ✅ 扫描**不可达但结构合法**的 MSB+ 目录页（三重准入：结构合法 + 自 LCN 自洽 + 不可达），live/deleted 严格分离 | ✅ 商业实现，机制未公开 | ❌ |
| 稀疏 / 空洞语义 | ✅ `start==0` 展开零块、稀疏洞补零（ext4/NTFS） | ✅ | ❌ |
| 碎片拼接 | ✅ FAT/exFAT 簇链 + NoFatChain 语义、NTFS runlist、ext4 extent | ✅ | ❌ |
| 雕刻签名数 | 513（自定义 JSON 可扩展，QC 无 Qt 依赖） | 未公开 | **>440**（PhotoRec） |
| 雕刻并行度 | ✅ 8 线程 **4.94×**（943 MB/s，合成 256 MiB） | 多线程（未公开数据） | ❌ 单线程 |
| 误报控制 | ✅ 量化：14.38 FP/MiB（合成随机噪声），precision 12.8% / recall 100% | ❌ 无公开数据 | ❌ 无 |
| 恢复正确性自证 | ✅ **逐字节引擎 verify**：真实 208 GiB 盘 **872,559 文件 / 114.07 GiB / 100% match**，0 mismatch | ❌ 人工抽查 | ❌ |
| 预览 | ✅ 图片/视频（GUI） | ✅ 文档/图片/视频/PDF | ❌ |
| 只读不变量 | ✅ 扫描源全程禁写 + `WriteGuard` 默认拒绝 + 事务回滚契约 | ✅ 官方声明不写源盘 | ✅ 不写源盘 |

---

## 5. 深度 B：三家各自的最强项

| 谁 | 最强项（短期难被追平） |
|---|---|
| **DiskGenius** | 广度与工程完整度：分区 CRUD / 无损调整 / MBR↔GPT / 系统迁移 / PMFX 增量备份 / 坏道修复 / 扇区编辑 / WinPE，且**可写 ext4/ReFS**——这是"管理"而非"恢复"的护城河 |
| **TestDisk** | 分区结构考古：25+ 文件系统的分区发现、boot 扇区重建、MFT 镜像修复、LUKS/LVM/RAID 识别；PhotoRec 440+ 格式，跨平台零安装 |
| **PhoenixRecover** | 引擎可证伪性：日志级语义（JBD2 csum v2/v3 / torn-commit / 外部日志）、ReFS 残留页删除恢复、逐字节自证 100%、误报率量化、AC 并行雕刻——三家唯一能"给出证据链"的 |

## 6. 结论与差距清单

**结论**：广度上我们排第三（工具域缺失明显：分区写回、坏道修复、克隆/备份、动态盘/LVM、APFS/XFS）；深度上在 **ext4 日志、ReFS 删除恢复、验证与误报量化** 三项领先，NTFS 与雕刻基本持平（雕刻格式数落后 PhotoRec）。

| 优先级 | 差距 | 影响 | 现状 |
|---|---|---|---|
| **P0** | ext4 / ReFS 引擎未接 UI | 能力存在但用户点不到，等于对外不可见 | 引擎 ✅ 门禁 ✅，UI ❌ |
| **P0** | BitLocker / RAID 未接 `SectorReader` 主路径 | 只能跑测试，不能用真实盘 | 引擎 ✅，接线 ❌ |
| P1 | 分区表写回 + boot 修复 | 对 DG/TestDisk 的"救砖"场景完全缺席 | A3/A4 未启动 |
| P1 | 坏道检测/修复、镜像还原、克隆 | 一体化工具的门面能力 | A2/A4 未启动 |
| P2 | 动态盘 / LVM2 / 存储池 | 服务器场景 | 未规划 |
| P2 | 雕刻格式数（513 vs >440 类） | 已够用，但视频/相机 RAW 子型可补 | 可增量扩签名库 |

> **不得夸大**：ReFS / ext4 均为**合成金标**验证（ReFS 真实卷样本仍缺）；RAID 真实阵列、BitLocker 真实卷、QCOW2/VDI 真实镜像仍是已知缺口（见 `ROADMAP_RECOVERY.md` A1-1d / A1-3b / A1-4b-b / A1-5b）。
