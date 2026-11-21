#include "../common/Platform.h"
#if PHOENIX_OS_WINDOWS

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winioctl.h>

#include "DiskManager.h"
#include "../log/Logger.h"
#include "../common/Utils.h"
#include <vector>
#include <algorithm>

// ── Internal helpers ────────────────────────────────────────────────────────
namespace {

std::wstring BusTypeStr(STORAGE_BUS_TYPE bt)
{
    switch (bt) {
    case BusTypeAta:    return L"ATA";
    case BusTypeSata:   return L"SATA";
    case BusTypeNvme:   return L"NVMe";
    case BusTypeUsb:    return L"USB";
    case BusTypeScsi:   return L"SCSI";
    case BusTypeSd:     return L"SD";
    case BusTypeMmc:    return L"MMC";
    default:            return L"Unknown";
    }
}

// Populate model / serial / interface type from StorageDeviceProperty
void QueryStorageProperty(HANDLE h, PhysicalDisk& disk)
{
    STORAGE_PROPERTY_QUERY q{};
    q.PropertyId = StorageDeviceProperty;
    q.QueryType  = PropertyStandardQuery;

    char buf[2048]{};
    DWORD ret = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
                         &q, sizeof(q), buf, sizeof(buf), &ret, nullptr))
        return;

    auto* desc = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(buf);

    if (desc->ProductIdOffset)
        disk.model = Utils::TrimRight(
            Utils::AnsiToWide(buf + desc->ProductIdOffset));

    if (desc->SerialNumberOffset)
        disk.serialNumber = Utils::TrimRight(
            Utils::AnsiToWide(buf + desc->SerialNumberOffset));

    disk.interfaceType = BusTypeStr(desc->BusType);
    disk.isRemovable   = desc->RemovableMedia != 0;
}

} // anonymous namespace

// ── DiskManager::GetDisks ───────────────────────────────────────────────────
std::vector<PhysicalDisk> DiskManager::GetDisks()
{
    std::vector<PhysicalDisk> disks;

    for (int i = 0; i < 32; ++i) {
        std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(i);

        HANDLE h = CreateFileW(path.c_str(),
                               GENERIC_READ,
                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        PhysicalDisk disk;
        disk.index      = i;
        disk.devicePath = path;

        // ── Geometry ──────────────────────────────────────────────────────
        DWORD ret = 0;
        DISK_GEOMETRY_EX geoEx{};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                            nullptr, 0, &geoEx, sizeof(geoEx), &ret, nullptr))
        {
            disk.sectorSize   = geoEx.Geometry.BytesPerSector;
            disk.sizeBytes    = static_cast<uint64_t>(geoEx.DiskSize.QuadPart);
            disk.totalSectors = disk.sizeBytes / disk.sectorSize;
        }

        // ── Model / Serial / Interface ────────────────────────────────────
        QueryStorageProperty(h, disk);

        // ── Partition layout ──────────────────────────────────────────────
        {
            constexpr size_t kPartBufSize =
                sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                sizeof(PARTITION_INFORMATION_EX) * 128;
            std::vector<BYTE> pBuf(kPartBufSize, 0);

            if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                                nullptr, 0, pBuf.data(), (DWORD)pBuf.size(),
                                &ret, nullptr))
            {
                auto* layout =
                    reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(pBuf.data());
                disk.isGPT = (layout->PartitionStyle == PARTITION_STYLE_GPT);

                for (DWORD p = 0; p < layout->PartitionCount; ++p) {
                    const auto& pi = layout->PartitionEntry[p];
                    if (pi.PartitionLength.QuadPart == 0) continue;

                    // Skip MBR extended / empty partitions
                    if (!disk.isGPT) {
                        BYTE t = pi.Mbr.PartitionType;
                        if (t == 0x00 || t == 0x05 || t == 0x0F || t == 0x85)
                            continue;
                    }

                    Partition part;
                    part.physicalDiskIdx = i;
                    part.index           = static_cast<int>(disk.partitions.size());
                    part.startSector     = static_cast<uint64_t>(
                        pi.StartingOffset.QuadPart) / disk.sectorSize;
                    part.sizeBytes       = static_cast<uint64_t>(
                        pi.PartitionLength.QuadPart);
                    part.totalSectors    = part.sizeBytes / disk.sectorSize;

                    if (!disk.isGPT)
                        part.isActive = pi.Mbr.BootIndicator != 0;

                    part.fsType = DetectFsType(h, part.startSector, disk.sectorSize);

                    disk.partitions.push_back(part);
                }
            }
        }

        // ── SMART health ──────────────────────────────────────────────────
        // Re-open with read+write access for IOCTL_STORAGE_PREDICT_FAILURE
        {
            HANDLE hSmart = CreateFileW(path.c_str(),
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (hSmart != INVALID_HANDLE_VALUE) {
                QuerySmart(hSmart, disk);
                CloseHandle(hSmart);
            } else {
                // Try read-only as fallback (may still work on some systems)
                QuerySmart(h, disk);
            }
        }

        CloseHandle(h);
        disks.push_back(std::move(disk));
    }

    // Fill in drive letters and volume labels
    AssignDriveLetters(disks);

    return disks;
}

