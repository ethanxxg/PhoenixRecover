#include "SmartInfo.h"
#include "../common/Platform.h"

#if PHOENIX_OS_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#endif
#include "../log/Logger.h"
#include <algorithm>
#include <cstring>

#if PHOENIX_OS_WINDOWS
// ─────────────────────────────────────────────────────────────────────────────
// S.M.A.R.T IOCTL definitions (not always in SDK headers)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SMART_RCV_DRIVE_DATA
#define SMART_RCV_DRIVE_DATA CTL_CODE(IOCTL_DISK_BASE, 0x0022, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif

#ifndef SMART_GET_VERSION
#define SMART_GET_VERSION CTL_CODE(IOCTL_DISK_BASE, 0x0020, METHOD_BUFFERED, FILE_READ_ACCESS)
#endif

#pragma pack(push, 1)

// GETVERSIONINPARAMS structure
struct SMART_VERSION
{
    BYTE  bVersion;
    BYTE  bRevision;
    BYTE  bReserved;
    BYTE  bIDEDeviceMap;
    DWORD fCapabilities;
    DWORD dwReserved[4];
};

// SENDCMDINPARAMS structure
struct SMART_CMD_IN
{
    DWORD cBufferSize;
    BYTE  bDriveNumber;
    BYTE  bReserved[3];
    DWORD dwReserved[4];
    BYTE  bFeaturesReg;
    BYTE  bSectorCountReg;
    BYTE  bSectorNumberReg;
    BYTE  bCylLowReg;
    BYTE  bCylHighReg;
    BYTE  bDriveHeadReg;
    BYTE  bCommandReg;
    BYTE  bReserved2;
    BYTE  bData[1];
};

// SENDCMDOUTPARAMS structure
struct SMART_CMD_OUT
{
    DWORD cBufferSize;
    BYTE  bDriverStatus;
    BYTE  bIDEStatus;
    BYTE  bReserved[2];
    DWORD dwReserved[2];
    BYTE  bBuffer[512];
};

// ATA IDENTIFY response (partial)
struct ATA_IDENTIFY
{
    WORD  generalConfig;          // 0
    WORD  numCylinders;           // 1
    WORD  reserved2;              // 2
    WORD  numHeads;               // 3
    WORD  bytesPerTrack;          // 4
    WORD  bytesPerSector;         // 5
    WORD  sectorsPerTrack;        // 6
    WORD  vendorUnique[3];        // 7-9
    CHAR  serialNumber[20];       // 10-19
    WORD  bufferType;             // 20
    WORD  bufferSectorSize;       // 21
    WORD  numECCBytes;            // 22
    CHAR  firmwareRev[8];         // 23-26
    CHAR  modelNumber[40];        // 27-46
    // ... more fields
};

// S.M.A.R.T attribute structure (12 bytes per attribute)
struct SMART_ATTR
{
    BYTE  id;
    WORD  flags;
    BYTE  current;
    BYTE  worst;
    BYTE  raw[6];
    BYTE  reserved;
};

// S.M.A.R.T data structure (362 bytes)
struct SMART_DATA
{
    WORD  revision;
    SMART_ATTR attrs[30];
    BYTE  reserved[149];
    BYTE  status;
};

// S.M.A.R.T threshold structure
struct SMART_THRESH_ENTRY
{
    BYTE  id;
    BYTE  threshold;
    BYTE  reserved[10];
};

struct SMART_THRESHOLDS
{
    WORD  revision;
    SMART_THRESH_ENTRY thresholds[30];
    BYTE  reserved[149];
    BYTE  checksum;
};

#pragma pack(pop)

// ATA command codes
constexpr BYTE kAtaSmartCmd         = 0xB0;
constexpr BYTE kAtaIdentifyCmd      = 0xEC;
constexpr BYTE kSmartReadValues     = 0xD0;
constexpr BYTE kSmartReadThresholds = 0xD1;
constexpr BYTE kSmartEnableOps      = 0xD8;
constexpr WORD kSmartCylLow         = 0x4F;
constexpr WORD kSmartCylHigh        = 0xC2;

// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Swap bytes in a string (ATA strings are byte-swapped)
void SwapBytes(char* str, int len)
{
    for (int i = 0; i < len - 1; i += 2)
        std::swap(str[i], str[i + 1]);
}

