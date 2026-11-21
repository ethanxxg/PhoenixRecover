> 本文档原位于 `testdata/测试镜像说明.md`，为便于克隆后直接阅读而收录于此；
> 文中出现的相对路径（如 `digital-corpora-disk-images/download-all.ps1`）均相对 `testdata/` 目录。

# 数字取证测试镜像说明文档

本文档说明已下载的所有测试镜像的来源、用途和使用方法。

---

## 目录结构

```
D:\dev\PhoenixRecover\testdata\          (当前基准路径)
├── cfreds/                               # CFReDS DFR 已删除文件恢复测试镜像 ✅
├── custom-carving-images/                # 20 个 dd 镜像，1100 个可雕刻文件 ✅
├── dftt/                                 # DFTT 解压后的测试镜像 ✅
├── digital-corpora-disk-images/          # Digital Corpora 磁盘镜像 ✅
├── file-carving-cftt-images/             # NIST CFTT 文件雕刻镜像 ✅
│   └── images/                           #   图形 dd 镜像 + 视频源文件
├── file-carving-images/                  # NIST CFReDS 文件雕刻测试镜像 ✅
├── filetypes1/                           # FILETYPES1 — 32 种文件类型样本 ✅
├── govdocs1/                             # Govdocs1 按类型子集 ✅
│   ├── docx/                             #   163 个 DOCX
│   └── rtf/                              #   1125 个 RTF
├── napierone-raw-files/                  # NapierOne 第 1 批 (9 种类型 × 100)
├── napierone-raw-files-2/                # NapierOne 第 2 批 (6 种类型 × 100)
├── sleuthkit_test_data/                  # Sleuth Kit 官方测试数据 ✅
├── testdisk-test-data/                   # TestDisk/PhotoRec 测试数据 ✅
│   ├── dftt/                             #   DFTT #6 #7 #8 #11 #12 ZIP 包
│   └── dfrws-2006/                       #   DFRWS 2006 + 2007 挑战
└── 2009.DFRWS.Corpora.pdf / .zh.md       # DFRWS 2009 论文及翻译

（以下外部数据源未下载，见第五章）：
    cfreds-hacking-case/                  # CFReDS 黑客入侵场景
    cfreds-data-leakage-case/             # CFReDS 数据泄露场景
    dfrws-challenges/                     # DFRWS 历年挑战赛
    forensics-samples/                    # 多文件系统 Linux 镜像
    apfs-specimens/                       # macOS APFS 测试镜像
```

---

## 支持雕刻的文件类型总览

| 扩展名 | 格式 | 来源（镜像集） | 文件数 |
|--------|------|---------------|--------|
| **图片 / 图像** |
| `JPG` / `JPEG` | JPEG 图片 | CFReDS Graphic, DFTT #8/#11, DFRWS 2006/2007, NapierOne, 自定义镜像 | ~140 |
| `PNG` | PNG 图片 | CFReDS Graphic, 自定义镜像 | ~7 |
| `BMP` | BMP 位图 | CFReDS Graphic, NapierOne, 自定义镜像 | ~107 |
| `GIF` | GIF 动图 | CFReDS Graphic, DFTT #11, NapierOne, 自定义镜像 | ~107 |
| `TIF` / `TIFF` | TIFF 图片 | CFReDS Graphic, 自定义镜像 | ~7 |
| `PCX` | PCX 图片 | CFReDS Graphic | ~1 |
| `SVG` | SVG 矢量图 | NapierOne, 自定义镜像 | ~100 |
| **文档 / 办公** |
| `DOC` | Word 文档 | CFReDS Documents, DFTT #11, NapierOne, 自定义镜像 | ~107 |
| `DOCX` | Word 文档 (新) | CFReDS Documents, Govdocs1, 自定义镜像 | ~170 |
| `XLS` | Excel 表格 | CFReDS Documents, DFTT #11, NapierOne, 自定义镜像 | ~107 |
| `XLSX` | Excel 表格 (新) | CFReDS Documents, Govdocs1 docx | ~4 |
| `PPT` | PowerPoint 演示 | CFReDS Documents, DFTT #11, 自定义镜像 | ~7 |
| `PPTX` | PowerPoint (新) | CFReDS Documents | ~4 |
| `PDF` | PDF 文档 | CFReDS Documents, DFTT #11, DFRWS 2006/2007, 自定义镜像 | ~12 |
| `RTF` | RTF 格式文档 | Govdocs1, 自定义镜像 | ~200 |
| `TXT` | 纯文本 | NapierOne, DFRWS 2006/2007, 自定义镜像 | ~110 |
| `HTML` / `HTM` | 网页文件 | DFRWS 2006/2007, NapierOne, 自定义镜像 | ~110 |
| `CSS` | 样式表 | NapierOne, 自定义镜像 | ~100 |
| **电子书 / 日历** |
| `EPUB` | 电子书 | NapierOne, 自定义镜像 | ~100 |
| `ICS` | 日历数据 | NapierOne, 自定义镜像 | ~100 |
| **数据 / 代码** |
| `CSV` | 逗号分隔值 | NapierOne, 自定义镜像 | ~100 |
| `JSON` | JSON 数据 | NapierOne, 自定义镜像 | ~100 |
| `XML` | XML 标记 | NapierOne, 自定义镜像 | ~100 |
| `JS` | JavaScript | NapierOne, 自定义镜像 | ~100 |
| `C` / `CC` / `H` | C/C++ 源码 | DFRWS 2007 | ~16 |
| `ZIP` | ZIP 压缩包 | CFReDS Archive, DFTT #11, DFRWS 2006/2007, 自定义镜像 | ~15 |
| `7Z` | 7-Zip 压缩 | CFReDS Archive | ~1 |
| `RAR` | WinRAR 压缩 | CFReDS Archive, 自定义镜像 | ~7 |
| `GZ` / `GZIP` | Gzip 压缩 | CFReDS Archive | ~1 |
| `BZ2` | Bzip2 压缩 | CFReDS Archive | ~1 |
| `TAR` | Tape Archive | CFReDS Archive | ~1 |
| `WIM` | WIM 映像 | CFReDS Archive | ~1 |
| **音频** |
| `MP3` | MP3 音频 | CFReDS Audio, 自定义镜像 | ~6 |
| `WAV` | WAV 音频 | CFReDS Audio, DFTT #11, 自定义镜像 | ~7 |
| `AU` | Sun 音频 | CFReDS Audio | ~1 |
| `WMA` | WMA 音频 | CFReDS Audio, FILETYPES1 | ~1 |
| `M4A` | MPEG-4 音频 | FILETYPES1 | ~1 |
| **视频** |
| `MP4` | MP4 视频 | CFReDS Video, 自定义镜像 | ~7 |
| `AVI` | AVI 视频 | CFReDS Video, 自定义镜像 | ~7 |
| `MOV` | MOV 视频 | CFReDS Video, DFTT #11, 自定义镜像 | ~8 |
| `FLV` | Flash 视频 | CFReDS Video, FILETYPES1 | ~1 |
| `MPG` / `MPEG` | MPEG 视频 | CFReDS Video | ~1 |
| `WMV` | WMV 视频 | CFReDS Video, DFTT #11 | ~2 |
| **二进制 / 可执行** |
| `EXE` / `DLL` | PE 可执行文件 | FILETYPES1 | ~2 |
| `ELF` | Linux ELF 可执行 | FILETYPES1 | ~1 |
| `JAR` | Java 归档 | FILETYPES1 | ~1 |
| `RPM` | RPM 软件包 | FILETYPES1 | ~1 |
| **邮件 / 数据** |
| `PST` | Outlook 个人文件夹 | FILETYPES1 | ~1 |
| `MBOX` (Tbird) | Thunderbird 邮件 | FILETYPES1 | ~1 |
| `ASP` | ASP 网页脚本 | FILETYPES1 | ~1 |
| **其他** |
| `JB2` | JBIG2 压缩图像 | FILETYPES1 | ~1 |
| `URL` | URL 快捷方式 | FILETYPES1 | ~1 |
| **自定义测试** |
| (N/A) | 错误扩展名 JPEG | DFTT #8 | 2 |
| (N/A) | 嵌入 ZIP/WORD JPEG | DFTT #8 | 4 |
| (N/A) | ADS 隐藏 JPEG | DFTT #8 | 1 |
| (N/A) | 随机前缀拼接 JPEG | DFTT #8 | 1 |
| (N/A) | 碎片 JPEG/ZIP/HTML/Office | DFRWS 2007 挑战 | ~10 |