// ── DetectFsType ─────────────────────────────────────────────────────────────
std::wstring DiskManager::DetectFsType(HANDLE hDisk, uint64_t startSector,
                                       uint32_t sectorSize)
{
    if (hDisk == INVALID_HANDLE_VALUE || sectorSize == 0) return L"Unknown";

    std::vector<uint8_t> sector(sectorSize, 0);
    LARGE_INTEGER li{};
    li.QuadPart = static_cast<LONGLONG>(startSector) * sectorSize;
    SetFilePointerEx(hDisk, li, nullptr, FILE_BEGIN);

    DWORD read = 0;
    ReadFile(hDisk, sector.data(), sectorSize, &read, nullptr);
    if (read < 11) return L"Unknown";

    // NTFS:  OEM ID at offset 3
    if (memcmp(sector.data() + 3, "NTFS    ", 8) == 0) return L"NTFS";

    // exFAT: OEM ID at offset 3
    if (memcmp(sector.data() + 3, "EXFAT   ", 8) == 0) return L"exFAT";

    // FAT32: FS type string at offset 82
    if (read >= 90 && memcmp(sector.data() + 82, "FAT32   ", 8) == 0)
        return L"FAT32";

    // FAT16/12: FS type string at offset 54
    if (read >= 62) {
        if (memcmp(sector.data() + 54, "FAT16   ", 8) == 0) return L"FAT16";
        if (memcmp(sector.data() + 54, "FAT12   ", 8) == 0) return L"FAT12";
        if (memcmp(sector.data() + 54, "FAT     ", 8) == 0) return L"FAT16";
    }

    return L"Unknown";
}

// ── AssignDriveLetters ────────────────────────────────────────────────────────
void DiskManager::AssignDriveLetters(std::vector<PhysicalDisk>& disks)
{
    wchar_t driveBuf[512]{};
    GetLogicalDriveStringsW(511, driveBuf);

    for (wchar_t* p = driveBuf; *p; p += wcslen(p) + 1) {
        wchar_t letter[3] = { p[0], L':', 0 };

        std::wstring volPath = std::wstring(L"\\\\.\\") + letter;
        HANDLE hVol = CreateFileW(volPath.c_str(), 0,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (hVol == INVALID_HANDLE_VALUE) continue;

        VOLUME_DISK_EXTENTS vde{};
        DWORD ret = 0;
        if (DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                            nullptr, 0, &vde, sizeof(vde), &ret, nullptr))
        {
            for (DWORD e = 0; e < vde.NumberOfDiskExtents; ++e) {
                DWORD diskNum = vde.Extents[e].DiskNumber;

                auto it = std::find_if(disks.begin(), disks.end(),
                    [diskNum](const PhysicalDisk& d) {
                        return d.index == static_cast<int>(diskNum);
                    });
                if (it == disks.end()) continue;

                uint64_t startSector = static_cast<uint64_t>(
                    vde.Extents[e].StartingOffset.QuadPart) / it->sectorSize;

                for (auto& part : it->partitions) {
                    if (part.startSector == startSector) {
                        part.driveLetter = std::wstring(1, p[0]) + L":";

                        // Get volume label and actual FS type from Windows
                        wchar_t volLabel[256]{};
                        wchar_t fsName[64]{};
                        GetVolumeInformationW(p,
                            volLabel, 256,
                            nullptr, nullptr, nullptr,
                            fsName, 64);
                        part.label  = volLabel;
                        if (wcslen(fsName) > 0)
                            part.fsType = fsName;
                        break;
                    }
                }
            }
        }
        CloseHandle(hVol);
    }
}