// Trim trailing spaces
std::wstring TrimAndConvert(const char* str, int len)
{
    std::string s(str, len);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
        s.pop_back();
    int wlen = ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (wlen <= 0) return L"";
    std::wstring ws(wlen, L'\0');
    // 兼容较旧标准库：ws.data() 在 C++11/14 下可能返回 const wchar_t*，
    // MultiByteToWideChar 需要可写缓冲区（LPWSTR），因此用 &ws[0]。
    ::MultiByteToWideChar(CP_ACP, 0, s.c_str(), (int)s.size(), &ws[0], wlen);
    return ws;
}

// Open physical disk handle
HANDLE OpenDisk(int index)
{
    wchar_t path[64]{};
    swprintf_s(path, L"\\\\.\\PhysicalDrive%d", index);
    return ::CreateFileW(path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
}

// Send ATA command via SMART_RCV_DRIVE_DATA
bool SendAtaCommand(HANDLE hDisk, BYTE cmd, BYTE feature, BYTE* outBuf, DWORD outLen)
{
    DWORD inSize = sizeof(SMART_CMD_IN) - 1;
    DWORD outSize = sizeof(SMART_CMD_OUT) + outLen;

    std::vector<BYTE> inBuf(inSize, 0);
    std::vector<BYTE> outData(outSize, 0);

    auto* cmdIn = reinterpret_cast<SMART_CMD_IN*>(inBuf.data());
    cmdIn->cBufferSize = outLen;
    cmdIn->bFeaturesReg = feature;
    cmdIn->bCylLowReg = kSmartCylLow;
    cmdIn->bCylHighReg = kSmartCylHigh;
    cmdIn->bCommandReg = cmd;

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(hDisk, SMART_RCV_DRIVE_DATA,
        inBuf.data(), inSize,
        outData.data(), outSize,
        &bytesReturned, nullptr);

    if (!ok) return false;

    auto* cmdOut = reinterpret_cast<SMART_CMD_OUT*>(outData.data());
    if (outBuf && outLen > 0)
        memcpy(outBuf, cmdOut->bBuffer, std::min(outLen, (DWORD)512));
    return true;
}

// Send IDENTIFY command
bool SendIdentify(HANDLE hDisk, ATA_IDENTIFY* id)
{
    DWORD inSize = sizeof(SMART_CMD_IN) - 1;
    DWORD outSize = sizeof(SMART_CMD_OUT) + 512;

    std::vector<BYTE> inBuf(inSize, 0);
    std::vector<BYTE> outData(outSize, 0);

    auto* cmdIn = reinterpret_cast<SMART_CMD_IN*>(inBuf.data());
    cmdIn->cBufferSize = 512;
    cmdIn->bCommandReg = kAtaIdentifyCmd;

    DWORD bytesReturned = 0;
    BOOL ok = ::DeviceIoControl(hDisk, SMART_RCV_DRIVE_DATA,
        inBuf.data(), inSize,
        outData.data(), outSize,
        &bytesReturned, nullptr);

    if (!ok) return false;

    auto* cmdOut = reinterpret_cast<SMART_CMD_OUT*>(outData.data());
    memcpy(id, cmdOut->bBuffer, sizeof(ATA_IDENTIFY));
    return true;
}

} // anonymous namespace

#endif // PHOENIX_OS_WINDOWS (disk I/O helpers)