**合计约 ~2,200+ 个可雕刻文件**，覆盖 **40+ 种文件格式**，分布在:
- CFReDS NIST 文件雕刻镜像（30 张）
- DFTT #7 / #8 / #11 / #12（4 张）
- DFRWS 2006 / 2007 challenge（2 张）
- **自定义 carving-00~19.dd（20 张，1100 个文件）**
- FILETYPES1 样本文件（32 种类型，作为雕刻素材库补充）

---

## 一、DFR 已删除文件恢复测试镜像 ✅ 已下载

**本地路径**: `cfreds/`  
**来源**: https://cfreds-archive.nist.gov/dfr-test-images.html  
**用途**: 测试基于元数据的已删除文件恢复工具（非文件雕刻）  
**格式**: dd 镜像 (bzip2 压缩，.dd.bz2)，解压后用 `bunzip2 *.dd.bz2`  
**总大小**: 159.52 MB（24 个测试集 × 3 文件系统 FAT/NTFS/exFAT，90 个文件）

| 目录 | 文件数 | 用途 |
|------|--------|------|
| `dfr-01/` | 5 | 恢复一个非碎片文件 |
| `dfr-01-recycle/` | 3 | 回收站清空后恢复一个文件 |
| `dfr-02/` | 4 | 恢复两个碎片的文件 |
| `dfr-03/` | 4 | 恢复多碎片文件 |
| `dfr-04/` | 5 | 恢复多个非碎片文件（非ASCII文件名） |
| `dfr-05/` | 4 | 恢复多个碎片文件 (B1B2E1E2) |
| `dfr-05-braid/` | 4 | 恢复多个碎片文件 (B1C1B2C2 编织) |
| `dfr-05-nest/` | 4 | 恢复多个碎片文件 (B1D1D2B2 嵌套) |
| `dfr-06/` | 4 | 恢复一个大文件 |
| `dfr-07/` | 4 | 恢复一个被覆盖的文件 |
| `dfr-07-one/` | 4 | 恢复一个被覆盖的文件 |
| `dfr-07-two/` | 4 | 恢复一个被覆盖的文件 |
| `dfr-08/` | 4 | 恢复多个被覆盖的文件 |
| `dfr-09/` | 4 | 恢复大量文件（无覆盖） |
| `dfr-10/` | 4 | 恢复大量文件（部分被覆盖） |
| `dfr-11/` | 4 | 恢复一个非碎片目录 |
| `dfr-11-mft/` | 1 | NTFS MFT 中恢复非碎片目录 |
| `dfr-11-compressed/` | 1 | NTFS 压缩中恢复非碎片目录 |
| `dfr-12/` | 4 | 恢复一个碎片目录 |
| `dfr-13/` | 4 | 恢复随机文件系统活动 |
| `dfr-14/` | 3 | 恢复其他文件系统对象 |
| `dfr-15/` | 4 | 列出每种文件系统对象各一个 |
| `dfr-16/` | 4 | 列出大量文件 |
| `dfr-17/` | 4 | 列出深层文件路径 |

---

## 二、Digital Corpora 磁盘镜像

**来源**: https://digitalcorpora.org/corpora/disk-images/  
**格式**: E01 (EnCase)、AFF、raw 等

### 2.1 ✅ 已恢复（手动重新下载）

以下 8 个数据集（~19 GB）已手动重新下载完毕:

| 目录 | 大小 | 说明 |
|------|------|------|
| `nps-2009-canon2/` | 202 MB | Canon 数码相机镜像 |
| `nps-2009-casper-rw/` | 295 MB | Ubuntu USB Token ext3 |
| `nps-2009-hfsjtest1/` | 20 MB | 日志式 HFS 测试 |
| `nps-2009-ntfs1/` | 55 MB | NTFS 碎片测试 |
| `nps-2009-ubnist1/` | 10.2 GB | FAT32 多版本镜像 |
| `nps-2009-domexusers/` | 8.3 GB | Windows XP SP3 场景 |
| `nps-2010-emails/` | 1 MB | 电子邮件编码测试 |
| `circl-2023-wiped/` | 52 MB | 擦除磁盘恢复练习 |

运行 `.\digital-corpora-disk-images\download-all.ps1` 重新下载。

> **2026-11-15 空间整理**：已删除 `circl-2023-wiped/wiped_disk.dd`（262.3 GiB）。
> 该文件是 54 MB `wiped_disk.E01` 的**全零展开**（擦除盘），`test_digital_corpora_batch`
> 对 >50 GiB 镜像只做 SMOKE，从不扫描，无测试价值。**不要再用 `Expand-EwfToRaw.ps1`
> 展开它**——会白写 262 GiB。E01 原件保留，可随时重新展开。
> 同批删除 `nps-2009-domexusers.xml.old`（与 `.xml` 字节完全相同的重复件）。
> 清理脚本：`tools/testdata_cleanup.py`（默认 dry-run，`--apply` 执行）；
> 日志：`docs/reports/testdata_cleanup_2026-11-15.json`。
> 注意本机删除默认走**回收站**，不手动清空回收站不会真正释放空间。

---

## 三、File Carving 文件雕刻测试镜像 ✅ 已下载

**来源**: https://cfreds-archive.nist.gov/FileCarving/index.html  
**用途**: 测试基于文件签名的文件雕刻工具  
**格式**: dd 镜像 (bzip2 压缩)  
**本地路径**: `file-carving-images/`  
**总大小**: 491 MB（61 个文件，已下载完成）

### 3.1 测试镜像

`Images/` 目录，共 30 个 `.dd.bz2` 文件：

| 目录 | 文件数 | 碎片级别 | 文件类型 |
|------|--------|----------|----------|
| `Graphic/` | 6 | L0-L5 | JPG, PNG, BMP, GIF, TIF, PCX |
| `Documents/` | 6 | L0-L5 | DOC, XLS, PPT, PDF |
| `Archive/` | 6 | L0-L5 | 7Z, BZ2, GZ, TAR, WIM, RAR, ZIP |
| `Audio/` | 6 | L0-L5 | MP3, WAV, AU, WMA |
| `Video/` | 6 | L0-L5 | MP4, AVI, MOV, FLV, MPG, WMV |

### 3.2 原始测试文件（用于对比验证）

`OriginalFiles/` 目录，共 31 个文件：

| 子目录 | 文件数 |
|--------|--------|
| `Archive/` | 7 |
| `Audio/` | 4 |
| `Document/` | 7 |
| `Graphic/` | 7 |
| `Video/` | 6 |

---

## 四、其他数据

### 4.1 Sleuth Kit 官方测试数据 ✅ 已下载

**本地路径**: `sleuthkit_test_data/`  
**来源**: https://github.com/sleuthkit/sleuthkit_test_data  
**用途**: Sleuth Kit 回归测试数据，含多文件系统格式  
**内容**: apfs, btrfs, exfat, fuzzing, gpt, nps-2009-hfsjtest1, ufs, xfs 等  
**状态**（2026-06-05）: 12 个 `.E01` 源文件完整；5 个 zip 直出 `.dd` + `6-fat-undel.dd` 已可读；其余 E01→`.dd` 运行 `Expand-EwfToRaw.ps1 -Roots sleuthkit_test_data`

### 4.2 DFTT 解压镜像 ✅ 已下载

**本地路径**: `dftt/`  
**内容**: 已解压的 DFTT 测试镜像目录（6-undel-fat, 7-undel-ntfs, 8-jpeg-search, 10-ntfs-autodetect, 11-carve-fat, 14-iso9660-1）  
原始 ZIP 包位于 `testdisk-test-data/dftt/`

### 4.3 CFTT 文件雕刻镜像 ✅ 图形 dd 已解压

| 已完成 | 未下载 |
|--------|--------|
| 8 个 graphic `.dd`（`images/images/`）+ 归档 `graphic-images.tbz2`（1.5 GB）+ 源文件包 | 视频 dd 镜像 (`images.tar.bz2`, 11.5 GB) 跳过 |

---

## 使用方法

### 解压 dd 镜像
```bash
bunzip2 *.dd.bz2
```

