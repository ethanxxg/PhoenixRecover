# P0-13 磁盘镜像：工具已就绪，当前被设备「未就绪」阻断

- 日期：2026-11-15 19:30–20:10
- 目标：为 `\\.\PhysicalDrive2`（ST1000DM003-1CH162，SN `0AD132601218`）建立镜像
- 结论：**镜像工具已实现并修正了 2 个真实缺陷；但盘当前对所有读请求返回
  `err=21 ERROR_NOT_READY`，无法镜像。需物理断电重上电后再试。**

## 一、空间约束（决定了不能整盘镜像）

| 位置 | 容量 | 可用 |
|---|---:|---:|
| C: | 951.6 GB | **661.2 GB** |
| D: | 953.9 GB | **137 GB** |

源盘 931.51 GiB，**整盘镜像放不下**（C+D 合计 798 GB）。按 NTFS 卷范围镜像才可行：

| 区域 | 起 LBA | 扇区数 | 大小 |
|---|---:|---:|---:|
| 卷 0（boot 63） | 63 | 209,728,511 | 100.0 GiB |
| 卷 1（boot 209,728,638） | 209,728,638 | 436,228,943 | 208.0 GiB |
| 合计 | | | **308 GiB**（C: 可容纳） |

另加 MBR(1 MiB)、LDM 数据库尾区(1 MiB)、卷 1 的 `$MFT` 五段(918 MiB)，
这些是元数据，优先级最高。

## 二、新增工具 `tools/image_region.cpp`

复用生产 `SectorReader`（GENERIC_READ only，不写源盘），按任意 LBA 区间镜像：

- **严格策略** `skipBadSectors = false`：大块读在**第一个**坏扇区就返回，
  而不是像 lenient 模式那样把整块每个扇区都磨一遍设备超时。
- 失败后以 **4 KiB** 为单位逐单元定位坏块，精确记录 `.unreadable`。
- 连续坏单元达到 `--max-streak` 则跳过一段（`--skip-mib` 起始，
  **按次翻倍**到 `--skip-max-mib`），避免在小坏带上过度跳过、又能在长坏带上快速穿越。
- 坏道密度判定：块内已见单元有 1/3 不可读即判为坏带，不再逐单元磨。
- 全部不可读区间写入 `<img>.map.json`（`.unreadable` 试过但失败、`.skipped` 直接跳过），
  并给出 `fullyRead` 与 `COVERAGE`。**`complete` 只代表走到区间末尾，
  不代表读全了**——早期版本正是把"一路跳过到终点"错报成 complete。
- `--resume` 按 `nextLba` 续跑；`--bad-budget` / `--time-budget` 兜底。

编译（不依赖 CMake 重配，避免 PowerShell 通道在 cmake 处中断）：

```
g++ -std=c++17 -O2 -DNDEBUG -DPHOENIX_BUILD_GUI=0 -I. \
    -o build-gcc14/tests/image_region.exe \
    tools/image_region.cpp disk/SectorReader_win.cpp \
    common/Utils.cpp log/Logger.cpp -lole32 -ladvapi32
```

## 三、过程中修掉的两个真实缺陷（都是我写的工具，不是引擎）

1. **缓冲未对齐**：`SectorReader::Open` 用
   `FILE_FLAG_NO_BUFFERING | FILE_FLAG_OVERLAPPED`，要求缓冲区与传输长度按设备
   扇区（4 KiB）对齐。最初用 `std::vector<uint8_t>`（仅 16 字节对齐）→ 每次
   `ReadFile` 直接失败，表现为**每个扇区都坏**。已改为手工 4096 对齐。
2. **512 字节单扇区读在 NO_BUFFERING 句柄上非法**：慢路径原本逐 512 字节读，
   在 4K 对齐设备上必然失败。已改为以 4 KiB（8 扇区）为最小单位。

这两点也解释了为什么"坏块"曾出现在 LBA 0——那是工具缺陷的假象，
与 P0-12 观测到的真实坏带（LBA 216,020,094 起，`err=121` 超时）是两回事。

## 四、当前真实阻断：设备未就绪

修复后重测，所有 LBA 建刻返回：

```
SectorReader: BAD SECTOR  LBA=0  err=21
READ FAIL lba=0 sectors=8 err=21
```

`err=21 = ERROR_NOT_READY`（设备未就绪），且**瞬时返回**（`elapsed=0`），不是超时。
三种独立方式互证：

| 方式 | 结果 |
|---|---|
| `image_region.exe`（引擎 SectorReader） | 每个 LBA `err=21` |
| 纯 .NET `[IO.File]::Open('\\.\PhysicalDrive2')` + `Read` | 抛「设备未就绪」 |
| `Win32_DiskDrive` | Index 2、ST1000DM003、SN 0AD132601218、931.5 GB、`Status OK` |

`Status OK` 是 WMI 缓存值，**不代表可读**。设备节点还在、句柄能打开，
但介质层不接受任何读命令。

## 五、已产出的镜像文件：全部作废

`C:\PhoenixImage\` 下的 `mbr.img` / `mft1.img` / `mft5.img` 虽然 rc=0，
但其 map 里 `cleanChunks = 0`、`skipped` 覆盖全区间——**内容是整片零，不是数据**。
产生于对齐缺陷 + 设备未就绪的叠加，不可用于任何恢复。`ldm_tail.img` 同理。
复查时应以 `.map.json` 的 `fullyRead` 为准，不要看 rc。

## 六、下一步（需人工断电）

1. 拔掉该 USB 盒的**电源与数据线**，静置 30 秒以上，再单独接回（不要与其他
   USB 存储共享 HUB，避免供电竞争）。
2. 上电后先做一次最小读验证（LBA 0，64 扇区），确认 `fullyRead=true`
   且 `mbrSig=55AA` 再进入正式镜像。
3. 正式镜像顺序（价值优先，盘随时可能彻底失效）：
   MBR → LDM 尾区 → 卷1 `$MFT` 五段 → 卷0 全卷(100 GiB) → 卷1 全卷(208 GiB)。
4. 目标目录建议 `C:\PhoenixImage\`（D: 仅 137 GB，不够卷1）。

若断电后仍返回 `err=21`，则该盘已进入不可用状态，只剩"拆盒接 SATA 直连"
或专业设备（PC-3000 级）两条路。