// ── QuerySmart ────────────────────────────────────────────────────────────────
// SMART attribute entry layout inside the 512-byte vendor data block returned
// by IOCTL_STORAGE_PREDICT_FAILURE (matches ATA Identify SMART data page 1).
#pragma pack(push,1)
struct SmartAttr {
    uint8_t  id;
    uint16_t flags;
    uint8_t  current;
    uint8_t  worst;
    uint8_t  raw[6];     // raw value (little-endian, 6 bytes)
    uint8_t  reserved;
};
#pragma pack(pop)

void DiskManager::QuerySmart(HANDLE hDisk, PhysicalDisk& disk)
{
    // IOCTL_STORAGE_PREDICT_FAILURE returns:
    //   STORAGE_PREDICT_FAILURE.PredictFailure  (DWORD, 1 = warning)
    //   STORAGE_PREDICT_FAILURE.VendorSpecific  (BYTE[512])
    // The VendorSpecific block follows the ATA SMART READ DATA format:
    //   offset 0:      512 bytes of SMART attributes
    //   attributes:    30 × 12-byte entries starting at offset 2
    STORAGE_PREDICT_FAILURE spf{};
    DWORD ret = 0;
    if (!DeviceIoControl(hDisk, IOCTL_STORAGE_PREDICT_FAILURE,
                         nullptr, 0, &spf, sizeof(spf), &ret, nullptr)) {
        disk.smart.available  = false;
        disk.smart.statusText = L"未知";
        return;
    }

    disk.smart.available      = true;
    disk.smart.predictFailure = (spf.PredictFailure != 0);

    // ATA SMART attribute block: attributes start at byte 2 of VendorSpecific,
    // each attribute is 12 bytes, 30 attributes max.
    const uint8_t* base = spf.VendorSpecific + 2;
    constexpr int kAttrCount  = 30;
    constexpr int kAttrStride = 12;

    for (int i = 0; i < kAttrCount; ++i) {
        const uint8_t* entry = base + i * kAttrStride;
        if (entry + kAttrStride > spf.VendorSpecific + 512) break;

        uint8_t attrId = entry[0];
        if (attrId == 0) continue;

        // Raw value: 4 bytes LE at bytes 5-8 within the 12-byte entry
        uint32_t raw = static_cast<uint32_t>(entry[5])
                     | (static_cast<uint32_t>(entry[6]) << 8)
                     | (static_cast<uint32_t>(entry[7]) << 16)
                     | (static_cast<uint32_t>(entry[8]) << 24);

        switch (attrId) {
        case 0x05: disk.smart.reallocSectors = raw; break;
        case 0x09: disk.smart.powerOnHours   = raw; break;
        case 0xBE: disk.smart.temperature    = raw & 0xFF; break;  // lower byte
        case 0xC2: if (!disk.smart.temperature) disk.smart.temperature = raw & 0xFF; break;
        case 0xC5: disk.smart.pendingSectors  = raw; break;
        case 0xC6: disk.smart.uncorrectable   = raw; break;
        default:   break;
        }
    }

    // ── Compute health score (0-100) ──────────────────────────────────────
    // Start from 100; deduct for bad sectors
    int score = 100;
    if (disk.smart.predictFailure)  score = std::min(score, 30);
    if (disk.smart.reallocSectors)  score -= std::min(30, static_cast<int>(disk.smart.reallocSectors / 2 + 5));
    if (disk.smart.pendingSectors)  score -= std::min(20, static_cast<int>(disk.smart.pendingSectors / 2 + 3));
    if (disk.smart.uncorrectable)   score -= std::min(20, static_cast<int>(disk.smart.uncorrectable  / 2 + 3));
    if (score < 0) score = 0;
    disk.smart.healthScore = score;

    // ── Status label ──────────────────────────────────────────────────────
    if (disk.smart.predictFailure || score < 40) {
        disk.smart.statusText = L"危险";
    } else if (score < 70 || disk.smart.reallocSectors || disk.smart.pendingSectors) {
        disk.smart.statusText = L"注意";
    } else {
        disk.smart.statusText = L"良好";
    }

    // ── Detail string (tooltip) ───────────────────────────────────────────
    wchar_t buf[512];
    swprintf_s(buf,
        L"健康: %s  得分: %d\n"
        L"重映射扇区: %u  待定扇区: %u  不可恢复: %u\n"
        L"通电时间: %u h  温度: %u °C",
        disk.smart.statusText.c_str(),
        disk.smart.healthScore,
        disk.smart.reallocSectors,
        disk.smart.pendingSectors,
        disk.smart.uncorrectable,
        disk.smart.powerOnHours,
        disk.smart.temperature);
    disk.smart.detail = buf;
}