### 分析镜像
```bash
mmls image.dd
fls -o <offset> image.dd
```

---

## 五、数据源概览（含已下载与推荐参考）

以下数据源覆盖数据恢复测试的各类场景（Win/Linux/Mac）：

### 5.1 CFReDS Hacking Case（Windows 黑客场景）

| 项目 | 说明 |
|------|------|
| **来源** | https://cfreds-archive.nist.gov/Hacking_Case.html |
| **格式** | E01 (EnCase) |
| **大小** | ~2 GB |
| **用途** | 分析黑客工具、注册表、用户活动，Windows XP NTFS 场景 |
| **内容** | Dell CPi 笔记本镜像（NTFS），含 hacking tools、WiFi 嗅探等 |
| **平台** | Windows |

### 5.2 CFReDS Data Leakage Case（数据泄露综合场景）

| 项目 | 说明 |
|------|------|
| **来源** | https://cfreds-archive.nist.gov/data_leakage_case/data-leakage-case.html |
| **格式** | DD + E01 + 7z |
| **大小** | PC ~20 GB、RM#1 ~3.7 GB、RM#2 ~3.7 GB、RM#3 ~102 MB |
| **用途** | 复杂场景：PC、USB、CD 等多设备数据泄露调查 |
| **平台** | Windows |

### 5.3 DFRWS 历年挑战赛数据

| 项目 | 说明 |
|------|------|
| **来源** | https://github.com/dfrws (各年份 repo) |
| **文件雕刻** | 2006: 50MB raw (JPEG/ZIP/HTML/Text/Office)、2007: 更复杂碎片场景 |
| **Linux 内存** | 2008-2009: Linux 内存分析、PS3 Linux 文件系统镜像 |
| **Android** | 2010-2011: Android 手机 NAND/YAFFS2 镜像 |
| **IoT** | 2017-2021: IoT 设备（Raspberry Pi、智能家居、NAS 等） |
| **ICS** | 2023: 工业控制系统 PLC 取证 |
| **平台** | 跨平台（Win/Linux/Mac/Android/IoT） |

### 5.4 forensics-samples（多文件系统 Linux 镜像）✅ 已下载

| 项目 | 说明 |
|------|------|
| **来源** | https://github.com/eribertomota/forensics-samples |
| **格式** | dd (xz 压缩) |
| **本地路径** | `forensics-samples/` |
| **大小** | 原始 xz ~175 MB，解压后 ~700 MB（含 ext2/ext4/btrfs/ntfs/exfat/vfat 各 50 MB、btrfs 150 MB + 多分区 250 MB + 原始文件 ~28 MB） |
| **用途** | 测试文件恢复、文件雕刻、元数据分析；含已删除目录（所有 `*2` 目录及文件已删除） |
| **平台** | Linux（ext2/ext4, btrfs, FAT32, exFAT）+ Windows (NTFS) + 通用 (vfat) |
| **多分区** | `fs.multiple` — 4 分区：btrfs(2048), ext4(227328), exfat(309248), ntfs(391168)，每分区含 2 文件（debian_logo.jpg + test.txt） |
| **使用** | `mount -o ro,offset=1048576 fs.&lt;name&gt; /mnt`（所有单 FS 镜像从 sector 2048 开始） |

### 5.5 Sleuth Kit 官方测试数据 ✅ 已下载

| 项目 | 说明 |
|------|------|
| **来源** | https://github.com/sleuthkit/sleuthkit_test_data |
| **格式** | DD / E01 (Git LFS) |
| **本地路径** | `sleuthkit_test_data/` |
| **用途** | Sleuth Kit 回归测试，各种文件系统格式 |
| **平台** | 跨平台 |

### 5.6 APFS Specimens（macOS APFS 测试镜像）

| 项目 | 说明 |
|------|------|
| **来源** | https://github.com/dfirlabs/apfs-specimens |
| **格式** | DMG (hdiutil 创建) |
| **大小** | 小镜像（4M-64M），生成脚本驱动 |
| **用途** | 测试 APFS 文件系统解析（大小写敏感、加密、大量文件、扩展属性） |
| **平台** | macOS (APFS) |

### 5.7 DFTT（Digital Forensics Tool Testing Images）✅ 已下载

| 项目 | 说明 |
|------|------|
| **来源** | https://dftt.sourceforge.net/ |
| **格式** | dd |
| **大小** | 小镜像（~11-62 MB） |
| **本地路径** | `testdisk-test-data/dftt/`（ZIP 包） + `dftt/`（解压后的镜像） |
| **已下载** | #6 FAT 反删除, #7 NTFS 反删除, #8 JPEG 搜索, #10 NTFS 自动检测, #11 FAT32 雕刻, #12 EXT2 雕刻, #14 ISO9660 |
| **用途** | 文件雕刻、文件系统分析、字符串搜索等基础测试 |
| **平台** | 通用 |

### 5.8 TestDisk / PhotoRec 专用测试数据 ✅ 已下载

**本地路径**: `testdisk-test-data/`  
**来源**:
- DFTT: https://dftt.sourceforge.net/
- DFRWS 2006: https://github.com/dfrws/dfrws2006-challenge
- TestDisk 官方测试案例: https://www.cgsecurity.org/testdisk_doc/testcase.html

已下载的测试镜像（可直接用于 TestDisk/PhotoRec 练习）：

| 测试项 | 压缩包 | 解压后 | 内容 |
|--------|--------|--------|------|
| **DFTT #7 — NTFS 反删除** | `dftt/7-undel-ntfs.zip` (857 KB) | 6 MB `7-ntfs-undel.dd` | 8个已删文件、2个已删目录、ADS；含驻留/单簇/多碎片文件 |
| **DFTT #8 — JPEG 搜索测试 #1** | `dftt/8-jpeg-search.zip` (839 KB) | 10 MB `8-jpeg-search.dd` | NTFS 含 10 张 JPEG：错误扩展名、ZIP/Word/ADS 嵌入、随机前缀拼接 |
| **DFTT #11 — FAT32 文件雕刻** | `dftt/11-carve-fat.zip` (857 KB) | 62 MB `11-carve-fat.dd` | FAT32，引导扇区损坏，含DOC/WAV/JPG/XLS/PDF/GIF/MOV/WMV/PPT/ZIP |
| **DFTT #12 — EXT2 文件雕刻** | `dftt/12-carve-ext2.zip` (858 KB) | 124 MB `12-carve-ext2.dd` | EXT2 超级块损坏，测试间接/双间接块处理 |
| **DFRWS 2006 雕刻挑战** | `dfrws-2006/dfrws-2006-challenge.zip` (41 MB) | 50 MB `dfrws-2006-challenge.raw` | 无文件系统，含 JPEG/ZIP/HTML/Text/Office 碎片 |
| **DFRWS 2006 原始文件** | `dfrws-2006/dfrws-2006-challenge-files.zip` (35 MB) | 对比验证 | 挑战中使用的原始未碎片文件 |
| **DFRWS 2007 雕刻挑战** | `dfrws-2006/dfrws-2007-challenge.zip` (24 MB) | 168 个原始文件 | JPEG/ZIP/HTML/TXT/PDF + C/C++ 源码，更复杂碎片场景 |

用法示例：
```bash
# 测试 NTFS 反删除
testdisk 7-ntfs-undel.dd       # Partition: None → Advanced → Undelete
photorec 7-ntfs-undel.dd       # 文件雕刻恢复

# 测试 JPEG 搜索（嵌入/错误扩展名/ADS）
testdisk 8-jpeg-search.dd
photorec 8-jpeg-search.dd

# 测试 FAT32 雕刻（引导扇区损坏）
testdisk 11-carve-fat.dd       # Advanced → RebuildBS 修复引导扇区
photorec 11-carve-fat.dd       # 签名雕刻

# DFRWS 2006 雕刻挑战
photorec dfrws-2006-challenge.raw

# DFRWS 2007 雕刻挑战（碎片更复杂）
# 需先解压，然后用 photorec 对解压后的原始文件逐个雕刻
```

---

### 5.9 NapierOne — 现代混合文件数据集 ✅ 已下载

**本地路径**: `napierone-raw-files/` (第 1 批) + `napierone-raw-files-2/` (第 2 批)  
**来源**: https://github.com/simonrdavies/NapierOne  
**AWS S3**: `s3://napierone.com/NapierOne/Data/` (eu-north-1，无需认证)  
**用途**: 提供大量真实原始文件，可作为雕刻素材库构建定制 dd 镜像，或直接验证雕刻恢复的文件完整性