// ─────────────────────────────────────────────────────────────────────────────
// GetAttributeName
// ─────────────────────────────────────────────────────────────────────────────
std::wstring SmartReader::GetAttributeName(uint8_t id)
{
    switch (id) {
    case 0x01: return L"读取错误率";
    case 0x02: return L"吞吐性能";
    case 0x03: return L"主轴启动时间";
    case 0x04: return L"启停次数";
    case 0x05: return L"重新分配扇区数";
    case 0x07: return L"寻道错误率";
    case 0x08: return L"寻道时间性能";
    case 0x09: return L"通电时间";
    case 0x0A: return L"主轴重试次数";
    case 0x0B: return L"校准重试次数";
    case 0x0C: return L"通电周期";
    case 0xBB: return L"报告的不可纠正错误";
    case 0xBC: return L"命令超时";
    case 0xBD: return L"高飞写入";
    case 0xBE: return L"气流温度";
    case 0xBF: return L"G-sense 错误率";
    case 0xC0: return L"断电保护次数";
    case 0xC1: return L"加载/卸载周期";
    case 0xC2: return L"温度";
    case 0xC3: return L"硬件 ECC 恢复";
    case 0xC4: return L"重新分配事件";
    case 0xC5: return L"当前待定扇区";
    case 0xC6: return L"不可纠正扇区";
    case 0xC7: return L"UltraDMA CRC 错误";
    case 0xC8: return L"写入错误率";
    case 0xC9: return L"软读取错误率";
    case 0xCA: return L"数据寻址标记错误";
    case 0xCB: return L"取消 ECC";
    case 0xCC: return L"软 ECC 纠正";
    case 0xCD: return L"热节流状态";
    case 0xDC: return L"磁盘偏移";
    case 0xDD: return L"振动";
    case 0xDE: return L"加载时间";
    case 0xDF: return L"加载/卸载重试";
    case 0xE0: return L"加载摩擦";
    case 0xE1: return L"加载/卸载周期";
    case 0xE2: return L"加载时间";
    case 0xE3: return L"扭矩放大";
    case 0xE4: return L"断电回缩";
    case 0xE6: return L"GMR 磁头振幅";
    case 0xE7: return L"温度";
    case 0xF0: return L"磁头飞行时间";
    case 0xF1: return L"总 LBA 写入";
    case 0xF2: return L"总 LBA 读取";
    case 0xFE: return L"自由落体保护";
    default:   return L"属性 " + std::to_wstring(id);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HealthStatusText
// ─────────────────────────────────────────────────────────────────────────────
std::wstring SmartReader::HealthStatusText(DiskHealthStatus status)
{
    switch (status) {
    case DiskHealthStatus::Good:     return L"良好";
    case DiskHealthStatus::Warning:  return L"警告";
    case DiskHealthStatus::Critical: return L"危险";
    case DiskHealthStatus::Failing:  return L"即将故障";
    default:                         return L"未知";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// HealthStatusColor (RGB)
// ─────────────────────────────────────────────────────────────────────────────
uint32_t SmartReader::HealthStatusColor(DiskHealthStatus status)
{
    switch (status) {
    case DiskHealthStatus::Good:     return 0x22A65A;  // Green
    case DiskHealthStatus::Warning:  return 0xF4A020;  // Orange
    case DiskHealthStatus::Critical: return 0xDC3545;  // Red
    case DiskHealthStatus::Failing:  return 0x8B0000;  // Dark Red
    default:                         return 0x808080;  // Gray
    }
}

#if PHOENIX_OS_WINDOWS
// ─────────────────────────────────────────────────────────────────────────────
// ReadDiskSmart
// ─────────────────────────────────────────────────────────────────────────────
SmartInfo SmartReader::ReadDiskSmart(int diskIndex)
{
    SmartInfo info;
    info.diskIndex = diskIndex;

    HANDLE hDisk = OpenDisk(diskIndex);
    if (hDisk == INVALID_HANDLE_VALUE) {
        LOG_WARN(L"SmartReader: cannot open PhysicalDrive" +
                 std::to_wstring(diskIndex));
        return info;
    }

    // Check if SMART is supported
    SMART_VERSION ver{};
    DWORD ret = 0;
    BOOL ok = ::DeviceIoControl(hDisk, SMART_GET_VERSION,
        nullptr, 0, &ver, sizeof(ver), &ret, nullptr);
    if (!ok) {
        ::CloseHandle(hDisk);
        return info;
    }

    info.smartSupported = (ver.fCapabilities & 1) != 0;
    if (!info.smartSupported) {
        ::CloseHandle(hDisk);
        return info;
    }

    // Get IDENTIFY data for model/serial
    ATA_IDENTIFY identify{};
    if (SendIdentify(hDisk, &identify)) {
        SwapBytes(identify.serialNumber, 20);
        SwapBytes(identify.firmwareRev, 8);
        SwapBytes(identify.modelNumber, 40);
        info.serial   = TrimAndConvert(identify.serialNumber, 20);
        info.firmware = TrimAndConvert(identify.firmwareRev, 8);
        info.model    = TrimAndConvert(identify.modelNumber, 40);
    }

    // Enable SMART
    SendAtaCommand(hDisk, kAtaSmartCmd, kSmartEnableOps, nullptr, 0);
    info.smartEnabled = true;

    // Read SMART attributes
    BYTE smartData[512]{};
    if (!SendAtaCommand(hDisk, kAtaSmartCmd, kSmartReadValues, smartData, 512)) {
        ::CloseHandle(hDisk);
        return info;
    }

    // Read SMART thresholds
    BYTE threshData[512]{};
    SendAtaCommand(hDisk, kAtaSmartCmd, kSmartReadThresholds, threshData, 512);

    auto* sd = reinterpret_cast<SMART_DATA*>(smartData);
    auto* st = reinterpret_cast<SMART_THRESHOLDS*>(threshData);

    // Parse attributes
    bool anyBelowThreshold = false;
    bool anyWarning = false;

    for (int i = 0; i < 30; ++i) {
        const SMART_ATTR& attr = sd->attrs[i];
        if (attr.id == 0) continue;

        SmartAttribute a;
        a.id = attr.id;
        a.name = GetAttributeName(attr.id);
        a.current = attr.current;
        a.worst = attr.worst;
        a.preFail = (attr.flags & 0x01) != 0;

        // Find threshold
        for (int j = 0; j < 30; ++j) {
            if (st->thresholds[j].id == attr.id) {
                a.threshold = st->thresholds[j].threshold;
                break;
            }
        }

        // Raw value (6 bytes, little-endian)
        a.rawValue = 0;
        for (int b = 5; b >= 0; --b)
            a.rawValue = (a.rawValue << 8) | attr.raw[b];

        // Check threshold
        if (a.threshold > 0 && a.current > 0 && a.current <= a.threshold) {
            a.belowThreshold = true;
            anyBelowThreshold = true;
        }

        // Check for degradation
        if (a.current > 0 && a.worst > 0 && a.current < a.worst * 0.9)
            anyWarning = true;

        // Extract common values
        switch (attr.id) {
        case 0x09:  // Power-on hours
            info.powerOnHours = static_cast<uint32_t>(a.rawValue & 0xFFFFFFFF);
            break;
        case 0x0C:  // Power cycle count
            info.powerCycleCount = static_cast<uint32_t>(a.rawValue & 0xFFFFFFFF);
            break;
        case 0x05:  // Reallocated sector count
            info.reallocatedSectors = static_cast<uint32_t>(a.rawValue & 0xFFFF);
            if (info.reallocatedSectors > 0) anyWarning = true;
            if (info.reallocatedSectors > 10) anyBelowThreshold = true;
            break;
        case 0xC5:  // Current pending sector count
            info.pendingSectors = static_cast<uint32_t>(a.rawValue & 0xFFFF);
            if (info.pendingSectors > 0) anyWarning = true;
            break;
        case 0xC6:  // Uncorrectable sector count
            info.uncorrectableSectors = static_cast<uint32_t>(a.rawValue & 0xFFFF);
            if (info.uncorrectableSectors > 0) anyWarning = true;
            break;
        case 0xC2:  // Temperature
        case 0xBE:  // Airflow temperature
            info.temperatureC = static_cast<uint32_t>(a.rawValue & 0xFF);
            break;
        }

        info.attributes.push_back(a);
    }

    // Determine health status
    if (anyBelowThreshold)
        info.health = DiskHealthStatus::Critical;
    else if (anyWarning)
        info.health = DiskHealthStatus::Warning;
    else if (!info.attributes.empty())
        info.health = DiskHealthStatus::Good;
    else
        info.health = DiskHealthStatus::Unknown;

    ::CloseHandle(hDisk);

    LOG_INFO(L"SmartReader: Disk " + std::to_wstring(diskIndex) +
             L" \"" + info.model + L"\" – " + HealthStatusText(info.health) +
             L" (T=" + std::to_wstring(info.temperatureC) + L"°C)");
    return info;
}

// ─────────────────────────────────────────────────────────────────────────────
// ReadAllDisks
// ─────────────────────────────────────────────────────────────────────────────
std::vector<SmartInfo> SmartReader::ReadAllDisks()
{
    std::vector<SmartInfo> result;
    for (int i = 0; i < 16; ++i) {
        HANDLE h = OpenDisk(i);
        if (h == INVALID_HANDLE_VALUE) continue;
        ::CloseHandle(h);
        result.push_back(ReadDiskSmart(i));
    }
    return result;
}

#else

SmartInfo SmartReader::ReadDiskSmart(int diskIndex)
{
    SmartInfo info;
    info.diskIndex = diskIndex;
    return info;
}

std::vector<SmartInfo> SmartReader::ReadAllDisks()
{
    return {};
}

#endif