// ── LoadImageFile ─────────────────────────────────────────────────────────────
bool DiskManager::LoadImageFile(const std::wstring& imagePath, PhysicalDisk& outDisk)
{
    HANDLE h = CreateFileW(imagePath.c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        LOG_ERROR(L"LoadImageFile: cannot open " + imagePath);
        return false;
    }

    LARGE_INTEGER sz{};
    GetFileSizeEx(h, &sz);

    PhysicalDisk disk;
    disk.index         = -1;
    disk.devicePath    = imagePath;
    disk.sectorSize    = 512;
    disk.sizeBytes     = static_cast<uint64_t>(sz.QuadPart);
    disk.totalSectors  = disk.sizeBytes / disk.sectorSize;
    disk.interfaceType = L"Image";

    // Extract filename as model
    size_t pos = imagePath.find_last_of(L"\\/");
    disk.model = (pos != std::wstring::npos)
                 ? imagePath.substr(pos + 1)
                 : imagePath;

    // Try to parse the partition table from the image
    // Simple approach: check for MBR/GPT at sector 0
    std::vector<uint8_t> sector0(512, 0);
    DWORD read = 0;
    ReadFile(h, sector0.data(), 512, &read, nullptr);

    if (read >= 512 && sector0[510] == 0x55 && sector0[511] == 0xAA) {
        // Has boot signature – try MBR partition table at offset 446
        auto* mbrParts = reinterpret_cast<uint8_t*>(sector0.data() + 446);
        bool hasPartitions = false;
        for (int i = 0; i < 4; ++i) {
            uint8_t type = mbrParts[i * 16 + 4];
            if (type == 0) continue;

            uint32_t startLBA = Utils::ReadLE32(mbrParts + i * 16 + 8);
            uint32_t sizeLBA  = Utils::ReadLE32(mbrParts + i * 16 + 12);
            if (sizeLBA == 0) continue;

            Partition part;
            part.index        = static_cast<int>(disk.partitions.size());
            part.physicalDiskIdx = -1;
            part.startSector  = startLBA;
            part.totalSectors = sizeLBA;
            part.sizeBytes    = static_cast<uint64_t>(sizeLBA) * disk.sectorSize;
            part.isActive     = (mbrParts[i * 16] & 0x80) != 0;
            part.fsType       = DetectFsType(h, startLBA, disk.sectorSize);
            disk.partitions.push_back(part);
            hasPartitions = true;
        }

        if (!hasPartitions) {
            // Treat the whole image as one partition
            Partition p;
            p.index        = 0;
            p.physicalDiskIdx = -1;
            p.startSector  = 0;
            p.totalSectors = disk.totalSectors;
            p.sizeBytes    = disk.sizeBytes;
            p.fsType       = DetectFsType(h, 0, disk.sectorSize);
            disk.partitions.push_back(p);
        }
    } else {
        // No MBR signature – treat entire image as one raw partition
        Partition p;
        p.index        = 0;
        p.physicalDiskIdx = -1;
        p.startSector  = 0;
        p.totalSectors = disk.totalSectors;
        p.sizeBytes    = disk.sizeBytes;
        p.fsType       = L"Unknown";
        disk.partitions.push_back(p);
    }

    CloseHandle(h);
    outDisk = std::move(disk);
    return true;
}

#endif // PHOENIX_OS_WINDOWS