已下载 15 个文件类型的 tiny 子集（各 100 个文件 + 1 个 PDF 说明）：

| 类型 | 压缩包 | 文件数 | 格式用途 |
|------|--------|--------|----------|
| **CSS** | `napierone-raw-files/CSS-tiny.zip` (4 MB) | 100 | 网页样式表 |
| **CSV** | `napierone-raw-files/CSV-tiny.zip` (8 MB) | 100 | 表格数据 |
| **EPUB** | `napierone-raw-files/EPUB-tiny.zip` (28 MB) | 100 | 电子书格式 |
| **HTML** | `napierone-raw-files/HTML-tiny.zip` (4 MB) | 100 | 网页标记语言 |
| **ICS** | `napierone-raw-files/ICS-tiny.zip` (2 MB) | 100 | 日历数据 |
| **JavaScript** | `napierone-raw-files/JAVASCRIPT-tiny.zip` (5 MB) | 100 | JS 脚本文件 |
| **JSON** | `napierone-raw-files/JSON-tiny.zip` (1 MB) | 100 | 结构化数据 |
| **TXT** | `napierone-raw-files/TXT-tiny.zip` (4 MB) | 100 | 纯文本文件 |
| **XML** | `napierone-raw-files/XML-tiny.zip` (4 MB) | 100 | XML 标记语言 |
| **BMP** | `napierone-raw-files-2/BMP-tiny.zip` (191 MB) | 100 | BMP 位图 |
| **DOC** | `napierone-raw-files-2/DOC-tiny.zip` (21 MB) | 100 | Word 文档 |
| **GIF** | `napierone-raw-files-2/GIF-tiny.zip` (64 MB) | 100 | GIF 动图 |
| **JPG** | `napierone-raw-files-2/JPG-from-web-tiny.zip` (11 MB) | 100 | JPEG 图片 |
| **SVG** | `napierone-raw-files-2/SVG-tiny.zip` (150 MB) | 100 | SVG 矢量图 |
| **XLS** | `napierone-raw-files-2/XLS-tiny.zip` (14 MB) | 100 | Excel 表格 |

**更多类型**：NapierOne 另有 29 个文件类型（APK/BIN/DOCX/DWG/ELF/EPS/EXE/GZIP/MKV/MP3/MP4/ODS/OXPS/PDF/PNG/PPT/PPTX/PS1/RAR/TAR/TIF/WEBP/XLSX/ZIP/ZLIB/7ZIP 等），可在需要时以同样方式下载 tiny/small/total 子集。

---

### 5.10 自定义文件雕刻测试镜像 ✅ 已构建

**本地路径**: `custom-carving-images/`  
**构建工具**: `rebuild_carving.py`（Python 脚本，将原始文件带随机间隙拼接为 dd 镜像）  
**用途**: 直接供 PhotoRec / TestDisk 进行文件雕刻练习，覆盖 25+ 种文件类型  
**来源**: NapierOne(1/2批) + NIST CFReDS + DFRWS 2006/2007 + Govdocs1 混合构建

| 镜像 | 文件数 | 大小 |
|------|--------|------|
| `carving-00.dd` ~ `carving-19.dd` (20 张) | 各 55 个 | 42 ~ 181 MB |
| **合计** | **1,100** | **1,217 MB** |

文件类型覆盖见上方「支持雕刻的文件类型总览」表。

用法示例：
```bash
# 群雕模式（恢复所有能识别的文件类型）
photorec carving-00.dd

# 查看镜像结构（无文件系统，mmls 将报错，属正常）
```
每张镜像附带 `manifest.json` 记载每个文件的偏移量、大小，可用于验证雕刻结果。

---

### 5.11 Govdocs1 按类型子集 ✅ 已下载

**本地路径**: `govdocs1/`  
**来源**: https://digitalcorpora.s3.amazonaws.com/corpora/files/govdocs1/by_type/  
**用途**: 百万公共领域文件的按类型子集，作为雕刻素材补充

| 子集 | 压缩包 | 文件数 | 说明 |
|------|--------|--------|------|
| **DOCX** | `docx_by_type.zip` (28 MB) | 163 个 DOCX | 新版 Word 文档 |
| **RTF** | `rtf_by_type.zip` (65 MB) | 1125 个 RTF | RTF 格式文档（抽样 200 入雕刻镜像） |

---

### 5.12 FILETYPES1 — 32 种文件类型样本 ✅ 已下载

**本地路径**: `filetypes1/`  
**来源**: https://downloads.digitalcorpora.org/corpora/files/filetypes1/  
**用途**: UT San Antonio（Dr. Nicole Beebe）提供的 32 种文件类型公开样本，原用于 Sceadan 文件类型分类器开发。补充 GOVDOCS1 语料库中未覆盖的文件类型。

**下载内容**（32 个 ZIP，共 5.28 GB）：

| 类型 | 压缩包 | 大小 | 说明 |
|------|--------|------|------|
| **ASP** | `ASP.zip` | 2.7 MB | ASP 网页脚本 |
| **AVI** | `AVI.zip` | 63 MB | AVI 视频 |
| **B64** | `B64.zip` | 12 MB | Base64 编码 |
| **B85** | `B85.zip` | 10.7 MB | Base85 编码 |
| **BZ2** | `BZ2.zip` | 10.3 MB | Bzip2 压缩 |
| **CSS** | `CSS.zip` | 219 KB | 样式表 |
| **DLL** | `DLL.zip` | 19.8 MB | Windows 动态链接库 |
| **ELF** | `ELF.zip` | 1.6 MB | Linux ELF 可执行文件 |
| **EXE** | `EXE.zip` | 277 MB | Windows PE 可执行文件 |
| **EXT3** | `EXT3.zip` | 246 KB | EXT3 文件系统超级块 |
| **FAT** | `FAT.zip` | 1 MB | FAT 文件系统超级块 |
| **FLV** | `FLV.zip` | 3.4 GB | Flash 视频 |
| **JAR** | `JAR.zip` | 36.6 MB | Java 归档 |
| **JB2** | `JB2.zip` | 7.2 MB | JBIG2 压缩图像 |
| **JS** | `JS.zip` | 945 KB | JavaScript |
| **M4A** | `M4A.zip` | 158 MB | MPEG-4 音频 |
| **MOV** | `MOV.zip` | 157 MB | QuickTime 视频 |
| **MP3** | `MP3.zip` | 453 MB | MP3 音频 |
| **MP4** | `MP4.zip` | 18.3 MB | MP4 视频 |
| **NTFS** | `NTFS.zip` | 1.1 MB | NTFS 文件系统元数据 |
| **PST** | `PST.zip` | 34.3 MB | Outlook 个人文件夹 |
| **Random** | `Random.zip` | 7.4 MB | 随机数据 |
| **RPM** | `RPM.zip` | 7.9 MB | RPM 软件包 |
| **RTF** | `RTF.zip` | 45 MB | RTF 文档 |
| **SWF** | `SWF.zip` | 352 MB | Shockwave Flash |
| **Tbird** | `Tbird.zip` | 110 MB | Thunderbird 邮件 |
| **TXT** | `TXT.zip` | 2.9 MB | 纯文本 |
| **URL** | `URL.zip` | 3.6 MB | URL 快捷方式 |
| **WAV** | `WAV.zip` | 376 MB | WAV 音频 |
| **WMA** | `WMA.zip` | 40 MB | WMA 音频 |
| **XLSX** | `XLSX.zip` | 9.1 MB | Excel 2007+ 表格 |
| **ZIP** | `ZIP.zip` | 14.4 MB | ZIP 压缩包 |

此外包含 `_README/` 目录，内含每个文件的版权声明和文件列表。

---

## 六、更多文件雕刻测试源（推荐列表）

以下数据源专门针对文件雕刻测试，未包含在前述已下载数据中，可供后续扩展：

### 6.1 DFRWS 2007 雕刻挑战（进阶）✅ 已下载

