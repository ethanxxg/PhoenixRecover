# PhoenixRecover API 参考手册

> 本文档根据各模块头文件自动提取，描述所有公开类、结构体、枚举及其方法签名。
>
> 最后更新：2026-05-26

---

## 目录

1. [Common — 通用工具与类型](#1-common--通用工具与类型)
2. [Disk — 磁盘与扇区访问](#2-disk--磁盘与扇区访问)
3. [FS — 文件系统解析器](#3-fs--文件系统解析器)
4. [Scan — 扫描引擎](#4-scan--扫描引擎)
5. [Recover — 恢复引擎](#5-recover--恢复引擎)
6. [Controllers — 控制器层（Qt UI 桥接）](#6-controllers--控制器层qt-ui-桥接)
7. [Media — 媒体缩略图](#7-media--媒体缩略图)
8. [Log — 日志系统](#8-log--日志系统)

---

## 1. Common — 通用工具与类型

### 1.1 `Utils` 命名空间

**头文件：** `common/Utils.h`

提供字符串编码转换、文件系统辅助、格式化、二进制读取等跨模块通用函数。

| 函数签名 | 说明 |
|---|---|
| `std::string WideToUtf8(const std::wstring& wide)` | 宽字符串转 UTF-8 |
| `std::wstring Utf8ToWide(const std::string& utf8)` | UTF-8 转宽字符串 |
| `std::wstring AnsiToWide(const char* str, int codePage = 0)` | ANSI 字符串转宽字符串，`codePage` 默认为 `CP_ACP` |
| `std::wstring TrimRight(std::wstring s)` | 去除尾部空白/空字符 |
| `std::string TrimRight(std::string s)` | 同上（`std::string` 版本） |
| `bool CreateDirectoryRecursive(const std::wstring& path)` | 递归创建目录 |
| `bool FileExists(const std::wstring& path)` | 检查文件是否存在 |
| `uint64_t GetFreeDiskSpace(const std::wstring& path)` | 获取指定路径所在磁盘的可用空间（字节） |
| `std::string FormatBytes(uint64_t bytes)` | 将字节数格式化为人类可读字符串（如 `"1.23 GB"`） |
| `std::string FormatFiletime(uint64_t ft100ns)` | 将 Windows FILETIME（100ns 精度）格式化为时间字符串 |
| `uint16_t ReadLE16(const void* p)` | 从内存读取 16 位小端整数（未对齐安全） |
| `uint32_t ReadLE32(const void* p)` | 从内存读取 32 位小端整数 |
| `uint64_t ReadLE64(const void* p)` | 从内存读取 64 位小端整数 |
| `int64_t ReadSignedLE(const uint8_t* p, int byteCount)` | 读取变长有符号小端整数（1–8 字节），用于 NTFS DataRun 偏移量 |
| `uint64_t AlignUp(uint64_t value, uint64_t alignment)` | 向上对齐至 `alignment` 的倍数 |
| `std::string HexDump(const uint8_t* data, size_t length, size_t maxBytes = 256)` | 十六进制转储（调试用） |
| `std::wstring ToHexW(uint32_t val, int minDigits = 4)` | 整数转十六进制宽字符串 |
| `uint64_t NowFileTime()` | 获取当前系统时间（FILETIME 格式） |

---

### 1.2 `Platform.h` — 平台宏

**头文件：** `common/Platform.h`

根据 Qt 平台宏定义跨平台编译标志。

| 宏 | 含义 |
|---|---|
| `PHOENIX_OS_WINDOWS` | `1` 表示 Windows 平台 |
| `PHOENIX_OS_LINUX_DESKTOP` | `1` 表示 Linux 桌面平台（排除 Android） |
| `PHOENIX_OS_MACOS` | `1` 表示 macOS 平台 |

---

### 1.3 `SysInfo` 结构体

**头文件：** `common/SysInfo.h`

收集本机系统信息，供 UI Header 条显示。

| 字段 | 类型 | 说明 |
|---|---|---|
| `osName` | `std::wstring` | 操作系统名称，如 `"Windows 10 Pro 64-bit"` |
| `isAdmin` | `bool` | 是否以管理员权限运行 |
| `cpuName` | `std::wstring` | CPU 名称 |
| `ramMB` | `uint32_t` | 物理内存（MB） |
| `gpuName` | `std::wstring` | GPU 名称 |

| 方法 | 说明 |
|---|---|
| `static SysInfo Collect()` | 一次性收集所有系统信息 |
| `std::wstring Line1() const` | 格式化为第一行（应用名称与版本） |
| `std::wstring Line2() const` | 格式化为第二行（操作系统与权限） |
| `std::wstring Line3() const` | 格式化为第三行（硬件概要） |

---

### 1.4 `Types.h` — 通用枚举与类型别名

**头文件：** `common/Types.h`

#### 类型别名

| 别名 | 原始类型 |
|---|---|
| `Byte` | `uint8_t` |
| `ByteBuffer` | `std::vector<uint8_t>` |

#### `ErrorCode` 枚举

| 值 | 含义 |
|---|---|
| `Success` | 成功 |
| `AccessDenied` | 需要管理员权限 |
| `DeviceNotFound` | 物理磁盘/卷未找到 |
| `PartitionNotRecognized` | 分区表无效 |
| `ReadError` | I/O 读取失败 |
| `BadSector` | 遇到坏扇区 |
| `InvalidFileSystem` | 文件系统签名无法识别 |
| `RecoverPathSameAsSrc` | 恢复目标路径与源分区相同 |
| `DiskFull` | 目标磁盘空间不足 |
| `FileWriteError` | 无法写入恢复文件 |
| `Cancelled` | 用户取消 |
| `SnapshotInvalid` | `.prs` 快照文件损坏 |
| `NotImplemented` | 功能占位符 |
| `InvalidParameter` | 参数无效 |
| `OutOfMemory` | 内存不足 |

辅助函数：`const char* ErrorCodeToStr(ErrorCode c)` — 返回错误码的英文描述。

#### `RecoverabilityLevel` 枚举

| 值 | 含义 |
|---|---|
| `Excellent` | 所有数据簇仍为空闲（最佳恢复机会） |
| `Good` | ≤10% 簇被覆盖 |
| `Poor` | 10%–50% 簇被覆盖 |
| `Lost` | >50% 簇被覆盖（恢复希望极低） |
| `Unknown` | 无法判定 |

辅助函数：`const char* RecoverabilityStr(RecoverabilityLevel r)` — 返回中文描述。

#### `FileStatus` 枚举

| 值 | 含义 |
|---|---|
| `Deleted` | 文件被显式删除 |
| `Lost` | 因格式化/分区损坏丢失 |
| `Normal` | 文件系统完好，文件仍可访问 |

#### `ScanMode` 枚举

| 值 | 含义 |
|---|---|
| `Quick` | 解析 FS 元数据（MFT/FAT 目录项）获取已删除记录 |
| `Deep` | 逐扇区扫描，重建文件系统结构 |
| `Raw` | 文件签名（文件雕刻）扫描 |

辅助函数：`const char* ScanModeStr(ScanMode m)` — 返回扫描模式英文名称。

---

## 2. Disk — 磁盘与扇区访问

### 2.1 `SectorReader` 类 ⭐重点

**头文件：** `disk/SectorReader.h`

**职责：** 物理磁盘、逻辑卷或磁盘镜像的只读扇区级 I/O 接口。对物理磁盘/卷使用 `FILE_FLAG_OVERLAPPED` 实现超时控制；坏扇区可按策略跳过并零填充。

**不可拷贝，不可赋值。**

#### `ReadPolicy` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `maxRetries` | `uint32_t` | `3` | 单个扇区最大重试次数 |
| `timeoutMs` | `uint32_t` | `8000` | 单次读取超时（毫秒），`0` = 同步阻塞 |
| `retryDelayMs` | `uint32_t` | `50` | 重试间隔（毫秒） |
| `skipBadSectors` | `bool` | `true` | 坏扇区零填充后继续（`true`）或立即失败（`false`） |

#### 类型别名

```cpp
using SectorReadCallback = std::function<void(uint64_t sectorsRead, uint64_t totalSectors)>;
```

#### 公开方法

| 方法签名 | 说明 |
|---|---|
| `SectorReader()` | 构造函数 |
| `~SectorReader()` | 析构函数，自动关闭句柄 |
| **打开** | |
| `bool Open(int diskIndex)` | 打开 `\\.\PhysicalDriveN`，需要管理员权限。`diskIndex` — 物理磁盘编号 |
| `bool OpenImage(const std::wstring& imagePath)` | 打开磁盘镜像文件（`.img` / `.dd`） |
| `bool OpenVolume(const std::wstring& driveLetter)` | 打开逻辑卷（如 `L"D:"`） |
| `void Close()` | 关闭设备句柄 |
| **读取** | |
| `bool ReadSectors(uint64_t sectorIndex, void* buffer, uint32_t sectorCount = 1)` | 读取连续扇区。`sectorIndex` — 起始扇区 LBA，`buffer` — 接收缓冲区（≥ `sectorCount × GetSectorSize()` 字节），`sectorCount` — 扇区数。`skipBadSectors=true` 时始终返回 `true`（坏扇区零填充），否则首个坏扇区即返回 `false` |
| `bool ReadBytes(uint64_t byteOffset, void* buffer, uint32_t length)` | 读取任意字节范围，内部自动对齐到扇区边界。`byteOffset` — 起始字节偏移，`length` — 读取字节数 |
| **策略与统计** | |
| `void SetReadPolicy(const ReadPolicy& p)` | 设置读取策略 |
| `const ReadPolicy& GetReadPolicy() const` | 获取当前读取策略 |
| `const BadSectorMap& GetBadSectors() const` | 获取坏扇区映射表 |
| `void ClearBadSectors()` | 清空坏扇区记录 |
| `uint64_t GetBadSectorCount() const` | 获取坏扇区总数 |
| `uint64_t GetSectorsRead() const` | 已读取的扇区总数（包括零填充的坏扇区） |
| `uint64_t GetRetryCount() const` | 所有读取的总重试次数 |
| **设备信息** | |
| `bool IsOpen() const` | 设备是否已打开 |
| `uint32_t GetSectorSize() const` | 扇区大小（字节，通常为 512） |
| `uint64_t GetTotalSectors() const` | 总扇区数 |
| `uint64_t GetTotalBytes() const` | 总字节数（`totalSectors × sectorSize`） |
| `DWORD GetLastWinError() const` | 最后一次 Windows 错误码 |
| `void SetSectorReadCallback(SectorReadCallback cb)` | 设置扇区读取进度回调 |
| `void SetCancelFlag(std::atomic<bool>* flag)` | 设置外部取消标志，用于长任务扫描的协同取消 |

---

### 2.2 `BadSectorMap` 类

**头文件：** `disk/BadSectorMap.h`

**职责：** 使用有序范围列表紧凑地追踪坏扇区 LBA，自动合并相邻/重叠区间。非线程安全。

#### `BadSectorMap::Range` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `startLba` | `uint64_t` | 首个坏扇区 LBA（含） |
| `endLba` | `uint64_t` | 最后一个坏扇区 LBA（含） |

方法：`uint64_t Count() const` — 该范围内的坏扇区数。

#### 公开方法

| 方法 | 说明 |
|---|---|
| `void MarkBad(uint64_t lba)` | 标记单个坏扇区 |
| `void MarkBadRange(uint64_t startLba, uint64_t count)` | 标记连续坏扇区范围 |
| `bool IsBad(uint64_t lba) const` | 查询某扇区是否为坏扇区 |
| `uint64_t Count() const` | 坏扇区总数 |
| `bool IsEmpty() const` | 是否无坏扇区 |
| `size_t RangeCount() const` | 坏扇区范围段数 |
| `const std::vector<Range>& Ranges() const` | 获取所有范围 |
| `void Clear()` | 清空所有记录 |

---

### 2.3 `Partition` 结构体

**头文件：** `disk/Partition.h`

描述一个磁盘分区。

| 字段 | 类型 | 说明 |
|---|---|---|
| `index` | `int` | 分区索引 |
| `physicalDiskIdx` | `int` | 所属物理磁盘编号 |
| `startSector` | `uint64_t` | 在物理磁盘上的绝对起始扇区 |
| `totalSectors` | `uint64_t` | 总扇区数 |
| `sizeBytes` | `uint64_t` | 分区大小（字节） |
| `fsType` | `std::wstring` | 文件系统类型（`"NTFS"` / `"FAT32"` / `"exFAT"` / `"ext4"` 等） |
| `driveLetter` | `std::wstring` | 盘符（如 `"C:"`），未挂载时为空 |
| `label` | `std::wstring` | 卷标 |
| `isActive` | `bool` | MBR 活动分区标志 |
| `isSystem` | `bool` | 系统分区（EFI） |

方法：`std::wstring DisplayName() const` — 生成显示名称。

---

### 2.4 `PhysicalDisk` 结构体

**头文件：** `disk/PhysicalDisk.h`

描述一块物理磁盘。

| 字段 | 类型 | 说明 |
|---|---|---|
| `index` | `int` | `\\.\PhysicalDriveN` 编号 |
| `devicePath` | `std::wstring` | 设备路径 |
| `model` | `std::wstring` | 型号 |
| `serialNumber` | `std::wstring` | 序列号 |
| `interfaceType` | `std::wstring` | 接口类型（`"SATA"` / `"NVMe"` / `"USB"` / `"Unknown"`） |
| `totalSectors` | `uint64_t` | 总扇区数 |
| `sectorSize` | `uint32_t` | 扇区大小 |
| `sizeBytes` | `uint64_t` | 总大小（字节） |
| `isGPT` | `bool` | 是否 GPT 分区表 |
| `isRemovable` | `bool` | 是否可移动设备 |
| `partitions` | `std::vector<Partition>` | 分区列表 |
| `smart` | `SmartHealth` | S.M.A.R.T. 健康数据 |

方法：`std::wstring DisplayName() const` — 生成显示名称。

#### `SmartHealth` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `available` | `bool` | SMART 是否可用 |
| `predictFailure` | `bool` | 是否预报即将故障 |
| `reallocSectors` | `uint32_t` | 重映射扇区计数 |
| `pendingSectors` | `uint32_t` | 待定扇区计数 |
| `uncorrectable` | `uint32_t` | 不可修正扇区计数 |
| `powerOnHours` | `uint32_t` | 通电时间（小时） |
| `temperature` | `uint32_t` | 温度（°C） |
| `statusText` | `std::wstring` | 健康状态文本（`"良好"` / `"注意"` / `"危险"` / `"未知"`） |
| `healthScore` | `int` | 健康评分（0–100），-1 表示未知 |
| `detail` | `std::wstring` | 详细说明 |

---

### 2.5 `DiskManager` 类

**头文件：** `disk/DiskManager.h`

**职责：** 枚举系统物理磁盘、加载镜像文件。全部为静态方法。

| 方法 | 说明 |
|---|---|
| `static std::vector<PhysicalDisk> GetDisks()` | 枚举所有物理磁盘（需管理员权限），含分区列表与 SMART 数据 |
| `static bool LoadImageFile(const std::wstring& imagePath, PhysicalDisk& outDisk)` | 将磁盘镜像文件作为单分区"磁盘"加载 |

---

### 2.6 `PartitionProbe` 类

**头文件：** `disk/PartitionProbe.h`

**职责：** 在物理磁盘上探测候选分区（MBR/GPT 表 + 文件系统签名扫描）。

#### `PartitionCandidate` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `startSector` | `uint64_t` | 候选分区起始扇区 |
| `totalSectors` | `uint64_t` | 总扇区数 |
| `sizeBytes` | `uint64_t` | 大小（字节） |
| `fsType` | `std::wstring` | 文件系统类型 |
| `source` | `std::wstring` | 发现来源（`"MBR"` / `"GPT"` / `"Signature"`） |
| `confidence` | `int` | 置信度（0–100） |

#### `PartitionProbe::Options` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `signatureStrideSectors` | `uint64_t` | `2048` | 签名扫描步进扇区数 |
| `scanStartSector` | `uint64_t` | `0` | 扫描起始扇区 |
| `scanEndSector` | `uint64_t` | `0` | 扫描结束扇区（`0` = 磁盘末尾） |
| `includeSignatureScan` | `bool` | `true` | 是否包含签名扫描 |

#### 公开静态方法

| 方法 | 说明 |
|---|---|
| `static std::vector<PartitionCandidate> ProbePhysicalDisk(int physicalDiskIdx, const Options& opt, const std::atomic<bool>* cancelFlag, std::function<void(int percent)> onProgress)` | 探测物理磁盘上的候选分区 |
| `static Partition ToPartition(const PartitionCandidate& c, int physicalDiskIdx)` | 将候选分区转换为 `Partition` 结构 |

---

### 2.7 `Imager` 类

**头文件：** `disk/Imager.h`

**职责：** 逐扇区创建物理磁盘的原始镜像文件（`.img`）。

#### `ImageProgress` / `ImageResult` 结构体

| 结构体 | 字段 | 类型 | 说明 |
|---|---|---|---|
| `ImageProgress` | `totalBytes` | `uint64_t` | 总字节数 |
| | `doneBytes` | `uint64_t` | 已完成字节数 |
| | `badBytes` | `uint64_t` | 坏扇区字节数 |
| `ImageResult` | `ok` | `bool` | 是否成功 |
| | `cancelled` | `bool` | 是否被取消 |
| | `totalBytes` | `uint64_t` | 总字节数 |
| | `writtenBytes` | `uint64_t` | 已写入字节数 |
| | `badBytes` | `uint64_t` | 坏扇区字节数 |
| | `lastError` | `DWORD` | 最后 Windows 错误码 |
| | `message` | `std::wstring` | 结果消息 |

#### 公开静态方法

| 方法 | 说明 |
|---|---|
| `static ImageResult CreatePhysicalDiskImage(int srcDiskIndex, const std::wstring& outImgPath, uint32_t blockBytes, const std::atomic<bool>* cancelFlag, std::function<void(const ImageProgress&)> onProgress)` | 创建物理磁盘镜像。`srcDiskIndex` — 源磁盘编号，`blockBytes` — 顺序读取块大小（如 4 MiB） |

---

### 2.8 `SmartInfo` / `SmartReader`

**头文件：** `disk/SmartInfo.h`

#### `SmartAttribute` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `uint8_t` | 属性 ID |
| `name` | `std::wstring` | 属性名称 |
| `current` | `uint8_t` | 当前值（0–253） |
| `worst` | `uint8_t` | 历史最差值 |
| `threshold` | `uint8_t` | 故障阈值 |
| `rawValue` | `uint64_t` | 原始数据 |
| `preFail` | `bool` | 是否为预故障属性 |
| `belowThreshold` | `bool` | 当前值是否低于阈值（正在失败） |

#### `DiskHealthStatus` 枚举

| 值 | 含义 |
|---|---|
| `Unknown` | 无法读取 SMART 数据 |
| `Good` | 所有属性正常 |
| `Warning` | 部分属性退化但非致命 |
| `Critical` | 一个或多个属性低于阈值 |
| `Failing` | 即将故障 |

#### `SmartInfo` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `diskIndex` | `int` | 磁盘编号 |
| `model` / `serial` / `firmware` | `std::wstring` | 型号 / 序列号 / 固件版本 |
| `smartSupported` / `smartEnabled` | `bool` | SMART 是否支持/启用 |
| `health` | `DiskHealthStatus` | 总体健康状态 |
| `temperatureC` / `powerOnHours` / `powerCycleCount` | `uint32_t` | 温度 / 通电时长 / 上电次数 |
| `reallocatedSectors` / `pendingSectors` / `uncorrectableSectors` | `uint32_t` | 关键扇区计数 |
| `attributes` | `std::vector<SmartAttribute>` | 全部 SMART 属性列表 |

#### `SmartReader` 类

| 方法 | 说明 |
|---|---|
| `static SmartInfo ReadDiskSmart(int diskIndex)` | 读取指定磁盘的 SMART 信息 |
| `static std::vector<SmartInfo> ReadAllDisks()` | 读取所有物理磁盘的 SMART 信息 |
| `static std::wstring HealthStatusText(DiskHealthStatus status)` | 获取健康状态文本 |
| `static uint32_t HealthStatusColor(DiskHealthStatus status)` | 获取健康状态对应的 UI 颜色 |

---

## 3. FS — 文件系统解析器

### 3.1 `FileSystem` 抽象基类

**头文件：** `fs/FileSystem.h`

**职责：** 所有文件系统解析器的公共接口。

```cpp
using FileFoundCallback = std::function<void(const FileNode&)>;
```

| 虚方法 | 说明 |
|---|---|
| `virtual bool Open(SectorReader& reader, const Partition& partition) = 0` | 绑定读取器与分区，执行引导扇区验证。返回 `false` 表示该解析器不适用 |
| `virtual bool ScanMetadata(std::function<void(int)> onProgress, std::atomic<bool>* cancelFlag) = 0` | 读取文件系统元数据并构建文件列表。`onProgress` 回调传递 0–100 进度 |
| `virtual std::vector<FileNode> GetAllFiles() const = 0` | 返回所有已发现的文件节点 |
| `virtual std::wstring GetFsTypeName() const = 0` | 返回文件系统名称（如 `"NTFS"`） |
| `virtual bool IsValid() const = 0` | `Open()` 成功后返回 `true` |
| `virtual uint32_t GetCurrentFileCount() const` | 实时已发现文件计数（默认返回 0） |
| `virtual uint32_t GetTotalRecordCount() const` | 已访问的总记录数（默认返回 0） |
| `virtual uint32_t GetInUseCount() const` | 在用记录数（默认返回 0） |
| `virtual void SetFileFoundCallback(FileFoundCallback cb)` | 设置实时文件发现回调 |

---

### 3.2 `FatParser` 类 ⭐重点

**头文件：** `fs/fat/FatParser.h`

**继承：** `FileSystem`

**职责：** FAT12 / FAT16 / FAT32 卷的文件系统解析器。支持 LFN 长文件名、已删除文件检测、孤立目录项扫描。

#### 公开方法（覆写 FileSystem）

| 方法 | 说明 |
|---|---|
| `bool Open(SectorReader& reader, const Partition& partition)` | 读取 BPB 并验证 FAT 卷。`reader` — 扇区读取器，`partition` — 目标分区 |
| `bool ScanMetadata(std::function<void(int)> onProgress, std::atomic<bool>* cancelFlag)` | 遍历 FAT 目录树 + 孤立目录项扫描，实时上报发现的文件 |
| `std::vector<FileNode> GetAllFiles() const` | 返回所有文件节点 |
| `std::wstring GetFsTypeName() const` | 返回 `"FAT12"` / `"FAT16"` / `"FAT32"` |
| `bool IsValid() const` | 卷是否有效 |
| `uint32_t GetCurrentFileCount() const` | 当前文件计数 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |

#### `FatStructs.h` — FAT 磁盘结构

| 结构体 | 说明 |
|---|---|
| `FAT_BPB_COMMON` | FAT 通用 BPB（BIOS Parameter Block） |
| `FAT32_BPB` | FAT32 扩展 BPB（继承 `FAT_BPB_COMMON`） |
| `FAT16_BPB` | FAT12/16 扩展 BPB（继承 `FAT_BPB_COMMON`） |
| `FAT_DIR_ENTRY` | 8.3 短名目录项（`name[0]==0xE5` 表示已删除） |
| `FAT_LFN_ENTRY` | 长文件名目录项 |

**关键常量：**

| 常量 | 值 | 说明 |
|---|---|---|
| `FAT_ATTR_READ_ONLY` | `0x01` | 只读 |
| `FAT_ATTR_HIDDEN` | `0x02` | 隐藏 |
| `FAT_ATTR_SYSTEM` | `0x04` | 系统 |
| `FAT_ATTR_VOLUME_ID` | `0x08` | 卷标 |
| `FAT_ATTR_DIRECTORY` | `0x10` | 目录 |
| `FAT_ATTR_ARCHIVE` | `0x20` | 归档 |
| `FAT_ATTR_LFN` | `0x0F` | LFN 标识 |
| `FAT_DELETED_MARKER` | `0xE5` | 已删除标记 |
| `FAT32_EOC_MIN` | `0x0FFFFFF8` | FAT32 簇链结束标记最小值 |
| `FAT32_BAD` | `0x0FFFFFF7` | FAT32 坏簇标记 |

辅助函数：`uint64_t FatDateTimeToFiletime(uint16_t date, uint16_t time, uint8_t timeTenth = 0)` — FAT 日期时间转 Windows FILETIME。

---

### 3.3 `NtfsParser` 类 ⭐重点

**头文件：** `fs/ntfs/NtfsParser.h`

**继承：** `FileSystem`

**职责：** NTFS 卷的文件系统解析器。遍历 MFT 记录（含碎片化 MFT），实时回调文件发现，后续完善路径解析。支持回收站元数据解析和 USN Journal 扫描。

#### 扫描流程

1. `Open()` — 验证引导扇区，计算卷几何信息
2. `ScanMetadata()` — 读取每条 MFT 记录，构建 `m_recordMap`，实时回调发现的文件/目录，最后 `BuildFullPaths()` 完善部分路径
3. `GetAllFiles()` — 返回完整文件节点列表

#### 公开方法

| 方法 | 说明 |
|---|---|
| `bool Open(SectorReader& reader, const Partition& partition)` | 验证 NTFS 引导扇区。`reader` — 扇区读取器，`partition` — 目标分区 |
| `bool ScanMetadata(std::function<void(int)> onProgress, std::atomic<bool>* cancelFlag)` | 遍历所有 MFT 记录，构建文件列表。支持进度回调和取消 |
| `std::vector<FileNode> GetAllFiles() const` | 返回所有文件节点 |
| `std::wstring GetFsTypeName() const` | 返回 `"NTFS"` |
| `bool IsValid() const` | 卷是否有效 |
| `uint32_t GetCurrentFileCount() const` | 实时文件计数 |
| `uint32_t GetTotalRecordCount() const` | 已访问的有效 MFT 记录总数（排除系统记录 0–15） |
| `uint32_t GetInUseCount() const` | 在用记录数 |
| `static std::vector<uint64_t> DecodeDataRuns(const uint8_t* dataRunPtr, size_t maxBytes)` | 将 DataRun 列表解码为绝对 LCN 序列（每簇一个元素）。`dataRunPtr` — DataRun 起始指针，`maxBytes` — 最大读取字节数。返回空向量表示出错 |
| `RecoverabilityLevel AssessRecoverability(const FileNode& node)` | 通过 `$Bitmap` 评估文件可恢复性 |
| `uint32_t ScanUsnJournal(std::function<void(int)> onProgress, const std::atomic<bool>* cancelFlag)` | Phase-2：扫描 NTFS 变更日志（$UsnJrnl），发现 MFT 已被重用的已删除文件。必须在 `ScanMetadata()` 之后调用。返回新增文件数，若卷无盘符则返回 0 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |

#### `NtfsStructs.h` — NTFS 磁盘结构

| 结构体 | 说明 |
|---|---|
| `NTFS_BOOT_SECTOR` | NTFS 引导扇区（VBR） |
| `MFT_RECORD_HEADER` | MFT 记录头（magic = `"FILE"`） |
| `ATTR_HEADER_COMMON` | 属性头公共部分 |
| `ATTR_RESIDENT` | 驻留属性体 |
| `ATTR_NONRESIDENT` | 非驻留属性体（含 DataRun） |
| `FILE_NAME_ATTR` | `$FILE_NAME` 属性值 |
| `STANDARD_INFORMATION` | `$STANDARD_INFORMATION` 属性值 |

**MFT 引用辅助函数：**

| 函数 | 说明 |
|---|---|
| `uint64_t MftRefRecordNumber(uint64_t ref)` | 提取 48 位记录号 |
| `uint16_t MftRefSequenceNum(uint64_t ref)` | 提取 16 位序列号 |

---

### 3.4 `UsnJournalParser` 类

**头文件：** `fs/ntfs/UsnJournalParser.h`

**职责：** 通过 Windows FSCTL API 读取 NTFS $UsnJrnl:$J 数据流，按 FRN 去重产生变更记录。

#### `UsnRecord` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `fileFrn` | `uint64_t` | 文件引用号（MFT 记录索引，低 48 位） |
| `parentFrn` | `uint64_t` | 父目录 FRN |
| `name` | `std::wstring` | 文件名 |
| `timestamp` | `int64_t` | 时间戳（Windows FILETIME） |
| `allReasons` | `uint32_t` | 该 FRN 所有记录 reason 标志的 OR |
| `lastReason` | `uint32_t` | 最新记录的 reason 标志 |
| `fileAttributes` | `uint32_t` | 最新记录的文件属性 |
| `usn` | `int64_t` | 最新 USN |

#### `UsnReason` 命名空间 — USN 原因标志

| 常量 | 值 | 含义 |
|---|---|---|
| `FILE_CREATE` | `0x00000100` | 文件创建 |
| `FILE_DELETE` | `0x00000200` | 文件删除 |
| `RENAME_OLD_NAME` | `0x00001000` | 重命名（旧名） |
| `RENAME_NEW_NAME` | `0x00002000` | 重命名（新名） |
| `DATA_OVERWRITE` | `0x00000001` | 数据覆写 |
| `DATA_EXTEND` | `0x00000002` | 数据扩展 |
| `DATA_TRUNCATION` | `0x00000004` | 数据截断 |
| `CLOSE` | `0x80000000` | 文件关闭 |
| *其他* | — | `SECURITY_CHANGE` / `COMPRESSION_CHANGE` / `ENCRYPTION_CHANGE` 等 |

#### 公开方法

| 方法 | 说明 |
|---|---|
| `bool Scan(const std::wstring& volumePath, std::vector<UsnRecord>& outRecords, std::function<void(uint64_t, uint64_t)> onProgress, const std::atomic<bool>* cancelFlag)` | 扫描 USN 日志。`volumePath` — 卷路径（如 `L"\\\\.\\C:"`），`outRecords` — 每个唯一 FRN 输出一条记录 |
| `uint64_t GetRawRecordsRead() const` | 原始记录读取总数 |
| `uint64_t GetUniqueFrns() const` | 去重后 FRN 数 |
| `std::wstring GetLastError() const` | 最后错误信息 |
| `uint64_t GetJournalId() const` | 日志 ID |
| `uint64_t GetFirstUsn() const` | 起始 USN |
| `uint64_t GetNextUsn() const` | 下一条 USN |
| `uint64_t GetMaxSizeBytes() const` | 日志最大大小 |

---

### 3.5 `Ext4Parser` 类 ⭐重点

**头文件：** `fs/ext4/Ext4Parser.h`

**继承：** `FileSystem`

**职责：** ext4 文件系统解析器。支持 64 位块地址、extent 树、间接块映射、已删除 inode 检测。

#### 公开方法

| 方法 | 说明 |
|---|---|
| `bool Open(SectorReader& reader, const Partition& partition)` | 读取 superblock 并验证 ext4 卷 |
| `bool ScanMetadata(std::function<void(int)> onProgress, std::atomic<bool>* cancelFlag)` | 遍历 inode 表和目录树，构建文件列表 |
| `std::vector<FileNode> GetAllFiles() const` | 返回所有文件节点 |
| `std::wstring GetFsTypeName() const` | 返回 `"ext4"` |
| `bool IsValid() const` | 卷是否有效 |
| `uint32_t GetCurrentFileCount() const` | 当前文件计数 |
| `uint32_t GetTotalRecordCount() const` | 总 inode 记录数 |
| `uint32_t GetInUseCount() const` | 在用 inode 数 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |

#### `Ext4Structs.h` — ext4 磁盘结构

**主要结构体：**

| 结构体 | 大小 | 说明 |
|---|---|---|
| `EXT4_SUPERBLOCK` | 1024 B | Superblock（位于偏移 1024） |
| `EXT4_GROUP_DESC` | 64 B | 块组描述符 |
| `EXT4_INODE` | ≥128 B | Inode 结构 |
| `EXT4_EXTENT_HEADER` | — | Extent 树头 |
| `EXT4_EXTENT` | — | Extent 叶节点 |
| `EXT4_EXTENT_IDX` | — | Extent 索引节点 |
| `EXT4_DIR_ENTRY_2` | — | 目录项 |

**关键常量：**

| 常量 | 值 | 说明 |
|---|---|---|
| `EXT4_SUPER_MAGIC` | `0xEF53` | Superblock 魔数 |
| `EXT4_FEATURE_INCOMPAT_EXTENTS` | `0x0040` | 使用 extent 特性 |
| `EXT4_FEATURE_INCOMPAT_64BIT` | `0x0080` | 64 位支持 |
| `EXT4_S_IFREG` | `0x8000` | 普通文件 |
| `EXT4_S_IFDIR` | `0x4000` | 目录 |
| `EXT4_EXT_MAGIC` | `0xF30A` | Extent header 魔数 |
| `EXT4_ROOT_INO` | `2` | 根目录 inode 号 |
| `EXT4_FT_REG_FILE` / `EXT4_FT_DIR` | `1` / `2` | 目录项文件类型 |

---

### 3.6 `ExfatParser` 类 ⭐重点

**头文件：** `fs/exfat/ExfatParser.h`

**继承：** `FileSystem`

**职责：** exFAT 文件系统解析器。支持无 FAT 链（连续文件）、分配位图检查。

#### 公开方法

| 方法 | 说明 |
|---|---|
| `bool Open(SectorReader& reader, const Partition& partition)` | 读取 exFAT 引导扇区并验证 |
| `bool ScanMetadata(std::function<void(int)> onProgress, std::atomic<bool>* cancelFlag)` | 遍历 exFAT 目录树，构建文件列表 |
| `std::vector<FileNode> GetAllFiles() const` | 返回所有文件节点 |
| `std::wstring GetFsTypeName() const` | 返回 `"exFAT"` |
| `bool IsValid() const` | 卷是否有效 |
| `uint32_t GetCurrentFileCount() const` | 当前文件计数 |
| `uint32_t GetTotalRecordCount() const` | 总记录数 |
| `uint32_t GetInUseCount() const` | 在用记录数 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |

#### `ExfatStructs.h` — exFAT 磁盘结构

| 结构体 | 说明 |
|---|---|
| `EXFAT_BOOT_SECTOR` | exFAT 引导扇区 |
| `EXFAT_DIR_ENTRY` | 通用 32 字节目录项 |
| `EXFAT_FILE_ENTRY` (0x85) | 文件/目录目录项 |
| `EXFAT_STREAM_EXT` (0xC0) | 流扩展目录项（含首簇、数据长度） |
| `EXFAT_FILENAME_EXT` (0xC1) | 文件名扩展目录项（每条最多 15 个 Unicode 字符） |

**目录项类型常量：**

| 常量 | 值 | 说明 |
|---|---|---|
| `EXFAT_ENTRY_EOD` | `0x00` | 目录结束 |
| `EXFAT_ENTRY_ALLOC_BMP` | `0x81` | 分配位图 |
| `EXFAT_ENTRY_UPCASE` | `0x82` | 大写转换表 |
| `EXFAT_ENTRY_VOLUME_LABEL` | `0x83` | 卷标 |
| `EXFAT_ENTRY_FILE` | `0x85` | 文件/目录 |
| `EXFAT_ENTRY_STREAM_EXT` | `0xC0` | 流扩展 |
| `EXFAT_ENTRY_FILE_NAME` | `0xC1` | 文件名扩展 |

---

## 4. Scan — 扫描引擎

### 4.1 `ScanEngine` 抽象基类 ⭐重点

**头文件：** `scan/ScanEngine.h`

**职责：** 所有扫描引擎的基类，提供回调管理、取消机制和进度上报。

#### 关联类型

```cpp
struct ScanProgress {
    uint64_t     scannedBytes;   // 已扫描字节
    uint64_t     totalBytes;     // 总字节
    uint32_t     foundCount;     // 已发现文件数
    int          percent;        // 0–100
    double       speedMBps;      // 扫描速度（MB/s）
    std::wstring stageLabel;     // 阶段标签
};

struct ScanResult {
    std::vector<FileNode>              files;       // 扫描到的文件列表
    uint64_t                           scannedBytes;
    uint32_t                           foundCount;  // 已删除/丢失文件数
    uint32_t                           totalRecords;// 已解析的总记录数
    uint32_t                           inUseCount;  // 在用记录数
    bool                               cancelled;
    std::wstring                       errorMsg;
    std::chrono::milliseconds          duration;    // 扫描耗时
};

using ProgressCallback  = std::function<void(const ScanProgress&)>;
using FileFoundCallback = std::function<void(const FileNode&)>;
using CancelCallback    = std::function<bool()>;  // 返回 true 则取消
```

#### 公开方法

| 方法 | 说明 |
|---|---|
| `void SetProgressCallback(ProgressCallback cb)` | 设置进度回调 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |
| `void SetCancelCallback(CancelCallback cb)` | 设置取消回调 |
| `virtual ScanResult Run(SectorReader& reader, const Partition& partition) = 0` | 同步执行扫描，阻塞直到完成或取消。`reader` — 扇区读取器，`partition` — 目标分区 |
| `void Cancel()` | 从其他线程请求取消 |

#### 受保护方法

| 方法 | 说明 |
|---|---|
| `bool IsCancelled() const` | 检查是否已取消 |
| `void ReportProgress(uint64_t scanned, uint64_t total, uint32_t found)` | 上报进度 |
| `void ReportFile(const FileNode& node)` | 上报发现的文件 |

---

### 4.2 `QuickScan` 类

**头文件：** `scan/QuickScan.h`

**继承：** `ScanEngine`

**职责：** 兼容层，内部委托给 `VolumeSectorScanner` 执行快速扫描。

| 方法 | 说明 |
|---|---|
| `void SetExternalCancelFlag(std::atomic<bool>* flag)` | 设置外部取消标志（如来自 GUI 主窗口） |
| `ScanResult Run(SectorReader& reader, const Partition& partition)` | 执行快速扫描 |

---

### 4.3 `DeepScan` 类 ⭐重点

**头文件：** `scan/DeepScan.h`

**继承：** `ScanEngine`

**职责：** 逐扇区扫描，在损坏或已格式化的卷上查找有效 MFT 记录和 FAT 目录项。适用于严重损坏的卷。

#### `FatDeepInfo` 结构体

| 字段 | 类型 | 说明 |
|---|---|---|
| `valid` | `bool` | BPB 是否有效 |
| `bytesPerSector` | `uint32_t` | 扇区字节数 |
| `sectorsPerCluster` | `uint32_t` | 每簇扇区数 |
| `partStartSector` | `uint64_t` | 分区绝对起始扇区 |
| `dataStartSector` | `uint64_t` | 数据区（簇 2）绝对起始扇区 |
| `fatType` | `uint8_t` | FAT 类型（12 / 16 / 32） |

#### 公开方法

| 方法 | 说明 |
|---|---|
| `ScanResult Run(SectorReader& reader, const Partition& partition)` | 执行深度扫描。逐扇区检测 MFT 记录（NTFS）和 FAT 目录页 |

---

### 4.4 `RawScan` 类

**头文件：** `scan/RawScan.h`

**继承：** `ScanEngine`

**职责：** 文件签名（文件雕刻）扫描。不依赖任何文件系统结构，适用于 RAW 分区或最后手段恢复。

| 方法 | 说明 |
|---|---|
| `explicit RawScan(SignatureDB* db = nullptr)` | 构造函数。可注入自定义签名数据库，否则使用内置数据库 |
| `void SetThreadCount(size_t n)` / `size_t GetThreadCount() const` | **A1-10**：设置 / 读取 2 参 `Run()` 使用的工作线程数。默认 `1`（串行，保持接线前的行为零变化）；`0` 归一化为 `1`；`>1` 时 2 参 `Run()` 委派给并行重载 |
| `ScanResult Run(SectorReader& reader, const Partition& partition)` | 执行 RAW 签名扫描。默认串行；若事先 `SetThreadCount(n>1)` 则自动走并行路径。文件 / 进度回调始终在调用线程发射 |
| `ScanResult Run(SectorReader& reader, const Partition& partition, size_t numThreads)` | **A1-8/A1-10**：并行重载。把 `[0, scanSectors)` 切成 N 个连续块，每个 worker 持有**独立 `SectorReader`**（由 `GetImagePath()` 克隆）与**独立 `SignatureDB`**（按 `m_db->GetAll()` 快照精确构造，非内建全集）；块间 512 B 重叠由主线程按 `rawOffset` 去重吸收；`RescanNestedGraphicHeaders` / `AddFragmentNodes` 只在合并后跑一次。`numThreads` 硬上限 64；非镜像源（`GetImagePath()` 为空）自动回退单线程。输出文件集与串行一致（CTest `RawScanParallel`） |

**生产配置链（A1-10）**：`UnifiedScanConfig.rawScanThreads`（默认 1）→ inline `MakeCarveConfig()` → `CarveModuleService::CarveConfig.numThreads`（默认 1；`0`=auto=`min(32, 硬件并发)`，`N` 亦夹紧到该上限）→ `RawScan::SetThreadCount()`。`RecoveryScanWorker` 从 `QSettings` 键 `recover/rawScanThreads` 读取（默认 1，无 UI 控件），因此默认行为与接线前完全相同。`CarveModuleService` 的 `targetExtensions` 于 A1-10 修复为**真正过滤**（此前 `filteredDb` 由默认 ctor 构造后仅追加命中项，实际含全部内建签名）。

---

### 4.5 `SignatureDB` 类 ⭐重点

**头文件：** `scan/SignatureDB.h`

**职责：** 文件类型签名数据库，用于 RAW 文件雕刻。支持主/次模式匹配、页脚标记、大小限制。

#### `FileSignature` 结构体

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `extension` | `std::wstring` | — | 文件扩展名（如 `L"jpg"`） |
| `header` | `std::vector<uint8_t>` | — | 主魔数字节序列 |
| `headerOffset` | `int` | `0` | 主魔数在文件中的偏移 |
| `footer` | `std::vector<uint8_t>` | — | 可选的文件结尾标记 |
| `maxSize` | `uint64_t` | `50 MB` | 该类型文件的最大尺寸 |
| `secondary` | `std::vector<uint8_t>` | — | 可选次级模式（用于子类型区分，如 RIFF→WAV/AVI/WebP） |
| `secondaryOffset` | `int` | `-1` | 次级模式偏移，`-1` 表示无次级检查 |

#### 公开方法

| 方法 | 说明 |
|---|---|
| `SignatureDB()` | 构造函数，加载内置签名（运行时实测 513 条） |
| `explicit SignatureDB(const std::vector<FileSignature>& sigs)` | **A1-10**：构造**精确签名集**数据库，只含 `sigs`、**不加载内建签名**。供 `CarveModuleService` 的扩展名过滤与并行 worker 的线程私有 DB 使用；AC 匹配器仍按需惰性构建。因 `ScanBatch()` 使用共享可变 scratch，多线程不可复用同一 `SignatureDB` 实例 |
| `const std::vector<FileSignature>& GetAll() const` | 获取所有签名 |
| `int LoadFromFile(const std::wstring& path)` | 从外部 JSON 文件加载自定义签名并**追加**到现有数据库。`path` — JSON 文件路径。返回成功加载的签名数量；文件打开或 JSON 解析失败时返回 `-1`。无效条目（缺字段、hex 非偶数长度或含非法字符、非对象元素）被跳过并记录警告日志。**A1-9 起无 Qt 依赖**：内部用 `std::ifstream`(`Utils::WideToUtf8`) + 零依赖 `scan/SignatureJson.h` 解析，非 GUI/headless 构建与 GUI 构建行为一致（旧实现依赖 `QFile`/`QJsonDocument`，非 GUI 构建恒返回 `-1`）；加载后清除 AC 匹配器缓存，`Match()`/`ScanBatch()` 立即可见新签名。UTF-8 BOM 自动剥离。JSON 格式详见 [`SIGNATURE_FORMAT.md`](./SIGNATURE_FORMAT.md)，样例见 `testdata/custom_signatures.json` |
| `void Add(FileSignature sig)` | 编程方式添加单个签名 |
| `const FileSignature* Match(const uint8_t* buf, size_t bufLen) const` | 匹配缓冲区前若干字节。`buf` — 数据缓冲区，`bufLen` — 缓冲区长度。返回匹配的签名指针，无匹配返回 `nullptr` |

---

### 4.6 `UnifiedVolumeScanner` 类

**头文件：** `scan/UnifiedVolumeScanner.h`

**职责：** 统一卷扫描器，组合文件系统元数据解析与签名扫描。

| 方法 | 说明 |
|---|---|
| `void SetCancelFlag(std::atomic<bool>* flag)` | 设置取消标志 |
| `void SetProgressCallback(ProgressCallback cb)` | 设置进度回调 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |
| `ScanResult Run(SectorReader& reader, const Partition& partition)` | 执行统一扫描 |

---

### 4.7 `VolumeSectorScanner` 类

**头文件：** `scan/VolumeSectorScanner.h`

**职责：** 卷扇区扫描器，根据分区类型自动创建对应的文件系统解析器执行扫描。

| 方法 | 说明 |
|---|---|
| `void SetCancelFlag(std::atomic<bool>* flag)` | 设置取消标志 |
| `void SetProgressCallback(ProgressCallback cb)` | 设置进度回调 |
| `void SetFileFoundCallback(FileFoundCallback cb)` | 设置文件发现回调 |
| `ScanResult Run(SectorReader& reader, const Partition& partition)` | 执行扫描 |

---

### 4.8 `ScanSerializer` 类

**头文件：** `scan/ScanSerializer.h`

**职责：** 将扫描结果（`ScanResult` + `Partition`）序列化/反序列化为 `.prs`（Phoenix Recover Scan）二进制文件。

**常量：**

| 常量 | 值 | 说明 |
|---|---|---|
| `kVersion` | `1` | 文件格式版本号 |
| `kFileExt` | `L".prs"` | 文件扩展名 |

#### 公开静态方法

| 方法 | 说明 |
|---|---|
| `static bool Save(const std::wstring& filePath, const ScanResult& result, const Partition& partition)` | 保存扫描结果到文件 |
| `static bool Load(const std::wstring& filePath, ScanResult& outResult, Partition& outPartition)` | 从文件加载扫描结果 |
| `static std::wstring SuggestFilename(const Partition& part)` | 根据分区信息和当前时间生成建议文件名（如 `"C_NTFS_20260318_143022.prs"`） |

---

### 4.9 文件雕刻器 (`scan/carve/`)

#### `CarveCommon.h` — 雕刻通用类型

```cpp
struct CarveResult {
    bool         ok;            // 雕刻是否成功
    uint64_t     endOffset;     // 文件结束偏移（不含）
    std::wstring extOverride;   // 扩展名覆盖（如 L"docx"）
    int          confidence;    // 置信度（0–100）
    std::wstring reason;        // 调试信息
};
```

#### `CarveMp4` 类

**头文件：** `scan/carve/CarveMp4.h`

| 方法 | 说明 |
|---|---|
| `static CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)` | 解析 ISO-BMFF box 序列估算文件结尾和置信度。适用于 MP4/M4V/MOV/3GP 等容器 |

#### `CarveZip` 类

**头文件：** `scan/carve/CarveZip.h`

| 方法 | 说明 |
|---|---|
| `static CarveResult Carve(SectorReader& reader, uint64_t startOffset, uint64_t maxEndOffset)` | 通过 EOCD（PK 05 06）定位 ZIP 文件结尾并验证中央目录 |

---

## 5. Recover — 恢复引擎

### 5.1 `FileNode` 结构体

**头文件：** `recover/FileNode.h`

**职责：** 表示一个可恢复的文件或目录节点，由所有扫描引擎和恢复引擎共用。

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `uint64_t` | 扫描结果内唯一 ID（NTFS: MFT 记录号，FAT: 序号，RAW: 起始字节偏移） |
| `name` | `std::wstring` | 文件名 |
| `originalPath` | `std::wstring` | 卷上的原始完整路径 |
| `extension` | `std::wstring` | 小写扩展名（如 `L"jpg"`） |
| `size` | `uint64_t` | 逻辑文件大小（字节） |
| `createTime` | `uint64_t` | 创建时间（Windows FILETIME） |
| `modifyTime` | `uint64_t` | 修改时间 |
| `accessTime` | `uint64_t` | 访问时间 |
| `status` | `FileStatus` | 文件状态 |
| `recoverability` | `RecoverabilityLevel` | 可恢复性评级 |
| `isDirectory` | `bool` | 是否为目录 |
| `fsType` | `std::wstring` | 文件系统类型标识 |
| `clusters` | `std::vector<uint64_t>` | 有序绝对簇号列表（NTFS/FAT） |
| `rawOffset` | `uint64_t` | RAW 扫描时的字节偏移 |
| `rawSize` | `uint64_t` | RAW 扫描时的大小 |
| `residentData` | `std::vector<uint8_t>` | NTFS 驻留文件数据（小文件 <700B，可完全恢复） |
| `isNtfsCompressed` | `bool` | 是否为 NTFS 压缩文件 |
| `compressionUnit` | `uint32_t` | NTFS 压缩单元大小（簇数），0 = 未压缩 |
| `parentId` | `uint64_t` | 父目录 FileNode::id（0 = 根） |

方法：`bool IsValid() const` — 名称非空或 `rawOffset > 0` 时返回 `true`。

---

### 5.2 `RecoverEngine` 类 ⭐重点

**头文件：** `recover/RecoverEngine.h`

**职责：** 读取簇/扇区数据并写入目标路径，支持单文件和批量恢复、NTFS LZNT1 解压、驻留数据恢复。

#### 关联类型

```cpp
enum class RecoverStatus { Success, Failed, Skipped };

struct FileRecoverResult {
    const FileNode* node;       // 源文件节点
    RecoverStatus   status;     // 恢复结果
    std::wstring    reason;     // 失败/跳过原因
    std::wstring    targetPath; // 目标文件路径
};

struct RecoverBatchResult {
    uint32_t  succeeded;        // 成功数
    uint32_t  failed;           // 失败数
    uint32_t  skipped;          // 跳过数
    bool      cancelled;        // 是否被取消
    std::vector<FileRecoverResult> details; // 每个文件的详细结果
};

using RecoverProgressCb = std::function<void(uint32_t done, uint32_t total)>;
using RecoverFileCb     = std::function<void(const FileRecoverResult&)>;
```

#### 公开方法

| 方法 | 说明 |
|---|---|
| `void SetProgressCallback(RecoverProgressCb cb)` | 设置批量恢复进度回调 |
| `void SetFileCallback(RecoverFileCb cb)` | 设置单文件恢复结果回调 |
| `FileRecoverResult RecoverFile(const FileNode& node, const std::wstring& targetDir, bool keepDirStructure, SectorReader& reader, const Partition& partition)` | 恢复单个文件。`node` — 源文件节点，`targetDir` — 目标目录，`keepDirStructure` — 是否保持原始目录结构，`reader` — 扇区读取器，`partition` — 源分区 |
| `RecoverBatchResult RecoverBatch(const std::vector<FileNode>& nodes, const std::wstring& targetDir, bool keepDirStructure, SectorReader& reader, const Partition& partition, std::atomic<bool>* cancelFlag)` | 批量恢复文件。`cancelFlag` — 可选取消标志 |
| `static bool ValidateTargetPath(const std::wstring& targetDir, const Partition& source)` | 验证目标路径不在源分区上（避免覆盖待恢复数据） |
| `static bool TryPreviewSingleReadSlice(SectorReader& reader, const Partition& partition, const FileNode& node, uint32_t maxBytes, uint64_t& outVolumeByteOffset, uint32_t& outLength)` | 检查预览数据是否可通过单次连续读取获得。`maxBytes` — 最大预览字节数，输出参数返回卷内字节偏移和长度 |
| `static bool ReadFileDataPrefix(SectorReader& reader, const Partition& partition, const FileNode& node, uint32_t maxBytes, std::vector<uint8_t>& out)` | 读取文件前若干字节用于预览（不写入文件）。`maxBytes` — 最大读取字节数 |

---

### 5.3 `FileFilter` 结构体

**头文件：** `recover/FileFilter.h`

**职责：** 不可变筛选条件集，`Matches()` 在节点满足所有活动条件时返回 `true`。

| 字段 | 类型 | 默认值 | 说明 |
|---|---|---|---|
| `includeExtensions` | `std::vector<std::wstring>` | 空 | 只包含这些扩展名（空 = 接受所有） |
| `excludeExtensions` | `std::vector<std::wstring>` | 空 | 排除这些扩展名 |
| `nameContains` | `std::wstring` | 空 | 文件名子串匹配（不区分大小写） |
| `pathContains` | `std::wstring` | 空 | 路径子串匹配（不区分大小写） |
| `minSize` | `uint64_t` | `0` | 最小文件大小（0 = 无限制） |
| `maxSize` | `uint64_t` | `0` | 最大文件大小（0 = 无限制） |
| `modifiedAfter` | `uint64_t` | `0` | 修改时间下限（FILETIME，0 = 无限制） |
| `modifiedBefore` | `uint64_t` | `0` | 修改时间上限 |
| `minRecoverability` | `RecoverabilityLevel` | `Unknown` | 最低可恢复性等级 |
| `includeDirectories` | `bool` | `false` | 是否包含目录 |

#### 公开方法

| 方法 | 说明 |
|---|---|
| `bool IsEmpty() const` | 所有条件均为默认值时返回 `true` |
| `bool Matches(const FileNode& node) const` | 判断节点是否满足所有条件 |
| `std::vector<FileNode> Apply(const std::vector<FileNode>& nodes) const` | 批量过滤，返回满足条件的节点列表 |

---

## 6. Controllers — 控制器层（Qt UI 桥接）

### 6.1 `ScanTypes.h` — UI 层数据传输类型

**头文件：** `controllers/ScanTypes.h`

#### `ScanPreviewKind` 枚举

| 值 | 含义 |
|---|---|
| `None` | 无预览 |
| `ResidentBlob` | 驻留数据旁路表 |
| `VolumeSlice` | 卷内偏移 + 读取长度 |
| `FullNode` | 需通过 `ReadFileDataPrefix` 完整读取 |

#### `ScanFileDto` 结构体

UI 线程使用的轻量文件描述（由控制层从 `FileNode` 转换）。

| 字段 | 类型 | 说明 |
|---|---|---|
| `id` | `quint64` | 文件 ID |
| `name` | `QString` | 文件名 |
| `fullPath` | `QString` | 完整路径 |
| `relativePath` | `QString` | 相对路径 |
| `extension` | `QString` | 扩展名 |
| `fsType` | `QString` | 文件系统类型 |
| `sizeBytes` | `qint64` | 文件大小 |
| `parentId` | `quint64` | 父目录 ID |
| `isDirectory` | `bool` | 是否目录 |
| `isDeleted` | `bool` | 是否已删除 |
| `isReconstructed` | `bool` | 是否重建 |
| `modifyTimeFt` | `quint64` | 修改时间（FILETIME） |
| `previewKind` | `ScanPreviewKind` | 预览策略 |
| `previewVolumeOffset` | `quint64` | 卷内预览偏移 |
| `previewReadLength` | `quint32` | 预览读取长度 |

已注册 Qt 元类型：`Q_DECLARE_METATYPE(ScanFileDto)` 和 `Q_DECLARE_METATYPE(QVector<ScanFileDto>)`。

---

### 6.2 `RecoveryScanController` 类

**头文件：** `controllers/RecoveryScanController.h`

**继承：** `QObject`

**职责：** 在子线程启动卷扫描，通过信号将进度与结果送到 UI（`QueuedConnection`）。

#### 公开方法

| 方法 | 说明 |
|---|---|
| `explicit RecoveryScanController(QObject* parent = nullptr)` | 构造函数 |
| `ScanResultStore* resultStore() const` | 获取扫描结果存储 |

#### 公开槽

| 槽 | 说明 |
|---|---|
| `void startVolumeScan(const QString& driveLetter)` | 启动卷扫描 |
| `void stopScan()` | 停止扫描 |

#### 信号

| 信号 | 说明 |
|---|---|
| `void scanIndexUpdated(quint64 revision)` | 扫描索引版本更新 |
| `void scanProgress(int percent, quint32 foundCount, quint64 scannedBytes, quint64 totalBytes, quint64 elapsedMs, QString phaseText)` | 扫描进度 |
| `void scanFinished(bool ok, const QString& message)` | 扫描完成 |

---

### 6.3 `RecoveryScanWorker` 类

**头文件：** `controllers/RecoveryScanWorker.h`

**继承：** `QObject`

**职责：** 在独立线程中执行统一卷扫描，仅由 `RecoveryScanController` 持有与调用。

#### 公开槽

| 槽 | 说明 |
|---|---|
| `void runVolumeScan(QString driveLetter)` | 执行卷扫描 |
| `void requestStopScan()` | 请求停止扫描 |

#### 信号

| 信号 | 说明 |
|---|---|
| `void indexUpdated(quint64 revision)` | 索引版本更新 |
| `void progress(int percent, quint32 foundCount, quint64 scannedBytes, quint64 totalBytes, quint64 elapsedMs, QString phaseText)` | 扫描进度 |
| `void finished(bool ok, QString message)` | 扫描完成 |

---

### 6.4 `ScanResultStore` 类

**头文件：** `controllers/ScanResultStore.h`

**职责：** 线程安全的扫描结果存储，提供路径树索引、类型分类索引和预览数据管理。使用 `QReadWriteLock` 保护。

#### 公开方法

| 方法 | 说明 |
|---|---|
| `void clear()` | 清空所有数据 |
| `bool upsert(const ScanFileDto& dto)` | 插入或更新文件记录 |
| `quint64 revision() const` | 当前数据版本号 |
| `int fileCount() const` | 文件总数 |
| **按 ID 查询** | |
| `bool tryGetFile(quint64 id, ScanFileDto* out) const` | 按 ID 获取文件 |
| **路径树索引** | |
| `QStringList pathChildDirectories(const QString& parentRel) const` | 获取子目录列表 |
| `int pathNodeCount(const QString& relPath) const` | 指定路径下的节点数 |
| `qint64 pathDirectFileBytes(const QString& parentRel) const` | 直接子文件总字节数 |
| `QVector<quint64> pathDirectChildIds(const QString& parentRel) const` | 直接子节点 ID 列表 |
| **类型分类索引** | |
| `QVector<QString> typeCategories() const` | 获取所有类型分类 |
| `QStringList typeCategoryChildren(const QString& category) const` | 分类下的子项列表 |
| `int typeNodeCount(const QString& key) const` | 指定类型键的节点数 |
| `QVector<quint64> typeNodeFileIds(const QString& key) const` | 指定类型键的文件 ID 列表 |
| **分区信息** | |
| `void setScanVolumePartition(const Partition& partition)` | 设置扫描卷分区信息 |
| `bool tryGetScanVolumePartition(Partition* out) const` | 获取扫描卷分区信息 |
| **预览与恢复数据** | |
| `void setPreviewResidentBlob(quint64 id, const QByteArray& bytes)` | 设置驻留预览数据 |
| `void setPreviewFileNodeSnapshot(quint64 id, const FileNode& node)` | 设置预览文件节点快照 |
| `bool tryGetPreviewResidentBlob(quint64 id, QByteArray* out) const` | 获取驻留预览数据 |
| `bool tryGetPreviewFileNode(quint64 id, FileNode* out) const` | 获取预览文件节点 |
| `void setRecoveredFileNode(quint64 id, const FileNode& node)` | 设置已恢复文件节点 |
| `bool tryGetRecoveredFileNode(quint64 id, FileNode* out) const` | 获取已恢复文件节点 |

---

### 6.5 `RecoveryFlowController` 类

**头文件：** `controllers/RecoveryFlowController.h`

**继承：** `QObject`

**职责：** 恢复流程控制器，处理用户选择的恢复位置和恢复方式。

#### 公开槽

| 槽 | 说明 |
|---|---|
| `void handleLocationRequested(const QString& selectedLocation)` | 处理位置选择 |
| `void handleSdCardRecoveryRequested()` | 处理 SD 卡恢复请求 |
| `void handleToolRecoveryRequested(const QString& toolName)` | 处理工具恢复请求 |

---

## 7. Media — 媒体缩略图

### 7.1 `MediaThumbnails` 命名空间

**头文件：** `media/MediaThumbnails.h`

**职责：** 为本地文件生成网格用缩略图（约 180×110 像素），按平台选择解码/系统 API。

| 函数 | 说明 |
|---|---|
| `QPixmap videoPreviewPixmapForLocalFile(const QString& localFilePath)` | 为视频文件生成预览缩略图 |
| `QPixmap imagePreviewPixmapForLocalFile(const QString& localFilePath)` | 为图片文件生成预览缩略图 |

---

## 8. Log — 日志系统

### 8.1 `Logger` 类

**头文件：** `log/Logger.h`

**职责：** 单例日志系统，支持控制台和文件双输出，独立的日志级别控制，线程安全。

#### `LogLevel` 枚举

| 值 | 说明 |
|---|---|
| `Debug` | 调试信息 |
| `Info` | 一般信息 |
| `Warning` | 警告 |
| `Error` | 错误 |
| `Fatal` | 致命错误 |

#### 公开方法

| 方法 | 说明 |
|---|---|
| `static Logger& Instance()` | 获取单例实例 |
| **级别控制** | |
| `void SetLogLevel(LogLevel level)` | 同时设置控制台和文件的最低日志级别（向后兼容） |
| `void SetConsoleLogLevel(LogLevel level)` | 仅设置控制台最低日志级别 |
| `void SetFileLogLevel(LogLevel level)` | 仅设置文件最低日志级别（默认 `Debug`） |
| **输出控制** | |
| `void SetConsoleOutput(bool enabled)` | 启用/禁用控制台输出 |
| `void SetLogFile(const std::wstring& path)` | 打开指定日志文件（追加模式） |
| `std::wstring OpenAutoLogFile(const std::wstring& dir = L"")` | 自动创建带时间戳的日志文件，返回实际路径 |
| **日志写入** | |
| `void Log(LogLevel level, const std::wstring& msg, const char* file = nullptr, int line = 0)` | 写入日志 |
| `void Debug(const std::wstring& m, ...)` | 写入 Debug 级别日志 |
| `void Info(const std::wstring& m, ...)` | 写入 Info 级别日志 |
| `void Warning(const std::wstring& m, ...)` | 写入 Warning 级别日志 |
| `void Error(const std::wstring& m, ...)` | 写入 Error 级别日志 |
| `void Fatal(const std::wstring& m, ...)` | 写入 Fatal 级别日志 |
| **级别检查** | |
| `bool ShouldLog(LogLevel level) const noexcept` | 检查是否有任何输出通道会记录该级别的日志。应在构造日志消息字符串前调用以避免不必要的开销 |

#### 便捷宏

| 宏 | 说明 |
|---|---|
| `LOG_DEBUG(msg)` | 惰性求值 Debug 日志 |
| `LOG_INFO(msg)` | 惰性求值 Info 日志 |
| `LOG_WARN(msg)` | 惰性求值 Warning 日志 |
| `LOG_ERROR(msg)` | 惰性求值 Error 日志 |
| `LOG_FATAL(msg)` | 惰性求值 Fatal 日志 |

> 所有宏使用惰性求值：仅当当前日志级别需要时才计算消息表达式和 `wstring` 拼接，在热路径上几乎零开销。