| 项目 | 说明 |
|------|------|
| **来源** | https://github.com/dfrws/dfrws2007-challenge |
| **格式** | zip (24 MB)，解压后 168 个文件 |
| **文件类型** | JPEG, ZIP, HTML, Text, PDF, C/C++ |
| **特点** | 比 2006 更复杂碎片场景（顺序/乱序/缺失），学术界广泛引用为标准测试集 |
| **状态** | 已下载至 `testdisk-test-data/dfrws-2006/dfrws-2007-challenge.zip` |

### 6.2 NIST CFTT 文件雕刻测试 ✅ 图形镜像已解压

| 项目 | 说明 |
|------|------|
| **来源** | https://cfreds-archive.nist.gov/filecarvingtestreports.html |
| **格式** | dd (bzip2/tar)，图形 ~2 GB，视频 ~11.5 GB |
| **已下载** | 图形 dd 镜像 (`graphic-images.tbz2`, 1.5 GB) + 图形源文件 (`graphic-src.tbz2`, 222 MB) |
| **已解压** | 8 个 `.dd` 于 `file-carving-cftt-images/images/images/` |
| **跳过** | 视频 dd 镜像 (`images.tar.bz2`, 11.5 GB)——超过磁盘限制 |
| **路径** | `file-carving-cftt-images/images/` |

### 6.3 Govdocs1 — 百万文件语料库（雕刻素材来源）

| 项目 | 说明 |
|------|------|
| **来源** | https://digitalcorpora.org/corpora/file-corpora/files/ |
| **格式** | 原始文件 (1000 目录 × 1000 文件) / ZIP 包 |
| **大小** | ~1 TB（可选子集 subset0-9，各 1000 文件） |
| **用途** | 不直接是雕刻镜像，而是作为**雕刻素材库** —— 可从中选取文件自行构建雕刻测试镜像 |
| **工具** | 配合 `mk-file`、`fill-fs` 等 NIST 工具自行生成定制雕刻镜像 |

### 6.4 DFTT #8 — JPEG 搜索测试 #1 ✅ 已下载

| 项目 | 说明 |
|------|------|
| **来源** | https://dftt.sourceforge.net/test8/ |
| **格式** | dd |
| **大小** | 小镜像 (~839 KB zip) |
| **用途** | 测试 JPEG 文件签名搜索与雕刻准确性 |
| **状态** | 已下载至 `testdisk-test-data/dftt/8-jpeg-search.zip` |

### 6.5 forensics-samples 多文件系统镜像 ✅ 已下载

见 5.4 节 —— 已下载至 `forensics-samples/`，含 7 个 dd 镜像 + 原始文件，可直接用于雕刻/反删除测试。

---

## 已知问题 / Known Issues

### 雕刻测试报告 (`docs/reports/file_carving_graphic_results.csv`) 数据异常

| # | 镜像 | 源文件 | 问题描述 |
|---|------|--------|----------|
| 1 | L2_Graphic.dd | 02010025.pcx | **node_size 少 1 字节**：ref_size=806664 但 node_size=806663 |
| 2 | L2_Graphic.dd | 09260002.jpg | **node_size 非整块异常**：node_size=40748，介于 embed_bytes(19968) 和 ref_size(60716) 之间，且缺少 "fragment" 标注 |
| 3 | L2_Graphic.dd | 100_0018.tif | **node_size < ref_size 但无标注**：node_size=13325824 < ref_size=14969328，碎片文件但无 "fragment" 标注 |
| 4 | L5_Graphic.dd | 100_0018.tif | **L5 级缺标注**：node_size=4989440=embed_bytes（仅第一片），无 "fragment" 标注，同级别其他行均有 |
| 5 | L1_Graphic.dd | 全部 6 个文件 | **carve_ok=yes 语义不明确**：仅恢复首片（约 50% 数据），carve_md5 ≠ ref_md5，应使用三级状态（none/partial/full） |
| 6 | L4_Graphic.dd | 09260002.jpg | **node_size 含义不清**：node_size=34041，介于 embed_bytes(30208) 和 ref_size(60716) 之间 |

**可能原因**: `node_size` 含义在脚本中不统一（有时记录全文件大小、有时记录片段大小、有时记录节点跨跨度），且 L2 以上碎片文件的 "fragment" 标注条件不一致。

---

## 虚拟磁盘金标样本（A1-1c）

新增目录 `testdata/virtual-disk-golden/`，用于验证 `disk/VirtualDiskSource.h` 的**真实容器**兼容性（区别于 `tests/` 里的内存合成 fixture）。

| 项 | 内容 |
|---|---|
| 来源 | `qemu/qemu` 的 `tests/qemu-iotests/sample_images`，固定 commit `71f25c75417d31c214ba2cd26b273c114fc99987` |
| 为何算「真实」 | 上游 README 要求样本必须由**原生工具**创建：VHDX 由 Hyper-V、VHD 由 Virtual PC / Disk2vhd、VMDK 由 VMware 生成，不是 QEMU 造的，也不是本项目造的 |
| 规模 | 10 个镜像 / 约 644 MiB（含 3 个 100 MiB 以上的零填充动态盘） |
| 完整性 | `manifest.json` 记录 SHA-256、文件大小、创建工具、来源 URL；`Fetch-VirtualDiskGolden.py --verify-only` 离线校验 |
| 独立 oracle | `Verify-VirtualDiskGolden.py` 用 Python `struct` 独立重算 VHD dynamic / VHDX / RAW 的扇区内容，产出 `expectations.tsv` 与 `report.txt`；C++ CTest `VirtualDiskGolden` 逐扇区比对 |
| 版本控制 | 与 `testdata/` 其余取证语料一致，**不入库**；在无样本环境下该 CTest 会明确 `SKIPPED` 并提示获取命令 |

```bash
python testdata/tools/Fetch-VirtualDiskGolden.py            # 下载 + bz2 解压 + manifest
python testdata/tools/Fetch-VirtualDiskGolden.py --verify-only
python testdata/tools/Verify-VirtualDiskGolden.py          # 校验 SHA-256 + 重生成 oracle
ctest --test-dir build/a0-verify -R VirtualDiskGolden -V
```

| 分组 | 样本 | 验证点 |
|---|---|---|
| VHD dynamic | `virtualpc-dynamic.vhd`、`hyperv2012r2-dynamic.vhd`、`d2v-zerofilled.vhd` | 127 GiB / 251 MiB 虚拟几何、BAT 稀疏零填充 |
| VHDX dynamic | `iotest-dynamic-1G.vhdx`、`test-disk2vhd.vhdx`、`iotest-dirtylog-10G-4M.vhdx` | 真实 MBR、qemu `0xA5` 填充、交错 BAT、脏日志告警 |
| VMDK | `iotest-version3.vmdk`、`afl9.vmdk` | streamOptimized 拒绝、损坏头鲁棒性 |
| RAW | `grub_mbr.raw`、`afl5.img` | GRUB MBR、残尾扇区、越界 |

**覆盖边界**：QCOW2、VDI、普通 monolithicSparse VMDK 尚无真实样本；三个真实 VHD 的数据区为零，未验证含数据的 VHD dynamic 读取。`log2timeline/dfvfs` 的 `ext2.{raw,vmdk,qcow2,vhd,vhdx}` 同内容多容器组是首选补充来源（`.raw` 可直接当逐扇区 oracle），见 `docs/ROADMAP_RECOVERY.md` A1-1d。

---

## BitLocker 合成金标语料（A1-3a）

与上面的**真实取证镜像不同**，`testdata/bitlocker-golden/` 全部是**合成加密卷**，用于验证只读解锁链路的算术与元数据解析，不代表真实 Windows BitLocker 卷兼容性。

| 项 | 说明 |
|---|---|
| 生成 | `python testdata/tools/Gen-BitLockerGolden.py`（确定性，可重复）；`--verify-only` 用 `manifest.json` 的 SHA-256 离线校验 |
| oracle 独立性 | 纯 Python AES（S-box 由 GF(2⁸) 逆元推导）+ Python `hashlib` SHA-256，与 C++ 实现无共享代码；生成器自身先用 FIPS-197 AES-128/192/256 向量自检 |
| 几何 | 512 字节扇区、256 KiB 卷、16 扇区卷头重定位到 `0x20000`、三份 FVE 元数据块 `0x30000/0x31000/0x32000`（各 4 KiB） |
| 卷类型 | 0x8004 XTS-128 × clear/recovery/副本损坏/tag 篡改、0x8005 XTS-256 × 用户密码、0x8002 CBC-128 × BEK 外部密钥、0x8003 CBC-256 × 恢复密钥、0x8000 Diffuser 拒绝、Vista v1 拒绝、非 BitLocker 拒绝 |
| 期望值 | `primitives.tsv`（XTS/CBC/CCM 原语）、`stretch.tsv`（0x100000 次拉伸）、`cases.tsv`（凭据与预期结果）、`sectors.tsv`（96 个解密后逻辑读的 FNV-1a64 指纹，含重定位卷头、元数据区零填充、跨扇区、尾部扇区） |
| 消费方 | CTest `BitLockerCrypto` / `BitLockerVolume`；不依赖 GUI、不依赖物理设备 |

**覆盖边界**：没有真实 BitLocker 卷样本；TPM / TPM+PIN、Vista、BitLocker To Go、Elephant Diffuser、4Kn、部分解密 EOW、`.BEK` 文件结构解析均未完成。真实卷补齐计划见 `docs/ROADMAP_RECOVERY.md` A1-3b。

---

## ReFS 合成金标语料（A1-5a）

与上面的**真实取证镜像不同**，`testdata/refs-golden/` 全部是**按公开 ReFS 结构文档逐字节构造的合成卷**，用于验证只读解析、校验链与数据重组的自洽性，**不代表真实 Windows ReFS 卷兼容性**。

| 项 | 说明 |
|---|---|
| 生成 | `python testdata/tools/Gen-ReFSGolden.py`（确定性，可重复）；`--verify-only` 用 `manifest.json` 的 SHA-256 离线校验 |
| oracle 独立性 | 纯 Python CRC-32C / CRC-64/NVME / SHA-256（`hashlib`）/ VBR ROR1+ADD / 小端结构打包，与 C++ 实现无共享代码；生成器启动时用标准 check value 自检（`0xE3069283`、`0xAE8B14860A799888`、FIPS-180-4 SHA-256） |
| 几何 | 512e 4 KiB 簇（8 MiB 卷）、64 KiB 簇（64 MiB 卷）、4Kn（4096 B 扇区 × 1）；16 KiB 元数据页；64 MiB/容器，容器 ID 2 |
| 卷类型 | v3.14 CRC64 / v3.10 direct root list / v3.14 SHA-256 页引用 / 两层 B+ 树 / SUPB 主副本损坏回退 / CHKP 高时钟副本损坏回退（8 个正向）；根摘要篡改、节点不变量破坏、VBR 校验损坏、BitLocker 加密、非 ReFS（5 个拒绝） |
| 期望值 | `cases.tsv`（13 卷的预期状态与几何）、`files.tsv`（48 条文件期望：大小/分配/属性/三个 FILETIME/驻留类型/extent 列表/SHA-256/FNV-1a）、`primitives.tsv`（16 条校验原语向量）、`manifest.json`、`report.txt` |
| 消费方 | CTest `RefsCrypto` / `RefsVolume`；不依赖 GUI、不依赖物理设备、不依赖 Qt/Win32 |

**覆盖边界**：**没有任何真实 Windows ReFS 卷**。三处属合成卷的扁平化金标布局，不能当作真实卷规范：SUPB/CHKP 自校验的精确清零范围、embedded `$DATA` sub-record 链的内部 framing、目录名→子目录 OID 绑定（现通过 Parent-Child Table 暴露关系、由调用方按 OID 枚举）。未支持 legacy v3.4–v3.9 对象表 / 104 B 页引用、split 文件 type-0x40 backing record、ADS / 快照 / 压缩 / integrity streams / 硬链接、稀疏文件、从 B+ 树 slack 恢复删除项。详见 `docs/ROADMAP_RECOVERY.md` A1-5a 覆盖边界与 A1-5b 待办。

---

## mdadm / RAID 组合链合成金标语料（A1-4b-a）

与上面的**真实取证镜像不同**，`testdata/raid-probe-golden/` 全部是**按 mdadm 公开超块布局与 RAID 闭式公式逐字节构造的合成成员盘**，用于验证元数据解析、布局探测与组合只读源自洽性，**不代表真实阵列兼容性**。

| 项 | 说明 |
|---|---|
| 生成 | `python testdata/tools/Gen-RaidProbeGolden.py`（确定性，可重复）；`--verify-only` 用 `manifest.json` 的 SHA-256 离线校验 58 个文件 |
| oracle 独立性 | 纯 Python 闭式 parity 放置 + 位运算 GF(2⁸)（0x11D）+ 按 `md_p.h` / `super1.c` 自行组装 0.90 / 1.x 超块与校验和；与 C++ 的 role-slot 枚举和 exp/log 表实现无共享代码 |
| 元数据 | mdadm 0.90（4 KiB 超块，末尾 64 KiB 保留区）、1.0（末尾 8–12 KiB）、1.1（偏移 0）、1.2（偏移 4 KiB）；含 `dev_roles[]` 盘序、`data_offset`、`set_uuid`、校验和 |
| 用例 | 16 个：RAID1/5/6、左右对称与非对称布局、盘序打乱、降级缺盘、VHD fixed 容器包装、盲探测成功与歧义/无证据/超预算拒绝、UUID 冲突、部分校验和损坏、parity 与元数据矛盾、reshape 拒绝、盘角色冲突 |
| 期望值 | `cases.tsv`（16 例的预期状态/几何/盘序/缺盘）、`chain.tsv`（20 条逐层 FNV-1a + SHA-256 双指纹）、`primitives.tsv`（6 条原语向量）、`configs/*.conf`（配置驱动的非 GUI 输入）、`manifest.json`、`report.txt` |
| 消费方 | CTest `RaidProbe` / `ScanSourceChain`；后者另复用 `bitlocker-golden/sectors.tsv` 与 `refs-golden/files.tsv` 验证 BitLocker / ReFS 两段接线 |

**两个由金标实测得到的关键约束**：P/Q 逐扇区成立、与 chunk 大小无关，故 chunk 必须由配置给出候选集并由卷内明文锚点判定；单奇偶 XOR 对称（`A⊕B=P` ⇒ `A⊕P=B`），任意成员都可被当成 P 且校验通过，故盘序与 parity 轮转只能靠**跨条带的已知明文锚点**区分，少于 2 个锚点时探测器直接拒绝。

**覆盖边界**：**没有任何真实 mdadm / 硬件 RAID 阵列**。未做 DDF、Intel RIS、厂商专有布局、reshape / bitmap / PPL / journal / bblog、4Kn、事件数冲突仲裁；0.90 与 1.x 仅按小端目标解析；盲探测不猜无 parity 的级别，也不对全零条带计分。组合源目前只接受注入式字节源，尚未接 `SectorReader` 设备主路径。详见 `docs/ROADMAP_RECOVERY.md` A1-4b-a 实装说明与 A1-4b-b 待办。

---

## 签名多模式匹配合成金标语料（A1-6 / 并行轨）

与上面的**真实取证镜像不同**，`testdata/signature-matcher-golden/` 全部是**按规范构造的合成 64 KiB 镜像**，用于验证流式 Aho-Corasick 多模式签名匹配器（按类型恢复参数化引擎底座）的自洽性，**不代表真实磁盘镜像的误报 / 漏报率**。

| 项 | 说明 |
|---|---|
| 生成 | `python testdata/tools/Gen-SignatureMatcherGolden.py`（确定性，可重复）；`--verify-only` 用 `manifest.json` 的 SHA-256 离线校验 `disk_image.bin` |
| oracle 独立性 | 纯 Python 朴素逐字节逐模式扫描（与 C++ AC 自动机无共享代码路径），交叉验证期望命中集合 |
| 语料 | 64 KiB 确定性填充镜像；8 类 CATALOG：jpg / png / gif / zip / pdf(+EOF footer) / riff(+WAVE secondary) / mp4(headerOffset=4) / cr2 |
| 期望值 | `cases.tsv`（9 期望命中：fileStart / magicOffset / type 等）、`negative.tsv`（由命中间隙 + 16 字节 margin 派生的 10 个负向区，规避确定性填充偶含 magic）、`streaming.tsv`（512 B 窗口切分验证）、`manifest.json`（SHA-256）、`expectations.json` |
| 消费方 | CTest `SignatureMatcher`（#77）；覆盖 AC 单元属性、金标单缓冲 vs 独立 oracle、负向区零误报、流式==单缓冲、type-filter、alignment |

**两个由金标实测得到的关键约束**：跨窗口 magic 须「仅命中一次」——靠每窗重喂前窗末尾 `(maxDepth-1)` 字节实现；footer / secondary 须在后续窗口字节到达时**延迟解析**，跨窗口 OR 累积，且整段搜索区间可见或 `Finish()` 时才定稿，否则 footer 跨窗口会漏判或被 `Finish()` 空缓冲覆盖（初版 bug，已修）。

**覆盖边界**：**没有任何真实磁盘 / 取证镜像**。未量化真实全盘（碎片化、重叠 magic、坏扇区零填充）误报 / 漏报率；secondary / footer 仅覆盖 riff / pdf 两类，其余类型无 secondary / footer 约束；自动机按字节构建，不支持位级 / 变长 / 正则式签名；当前**已接 `RawScan` 主路径**（`ScanBatch` 流式 AC 批扫描替换线性 `memcmp`），并落地 `DeepScan` 损坏 NTFS 回退（A1-7）与并行 RAW 扫描（A1-8，跨线程分块 + 512 重叠 + 按 rawOffset 合并）。详见 `docs/ROADMAP_RECOVERY.md` A1-6/7/8 实装说明。

## DeepScan 损坏 NTFS 回退金标语料（A1-7）

验证 `DeepScan` 在「识别到已知 FS（NTFS/FAT boot 合法）但结构化解析几乎无收获（如 MFT/目录被覆盖）」时，透明回退到 `RawScan` 内容雕刻，恢复嵌入的魔数文件。

| 项 | 说明 |
| --- | --- |
| 镜像 | 4 MiB 合成镜像：seeded 随机填充（`mt19937_64(0x1234abcd)`）+ 合法 NTFS boot 扇区（OEM=NTFS、bytesPerSector=512、sectorsPerCluster=1、totalSectors、clustersPerRecord=-10、0x55AA），MFT 位置故意指向垃圾 |
| 植入 magic | 512 对齐的 header-only 类型（JPEG `FFD8FFE0`、PNG `89504E470D0A1A0A`、GIF `474946383961`），分布于 sector 10/100/500/2000/4000（覆盖高偏移，排除交接盲区） |
| 消费方 | CTest `DeepScanFallback`：正向恢复全部植入 magic；反向（同 magic、不植 boot）零恢复，证明回退门控正确 |
| 已知约束 | 结构敏感型（tif 需 IFD、cfb/OLE2 需内部布局、pdf 需 `%%EOF`）在随机/常量垃圾下校验失败、雕刻器不发射节点，属正确行为；回退仅继承 `RawScan` 既有雕刻策略 |

## 并行 RAW 扫描等价金标语料（A1-8）

验证 `RawScan::Run(reader, partition, numThreads)` 在多线程下恢复的文件集与串行 `Run(reader, partition)` 完全一致，且不同线程数（2/4/8）结果一致。

| 项 | 说明 |
| --- | --- |
| 镜像 | 8 MiB 合成镜像：seeded 随机填充（`mt19937_64(0x9e3779b97f4a7c15)`），16384 扇区 |
| 植入 magic | 严格递增的 512 对齐 header-only 类型（JPEG/PNG/GIF），分布于 sector 16/100/500/2048/3000/4096/6000/8192/10000/12288/14000/16000；其中 2048/4096/8192/12288 恰为 2/4/8 线程的 chunk 边界，用于触发 512 字节跨块重叠的双检测与合并去重 |
| 合并策略 | 每线程独立 `SectorReader`（经 `GetImagePath()` 克隆）+ 独立 `SignatureDB`；按 `rawOffset` 去重合并；全局 `RescanNestedGraphicHeaders`/`AddFragmentNodes` 仅主线程跑一次。**A1-10 起** worker 的 DB 由主线程 `m_db->GetAll()` 快照经精确集构造器克隆（本金标只用内建库，故与早期「默认 ctor 确定性构造」结果相同；自定义 / 过滤库下必须走克隆，见下一节） |
| 消费方 | CTest `RawScanParallel`：对 N=2/4/8 断言 rawOffset/ext/size 集合与串行逐偏移一致、无重复偏移 |
| 已知约束 | 设计上规避唯一会分叉的情形（某 magic 落在另一 magic 的已刻延伸段内且跨 chunk）——本金标所有 magic 严格递增，每个刻出段恰为 [本, 下一)，无嵌套；非镜像源（设备/虚拟盘）无法克隆 reader，自动回退单线程 |

## 自定义签名样例与加载金标（A1-9）

`testdata/custom_signatures.json` 是 `SIGNATURE_FORMAT.md` 的**可直接加载样例**（FAQ、PRODUCT_SCOPE 早已引用，此前文件缺失，本阶段补齐）。它不是磁盘镜像，而是 JSON 签名定义，用于验证 `SignatureDB::LoadFromFile` 的解析与接线。

| 项 | 说明 |
| --- | --- |
| 文件 | `testdata/custom_signatures.json`，3 条：`sqlite`（`SQLite format 3\0`，4 GiB）、`pdf`（`%PDF` + `%%EOF` footer）、`wav`（`RIFF` + 偏移 8 的 `WAVE` 二级签名） |
| 加载实现 | `std::ifstream`(`Utils::WideToUtf8`) + 零依赖 `scan/SignatureJson.h`；**无 Qt**，`PHOENIX_BUILD_GUI=0` 与 GUI 构建行为一致（旧实现非 GUI 恒返回 -1） |
| 消费方 | CTest `SignatureDBLoad`（90 项）：解析器 RFC 8259 子集属性 + 20 类严格拒绝、`LoadFromFile` 字段逐项核对（含 trim / 小写 hex / 默认 max_size / 非零 offset / secondary）、无效条目跳过与坏 hex 拒绝、UTF-8 BOM 剥离、`Match()`/`ScanBatch()` 证明 AC 匹配器重建、本样例 3 条逐字段验证 |
| 源码级负向验证 | 临时把 `LoadFromFile` 的 `m_matcherBuilt = false` 改为 `true`（陈旧匹配器）→ `Match()` 与 `ScanBatch()` 两条断言 FAIL（90 项 / 2 FAIL / EXIT=1），恢复源码后全绿 |
| 已知约束 | 解析器为签名 schema 所需的最小 RFC 8259 子集（非通用 JSON 库）：不支持超大整数精确表示（`double` 承载，`max_size` 最大到 2^53 无精度损失）、不保留对象成员顺序语义（重复键取首个）、不含流式/增量解析；运行时内建签名实测 513 条，与 §6 的 514（`UiTypeCoverage` 口径）不同 |

## 并行 RAW 扫描生产接线金标语料（A1-10）

`test_rawscan_parallel_wiring.cpp` 在运行时生成的 4 MiB 临时镜像（`rawscan_parallel_wiring.dd`，跑完即删），**不落盘为长期金标文件**；用于验证并行路径在真实生产入口 `CarveModuleService::Run`（`UnifiedVolumeScanner` 阶段 3）上的接线正确性。

| 项 | 说明 |
| --- | --- |
| 镜像规格 | 4 MiB = 8192 扇区；`mt19937_64(0x5a17c0de1234abcd)` 确定性随机填充；10 个严格递增、512 对齐的 header-only magic（png / jpg / gif），其中 sector 2048 / 4096 / 6144 恰为 4 线程 chunk 边界 |
| 签名集 | ① 精确集自定义库（只含 1 条 `png` 签名，`SignatureDB(std::vector<FileSignature>)` 构造）；② 内建库 + `targetExtensions={png}` 过滤 |
| 断言 | `CarveConfig`/`UnifiedScanConfig`/`RawScan` 默认线程数均为 1；`MakeCarveConfig` 三字段转发；自定义库下 serial=parallel(4) 逐偏移一致、png 全命中且 jpg/gif 不命中；过滤库下两者一致且发射扩展名全为 png；文件 / 进度回调零跨线程；取消标志传播到 `ScanResult.cancelled` |
| 合并策略 | worker 各持独立 `SectorReader`（`GetImagePath()` 克隆）+ 独立 `SignatureDB`（`m_db->GetAll()` 快照精确构造）；主线程按 `rawOffset` 去重后再跑一次全局 pass；回调只在调用线程发射 |
| 源码级负向验证 | ① 把 `SetThreadCount(ResolveThreadCount(config.numThreads))` 改为 `SetThreadCount(1)` → 源码守卫 FAIL（EXIT=1）；② 把 worker 改回 `SignatureDB localDb;`（内建 513 条）→ 自定义库用例 serial=5 / parallel=63，2 处 FAIL（EXIT=1）；恢复后 26 项全绿 |
| 已知约束 | 默认 `numThreads == 1`（串行，零行为变化），并行须显式配置；非镜像源无法克隆 reader 自动回退单线程；本金标所有 magic 严格递增，规避「嵌套 jpeg 跨 chunk 的 cursor 跳过」这一唯一已知分叉点；**合成镜像吞吐已由 A1-11 量化**，真实磁盘 / 真实卷提速与误报率仍未量化 |

## 并行 RAW 扫描吞吐基准镜像（A1-11）

`tools/bench_rawscan_parallel.cpp` 在运行时生成的临时镜像（`bench_rawscan_parallel.dd`，默认跑完删除，`--keep` 可保留），**不是金标语料**，只用于吞吐测量，不注册 CTest（计时断言会抖动）。

| 项 | 说明 |
| --- | --- |
| 镜像规格 | 默认 256 MiB = 524288 扇区（`--size-mib` 可调）；常量 `0x5A` 填充；每 256 KiB 轮换植入 png / jpg / gif 的 header-only magic，从扇区 32 起共 1023 个；每段恰为 `[本, 下一)`，无嵌套、无随机假阳性 magic |
| 计时口径 | `steady_clock` 包住整次 `Run()`（含 reader 打开、DB 构造、扫描、合并、全局 pass）；`_commit` 强制写盘回写 + 整文件预读 + 2 次未计时串行 warm-up；每线程数重复 3 次取中位数，且每轮旋转线程数顺序；每次与 warm-up 串行基线逐偏移比对 |
| 入口 | 默认直连 `RawScan::Run`；`--via-service` 走 A1-10 生产配置链 `CarveModuleService::Run` |
| 实测归档 | `docs/reports/rawscan_parallel_bench_2026-11-08.txt` + `*_direct.csv` / `*_service.csv`；结论见 `docs/PERFORMANCE_BASELINE.md §8` |
| 已知约束 | 合成低熵填充（无随机假阳性）、OS 页缓存普通文件（非真实物理盘 I/O / 无坏扇区）、magic 严格递增（规避嵌套 jpeg 分叉）、仅 RAW 雕刻流水线（非 GUI 端到端）、CPU/RAM/磁盘型号未采集故不可跨机比较。首版按 1→8 顺序测量曾被写盘回写污染（虚高 8.44x），已由预热 + 顺序轮换修正 |

## 真实 exFAT 恢复字节金标（W3 / CAR-003，`testdata/cfreds/dfr-01-xfat.dd`）

CTest `ExfatRecoverBytes` 的语料。此前 `ExfatProbeDispatch` 只校验解析器路由，不校验写出内容；本门禁首次对真实 exFAT 已删文件做**逐字节 + MD5**验收。

| 项 | 值 |
| --- | --- |
| 镜像 | CFReDS `dfr-01-xfat.dd`，64 MiB（128000 扇区），exFAT 引导扇区位于 **sector 2048**，前 1 MiB 为 MBR/GPT 保留区 |
| 几何（独立 oracle 自解析 boot） | 512 B/扇区、8 扇区/簇（4 KiB）、FAT @ sector 128、cluster heap @ sector 256、15584 簇、root = cluster 5、volumeLength = 124928 扇区 |
| 目标文件 | 已删 `Betelgeuse.txt`，`NoFatChain=1`（连续），first cluster = **13**，dataLength = **4396** B，2 簇 |
| 期望字节偏移 | `(2048 + 256 + (13-2)*8) * 512 = 1224704`（sector 2392 + 2400） |
| 期望 MD5 | `6650a82d6499770bf7eef8045a014b7d`（Python `hashlib` 独立计算），内容以 `\nDFR\nFile Betelgeuse.txt` 开头、尾部为可打印字母表填充 |
| oracle 独立性 | 测试内手写 boot 解析 + 已删根目录项遍历（`0x05` / `0x40` / `0x41`），不调用 `ExfatParser` / `RecoverEngine`；另用相邻簇（cluster 12）作负向对照，防止全零/常量数据让字节比较平凡通过 |
| 只读不变量 | 扫描 + 恢复前后对整镜像做 FNV-1a 64 全文件哈希比对（`9989bd85697d640d`），必须不变 |
| 发现的缺陷 | 首跑 FAIL：恢复 MD5 为 `998c2e0c7fb86669368ab9f5b9f932fe`。已删连续流的簇链 `[2392, 2400]` 被 FAT12/16/32 启发式拼接器改写为 `[2392, 2336]`（删除后 FAT 项清零，拼接器误判为空闲簇）。修复见 `ROADMAP_RECOVERY.md §8 CAR-003` |
| 验证 | 30 项断言全绿（GCC 7.3 / GCC 14.2 双工具链）；两轮源码级负向验证分别放开拼接器、恢复旧 `chainNoFat`，各触发 2 项 FAIL |

## 快速格式化卷恢复金标（W3 / `FormattedVolumeRecovery`，运行时合成）

CTest `FormattedVolumeRecovery` 的语料，4 MiB，运行时生成、跑完删除，**非真实快速格式化磁盘**。

| 项 | 说明 |
| --- | --- |
| 建模 | `std::mt19937_64` seed `0x5EEDF0A17` 随机填充；写入全新 NTFS boot（512 B/sector、1 sector/cluster、MFT LCN 16）；把 boot 之后的 **128 KiB 元数据区清零**（模拟快速格式化只重建元数据），数据区原样保留残留文件 |
| 残留载荷 | 5 个 512 对齐的 header-only magic（jpg / png / gif / jpg / png）+ 64 B 确定性字母表尾，位于 sector 300 / 900 / 2100 / 5100 / 7400，全部落在元数据区之外 |
| 独立 oracle | 直接重读原始镜像，逐项比对 5 段载荷字节，并验证 512..131071 全零；生产路径 `DeepScan` 结构化解析 0 文件 → RAW 回退，除验偏移命中外还校验雕刻区间的前缀字节与 oracle 一致 |
| 只读不变量 | 扫描前后整镜像 FNV-1a 比对必须不变 |
| 负向对照 | 全零「空白格式化卷」应报告 0 文件；另做源码级负向验证：临时关闭 `DeepScan` 的 RAW 回退后 6 项断言 FAIL，恢复源码后全绿 |
| 验证 | 22 项全绿（GCC 7.3 / GCC 14.2）。**RAW 共报告 59 个文件、仅 5 个为植入**，即约 13.5 FP/MiB，与 W5 的 14.38 FP/MiB 基线吻合——本门禁只保证召回率，不保证精确率 |

## RAW 雕刻误报率语料（W5 / CAR-002，运行时合成）

CTest `RawScanFalsePositive` 使用 8 MiB `std::mt19937` seed `12648430`（0xC0FFEE）随机噪声镜像，不落盘归档、每次运行重建；Case A 零植入、Case B 植入 16 个 512 对齐 header-only magic。实测与边界见 `docs/PERFORMANCE_BASELINE.md §9` 与 `docs/reports/rawscan_false_positive_2026-11-08.txt`。**合成语料，非真实卷误报率结论。**

---

## 引用

- **CFReDS**: https://cfreds-archive.nist.gov/
- **Digital Corpora**: https://digitalcorpora.org/ — Garfinkel et al., "Bringing Science to Digital Forensics with Standardized Forensic Corpora", DFRWS 2009
- **CFTT**: https://www.cftt.nist.gov/
- **DFRWS Challenges**: https://dfrws.org/forensic-challenges/
- **forensics-samples**: https://github.com/eribertomota/forensics-samples
- **Sleuth Kit**: https://github.com/sleuthkit/sleuthkit
- **APFS Specimens**: https://github.com/dfirlabs/apfs-specimens
- **DFTT**: https://dftt.sourceforge.net/
- **FILETYPES1**: https://downloads.digitalcorpora.org/corpora/files/filetypes1/ — Beebe, "FILETYPES1 File type samples", UT San Antonio, 2014
